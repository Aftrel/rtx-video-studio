// File-to-file adapter for NVIDIA Maxine Video Frame Generation.
// Input and output are RGBA8 pipes. The GPU owns the midpoint model.
#include <windows.h>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <memory>
#include <io.h>
#include <fcntl.h>
#include "nvVideoEffects.h"

static void check(NvCV_Status result, const char* stage) {
    if (result != NVCV_SUCCESS)
        throw std::runtime_error(std::string(stage)+" failed ("+std::to_string(result)+")");
}
using Image = std::unique_ptr<NvCVImage, decltype(&NvCVImage_Destroy)>;
static Image allocate(unsigned w, unsigned h) {
    NvCVImage* raw = nullptr;
    check(NvCVImage_Create(w,h,NVCV_RGBA,NVCV_U8,NVCV_INTERLEAVED,NVCV_CPU_PINNED,32,&raw),"allocate image");
    return Image(raw,NvCVImage_Destroy);
}
static bool readImage(NvCVImage& img) {
    auto row=static_cast<unsigned char*>(img.pixels);
    for (unsigned y=0;y<img.height;++y,row+=img.pitch) {
        size_t n=std::fread(row,1,img.width*4,stdin);
        if(y==0 && n==0 && std::feof(stdin))return false;
        if(n!=img.width*4)throw std::runtime_error("Incomplete input frame");
    }
    return true;
}
static void writeImage(FILE* out,const NvCVImage& img) {
    auto row=static_cast<const unsigned char*>(img.pixels);
    for(unsigned y=0;y<img.height;++y,row+=img.pitch)
        if(std::fwrite(row,1,img.width*4,out)!=img.width*4)throw std::runtime_error("Encoder pipe closed");
}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc!=3)throw std::runtime_error("Usage: nvidia-vfg WIDTH HEIGHT");
        unsigned w=std::stoul(argv[1]),h=std::stoul(argv[2]);
        if(w<64||h<64||w>8192||h>8192)throw std::runtime_error("Unsupported dimensions");
        _setmode(_fileno(stdin),_O_BINARY);
        FILE* out=_fdopen(_dup(_fileno(stdout)),"wb");
        if(!out)throw std::runtime_error("Output stream unavailable");
        _setmode(_fileno(out),_O_BINARY);
        _dup2(_fileno(stderr),_fileno(stdout));
        CUstream stream{};
        check(NvVFX_CudaStreamCreate(&stream),"CUDA stream");
        NvVFX_Handle handle{};
        check(NvVFX_CreateEffect("VideoFrameGeneration",&handle),"CreateEffect");
        std::unique_ptr<NvVFX_Object,decltype(&NvVFX_DestroyEffect)> effect(handle,NvVFX_DestroyEffect);
        auto prev=allocate(w,h),curr=allocate(w,h),mid=allocate(w,h);
        check(NvVFX_SetCudaStream(handle,NVVFX_CUDA_STREAM,stream),"bind stream");
        check(NvVFX_SetU32(handle,"InputWidth",w),"width");
        check(NvVFX_SetU32(handle,"InputHeight",h),"height");
        check(NvVFX_SetU32(handle,"Mode",2),"high quality");
        check(NvVFX_SetU32(handle,"AutomaticShotChangeDetectionEnabled",1),"cut detection");
        check(NvVFX_Load(handle),"load VFG");
        check(NvVFX_SetImage(handle,NVVFX_OUTPUT_IMAGE,mid.get()),"output");
        check(NvVFX_SetU32(handle,"FrameMultiplier",2),"multiplier");
        if(!readImage(*prev))throw std::runtime_error("Decoder produced no frames");
        size_t count=1;
        writeImage(out,*prev);
        while(readImage(*curr)) {
            check(NvVFX_SetImage(handle,NVVFX_INPUT_IMAGE_0,prev.get()),"previous image");
            check(NvVFX_SetImage(handle,NVVFX_INPUT_IMAGE_1,curr.get()),"current image");
            check(NvVFX_SetU32(handle,"FrameIndex",1),"midpoint");
            check(NvVFX_Run(handle,0),"generate frame");
            writeImage(out,*mid);writeImage(out,*curr);prev.swap(curr);
            if(++count%60==0)std::fprintf(stderr,"VFG_FRAMES %zu\n",count);
        }
        writeImage(out,*prev);
        if(std::fclose(out)!=0)throw std::runtime_error("Output write failed");
        effect.reset();
        check(NvVFX_CudaStreamDestroy(stream),"release stream");
        std::fprintf(stderr,"VFG_DONE %zu -> %zu\n",count,count*2);
        return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"NVIDIA VFG: %s\n",e.what());return 1;}
}
