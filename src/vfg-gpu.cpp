#include "gpu-video.hpp"
#include "nvVideoEffects.h"

static void vfxok(NvCV_Status rc,const char* call) {
    if(rc!=NVCV_SUCCESS)throw std::runtime_error(std::string(call)+": VFG status "+std::to_string(rc));
}
using Img=std::unique_ptr<NvCVImage,decltype(&NvCVImage_Destroy)>;
static Img image(int w,int h,bool ten){
    // RGB10A2 and P32 in the bundled VFX runtime; the public legacy header predates them.
    const auto format=ten?static_cast<NvCVImage_PixelFormat>(13):NVCV_RGBA;
    const auto type=ten?static_cast<NvCVImage_ComponentType>(11):NVCV_U8;
    NvCVImage* i{};vfxok(NvCVImage_Create(w,h,format,type,NVCV_INTERLEAVED,NVCV_GPU,32,&i),"GPU image");return Img(i,NvCVImage_Destroy);
}
class Interpolator {
    int w,h,colourFlags;
    CUstream stream{};
    CUmodule module{};
    CUfunction unpack{},pack{};
    NvVFX_Handle effect{};
    Img prev,cur,mid;
public:
    Interpolator(int width,int height,const std::filesystem::path& ptx,const Colour& colour):w(width),h(height),colourFlags(colour.kernelFlags()),prev(image(w,h,colour.ten)),cur(image(w,h,colour.ten)),mid(image(w,h,colour.ten)){
        cuok(cuStreamCreate(&stream,CU_STREAM_NON_BLOCKING),"CUDA stream");
        // The PTX file is ASCII. Load its contents so Unicode install paths work.
        FILE* fp=_wfopen(ptx.c_str(),L"rb");if(!fp)throw std::runtime_error("Colour kernel missing");
        std::fseek(fp,0,SEEK_END);long bytes=std::ftell(fp);std::rewind(fp);std::string text(bytes,'\0');
        auto read=std::fread(text.data(),1,bytes,fp);std::fclose(fp);if(read!=size_t(bytes))throw std::runtime_error("Colour kernel read failure");
        cuok(cuModuleLoadData(&module,text.c_str()),"load colour kernels");
        cuok(cuModuleGetFunction(&unpack,module,colour.ten?"unpackRGB10":"unpack709"),"unpack kernel");cuok(cuModuleGetFunction(&pack,module,colour.ten?"packRGB10":"pack709"),"pack kernel");
        vfxok(NvVFX_CreateEffect("VideoFrameGeneration",&effect),"VFG create");
        vfxok(NvVFX_SetCudaStream(effect,NVVFX_CUDA_STREAM,stream),"bind VFG stream");
        vfxok(NvVFX_SetU32(effect,"InputWidth",w),"width");vfxok(NvVFX_SetU32(effect,"InputHeight",h),"height");
        vfxok(NvVFX_SetU32(effect,"Mode",2),"high quality");vfxok(NvVFX_SetU32(effect,"AutomaticShotChangeDetectionEnabled",1),"cut detection");
        vfxok(NvVFX_Load(effect),"load model");vfxok(NvVFX_SetImage(effect,NVVFX_OUTPUT_IMAGE,mid.get()),"output");
        vfxok(NvVFX_SetU32(effect,"FrameMultiplier",2),"multiplier");
    }
    ~Interpolator(){if(effect)NvVFX_DestroyEffect(effect);if(module)cuModuleUnload(module);if(stream)cuStreamDestroy(stream);}
    void process(AVFrame& src, AVFrame* output){
        void* inArgs[]={&src.data[0],&src.linesize[0],&src.data[1],&src.linesize[1],&cur->pixels,&cur->pitch,&w,&h,&colourFlags};
        cuok(cuLaunchKernel(unpack,(w+15)/16,(h+15)/16,1,16,16,1,0,stream,inArgs,nullptr),"NV12 to RGBA");
        if(output){
            vfxok(NvVFX_SetImage(effect,NVVFX_INPUT_IMAGE_0,prev.get()),"previous");vfxok(NvVFX_SetImage(effect,NVVFX_INPUT_IMAGE_1,cur.get()),"current");
            vfxok(NvVFX_SetU32(effect,"FrameIndex",1),"midpoint");vfxok(NvVFX_Run(effect,1),"generate");
            void* outArgs[]={&mid->pixels,&mid->pitch,&output->data[0],&output->linesize[0],&output->data[1],&output->linesize[1],&w,&h,&colourFlags};
            cuok(cuLaunchKernel(pack,(w+31)/32,(h+31)/32,1,16,16,1,0,stream,outArgs,nullptr),"RGBA to NV12");
        }
        cuok(cuStreamSynchronize(stream),"finish frame");prev.swap(cur);
    }
};
int wmain(int argc,wchar_t** argv){
    if(argc!=6 && argc!=7){std::fprintf(stderr,"Usage: vfg-gpu input output start length fps [final-cq]\n");return 2;}
    try{
        av_log_set_level(AV_LOG_ERROR);AVRational r{};avok(av_parse_video_rate(&r,utf8(argv[5]).c_str()),"frame rate");
        wchar_t own[32768];GetModuleFileNameW(nullptr,own,32768);
        const auto quality=argc==7?utf8(argv[6]):std::string{};
        const auto ptx=std::filesystem::path(own).parent_path()/L"colour.ptx";
        return renderGpu("VFG",[&](int w,int h,const Colour& colour){return Interpolator(w,h,ptx,colour);},ptx,argv[1],utf8(argv[2]).c_str(),std::stod(argv[3]),std::stod(argv[4]),r,argc==7?quality.c_str():nullptr);
    }catch(const std::exception& e){std::fprintf(stderr,"GPU path: %s\n",e.what());return 1;}
}
