#pragma once
// GPU-resident CFR video worker. Audio and final mux belong to Studio.
#include <windows.h>
#include <cuda.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/parseutils.h>
}
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

static void avok(int rc,const char* call) {
    if(rc<0){char msg[AV_ERROR_MAX_STRING_SIZE];av_strerror(rc,msg,sizeof(msg));throw std::runtime_error(std::string(call)+": "+msg);}
}
static void cuok(CUresult rc,const char* call) {
    if(rc!=CUDA_SUCCESS){const char* msg=nullptr;cuGetErrorString(rc,&msg);throw std::runtime_error(std::string(call)+": "+(msg?msg:"CUDA error"));}
}
static std::string utf8(const std::filesystem::path& path) {
    auto s=path.u8string();return std::string(s.begin(),s.end());
}
template<class T,void(*Free)(T**)>
struct Release {void operator()(T* p)const{Free(&p);}};
using Frame=std::unique_ptr<AVFrame,Release<AVFrame,av_frame_free>>;
using Packet=std::unique_ptr<AVPacket,Release<AVPacket,av_packet_free>>;
using Codec=std::unique_ptr<AVCodecContext,Release<AVCodecContext,avcodec_free_context>>;
using Buffer=std::unique_ptr<AVBufferRef,Release<AVBufferRef,av_buffer_unref>>;
using Input=std::unique_ptr<AVFormatContext,Release<AVFormatContext,avformat_close_input>>;
struct Colour {
    bool ten,hdr,full,sd,top;
    AVColorTransferCharacteristic transfer;
    bool normalize()const{return full || sd;}
    AVPixelFormat storage()const{return ten?AV_PIX_FMT_P010LE:AV_PIX_FMT_NV12;}
    int kernelFlags()const{return (top?1:0)|(hdr?0:2);}
};
static Colour inputColour(const AVCodecParameters& p){
    bool hdr=p.color_trc==AVCOL_TRC_SMPTE2084 || p.color_trc==AVCOL_TRC_ARIB_STD_B67;
    bool ten=p.format==AV_PIX_FMT_YUV420P10LE || p.format==AV_PIX_FMT_P010LE;
    bool sd=p.color_space==AVCOL_SPC_BT470BG || p.color_space==AVCOL_SPC_SMPTE170M ||
            (p.color_space==AVCOL_SPC_UNSPECIFIED && p.height<=576);
    bool full=p.color_range==AVCOL_RANGE_JPEG || p.format==AV_PIX_FMT_YUVJ420P;
    if(!ten && p.format!=AV_PIX_FMT_YUV420P && p.format!=AV_PIX_FMT_YUVJ420P && p.format!=AV_PIX_FMT_NV12)
        throw std::runtime_error("Unsupported chroma format or bit depth");
    if(hdr && (!ten || (p.color_range!=AVCOL_RANGE_MPEG && p.color_range!=AVCOL_RANGE_JPEG) ||
               p.color_space!=AVCOL_SPC_BT2020_NCL || p.color_primaries!=AVCOL_PRI_BT2020))
        throw std::runtime_error("HDR input requires normalization");
    if(!hdr && ((p.color_primaries!=AVCOL_PRI_UNSPECIFIED && p.color_primaries!=AVCOL_PRI_BT709) ||
        (p.color_space!=AVCOL_SPC_UNSPECIFIED && p.color_space!=AVCOL_SPC_BT709 && !sd) ||
        (p.color_trc!=AVCOL_TRC_UNSPECIFIED && p.color_trc!=AVCOL_TRC_BT709 && p.color_trc!=AVCOL_TRC_SMPTE170M)))
        throw std::runtime_error("SDR input requires colour management");
    return {ten,hdr,full,sd,p.chroma_location==AVCHROMA_LOC_TOPLEFT,hdr?p.color_trc:AVCOL_TRC_BT709};
}
class YuvNormalizer {
    CUmodule module{};
    CUfunction convert{};
public:
    ~YuvNormalizer(){if(module)cuModuleUnload(module);}
    void open(const std::filesystem::path& ptx){
        std::ifstream file(ptx,std::ios::binary);
        if(!file)throw std::runtime_error("Colour kernel missing");
        std::string code{std::istreambuf_iterator<char>(file),{}};
        cuok(cuModuleLoadData(&module,code.c_str()),"load YUV normalization");
        cuok(cuModuleGetFunction(&convert,module,"normalizeYUV"),"find YUV normalization");
    }
    void run(AVFrame& src,AVFrame& dst,const Colour& colour,CUstream stream){
        int full=colour.full,sd=colour.sd,ten=colour.ten,top=colour.top;
        void* args[]={&src.data[0],&src.linesize[0],&src.data[1],&src.linesize[1],
            &dst.data[0],&dst.linesize[0],&dst.data[1],&dst.linesize[1],&src.width,&src.height,&full,&sd,&ten,&top};
        cuok(cuLaunchKernel(convert,(src.width+15)/16,(src.height+15)/16,1,16,16,1,0,stream,args,nullptr),"normalize YUV");
        cuok(cuStreamSynchronize(stream),"normalized frame ready");
    }
};
static AVPixelFormat requireCuda(AVCodecContext*,const AVPixelFormat* choices) {
    for(;*choices!=AV_PIX_FMT_NONE;++choices)if(*choices==AV_PIX_FMT_CUDA)return *choices;
    return AV_PIX_FMT_NONE;
}
static bool supportsCuda(const AVCodec* codec){
    if(!codec)return false;
    for(int i=0;;i++){
        const AVCodecHWConfig* config=avcodec_get_hw_config(codec,i);
        if(!config)return false;
        if(config->device_type==AV_HWDEVICE_TYPE_CUDA && config->pix_fmt==AV_PIX_FMT_CUDA &&
           (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))return true;
    }
}
static const AVCodec* cudaDecoder(AVCodecID id){
    if(id==AV_CODEC_ID_AV1){
        if(auto* cuvid=avcodec_find_decoder_by_name("av1_cuvid"))return cuvid;
    }
    auto* preferred=avcodec_find_decoder(id);
    if(supportsCuda(preferred))return preferred;
    void* cursor=nullptr;
    while(const AVCodec* codec=av_codec_iterate(&cursor)){
        if(codec->id==id && av_codec_is_decoder(codec) && supportsCuda(codec))return codec;
    }
    throw std::runtime_error("No NVDEC decoder for this codec");
}
class Writer {
public:
    Codec enc;
    AVFormatContext* mux{};
    AVStream* video{};
    Packet packet{av_packet_alloc()};
    int64_t written=0;
    ~Writer(){if(mux){avio_closep(&mux->pb);avformat_free_context(mux);}}
    void open(const char* filename,int w,int h,AVRational fps,AVBufferRef* frames,const char* finalCq,const Colour& colour,const AVFrame& first){
        avok(avformat_alloc_output_context2(&mux,nullptr,"mp4",filename),"MP4 output");
        const auto* codec=avcodec_find_encoder_by_name("hevc_nvenc");
        if(!codec)throw std::runtime_error("NVENC unavailable");
        enc.reset(avcodec_alloc_context3(codec));
        enc->width=w;enc->height=h;enc->pix_fmt=AV_PIX_FMT_CUDA;enc->sw_pix_fmt=colour.storage();
        enc->hw_frames_ctx=av_buffer_ref(frames);enc->framerate=fps;enc->time_base=av_inv_q(fps);
        enc->colorspace=colour.hdr?AVCOL_SPC_BT2020_NCL:AVCOL_SPC_BT709;enc->color_trc=colour.transfer;
        enc->color_primaries=colour.hdr?AVCOL_PRI_BT2020:AVCOL_PRI_BT709;
        if(colour.hdr) for(auto type:{AV_FRAME_DATA_MASTERING_DISPLAY_METADATA,AV_FRAME_DATA_CONTENT_LIGHT_LEVEL})
            if(auto* data=av_frame_get_side_data(&first,type))
                avok(av_frame_side_data_clone(&enc->decoded_side_data,&enc->nb_decoded_side_data,data,0),"copy HDR metadata");
        enc->color_range=AVCOL_RANGE_MPEG;enc->chroma_sample_location=colour.top?AVCHROMA_LOC_TOPLEFT:AVCHROMA_LOC_LEFT;
        enc->max_b_frames=0;enc->gop_size=250;enc->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary* opts=nullptr;
        av_dict_set(&opts,"preset",finalCq?"p5":"p3",0);
        av_dict_set(&opts,"rc",finalCq?"vbr":"constqp",0);
        av_dict_set(&opts,finalCq?"cq":"qp",finalCq?finalCq:"8",0);
        if(finalCq){enc->bit_rate=0;enc->rc_max_rate=240000000;}
        auto result=avcodec_open2(enc.get(),codec,&opts);av_dict_free(&opts);avok(result,"open NVENC");
        video=avformat_new_stream(mux,nullptr);if(!video)throw std::bad_alloc();
        video->time_base=enc->time_base;video->avg_frame_rate=fps;
        avok(avcodec_parameters_from_context(video->codecpar,enc.get()),"stream parameters");
        avok(avio_open(&mux->pb,filename,AVIO_FLAG_WRITE),"open intermediate");avok(avformat_write_header(mux,nullptr),"MP4 header");
    }
    void push(AVFrame* f){
        if(f){f->pts=written++;f->duration=1;f->pict_type=AV_PICTURE_TYPE_NONE;f->flags&=~AV_FRAME_FLAG_KEY;}
        avok(avcodec_send_frame(enc.get(),f),"encode frame");
        for(;;){int rc=avcodec_receive_packet(enc.get(),packet.get());if(rc==AVERROR(EAGAIN)||rc==AVERROR_EOF)break;
            avok(rc,"read NVENC packet");packet->duration=1;av_packet_rescale_ts(packet.get(),enc->time_base,video->time_base);
            packet->stream_index=video->index;avok(av_interleaved_write_frame(mux,packet.get()),"write packet");av_packet_unref(packet.get());}
    }
};
template<class Factory>
static int renderGpu(const char* engine,Factory makeInterpolator,const std::filesystem::path& ptx,const std::filesystem::path& file,const char* dst,double start,double length,AVRational rate,const char* finalCq){
    AVFormatContext* raw=nullptr;avok(avformat_open_input(&raw,utf8(file).c_str(),nullptr,nullptr),"open input");Input input(raw);
    avok(avformat_find_stream_info(raw,nullptr),"inspect video");
    int index=-1;
    for(unsigned i=0;i<raw->nb_streams;i++)if(raw->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO && !(raw->streams[i]->disposition&AV_DISPOSITION_ATTACHED_PIC)){index=i;break;}
    if(index<0)throw std::runtime_error("No video");
    auto* s=raw->streams[index];const auto* codec=cudaDecoder(s->codecpar->codec_id);
    Codec dec(avcodec_alloc_context3(codec));avok(avcodec_parameters_to_context(dec.get(),s->codecpar),"decoder parameters");
    AVBufferRef* dev=nullptr;
    avok(av_hwdevice_ctx_create(&dev,AV_HWDEVICE_TYPE_CUDA,"0",nullptr,AV_CUDA_USE_PRIMARY_CONTEXT),"CUDA primary context");Buffer device(dev);
    auto* cuda=static_cast<AVCUDADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(dev->data)->hwctx);
    cuok(cuCtxSetCurrent(cuda->cuda_ctx),"select GPU");
    dec->hw_device_ctx=av_buffer_ref(dev);dec->get_format=requireCuda;dec->pkt_timebase=s->time_base;dec->extra_hw_frames=4;
    avok(avcodec_open2(dec.get(),codec,nullptr),"open NVDEC");
    const int w=dec->width,h=dec->height;
    if(w<64||h<64||(w|h)%2)throw std::runtime_error("Unsupported dimensions");
    const Colour colour=inputColour(*s->codecpar);
    YuvNormalizer normalizer;if(colour.normalize())normalizer.open(ptx);
    Buffer frames(av_hwframe_ctx_alloc(dev));if(!frames)throw std::bad_alloc();auto* hw=reinterpret_cast<AVHWFramesContext*>(frames->data);
    hw->format=AV_PIX_FMT_CUDA;hw->sw_format=colour.storage();hw->width=w;hw->height=h;hw->initial_pool_size=8;
    avok(av_hwframe_ctx_init(frames.get()),"encoder surface pool");
    Writer writer;auto interpolator=makeInterpolator(w,h,colour);
    Frame normalized(av_frame_alloc());
    Frame current(av_frame_alloc()),last(av_frame_alloc()),middle(av_frame_alloc());Packet packet(av_packet_alloc());
    int64_t count=0;
    double firstTime=-1;const double fps=av_q2d(rate);const int64_t origin=s->start_time==AV_NOPTS_VALUE?0:s->start_time;
    if(start>0){int64_t ts=origin+int64_t(std::floor(start/av_q2d(s->time_base)));avok(av_seek_frame(raw,index,ts,AVSEEK_FLAG_BACKWARD),"seek");avcodec_flush_buffers(dec.get());}
    bool ended=false;
    auto receive=[&]{
        while(!ended){
            int rc=avcodec_receive_frame(dec.get(),current.get());if(rc==AVERROR(EAGAIN)||rc==AVERROR_EOF)return;avok(rc,"decode");
            if(current->best_effort_timestamp==AV_NOPTS_VALUE || !current->hw_frames_ctx || current->format!=AV_PIX_FMT_CUDA ||
               reinterpret_cast<AVHWFramesContext*>(current->hw_frames_ctx->data)->sw_format!=colour.storage() ||
               current->width!=w||current->height!=h||(current->flags&AV_FRAME_FLAG_INTERLACED) ||
               (current->color_range!=AVCOL_RANGE_UNSPECIFIED && (current->color_range==AVCOL_RANGE_JPEG)!=colour.full) ||
               (current->colorspace!=s->codecpar->color_space && current->colorspace!=AVCOL_SPC_UNSPECIFIED) ||
               (current->color_trc!=s->codecpar->color_trc && current->color_trc!=AVCOL_TRC_UNSPECIFIED) ||
               (current->color_primaries!=s->codecpar->color_primaries && current->color_primaries!=AVCOL_PRI_UNSPECIFIED) ||
               (current->chroma_location!=(colour.top?AVCHROMA_LOC_TOPLEFT:AVCHROMA_LOC_LEFT) && current->chroma_location!=AVCHROMA_LOC_UNSPECIFIED))
                throw std::runtime_error("Decoded surface, timestamps or colour tags require compatibility mode");
            double t=(current->best_effort_timestamp-origin)*av_q2d(s->time_base);
            if(t<start-1e-7){av_frame_unref(current.get());continue;}
            if(length>0 && t>=start+length-1e-7){ended=true;return;}
            if(!count){firstTime=t;writer.open(dst,w,h,av_mul_q(rate,{2,1}),frames.get(),finalCq,colour,*current);}
            if(std::abs((t-firstTime)*fps-count)>.12)throw std::runtime_error("Variable frame timestamps");
            // The decoder and interpolator share the primary context. Finish NVDEC's copy before consuming its CUDA surface.
            cuok(cuStreamSynchronize(cuda->stream),"decoded surface ready");
            if(colour.normalize()){
                avok(av_hwframe_get_buffer(frames.get(),normalized.get(),0),"normalized surface");
                normalizer.run(*current,*normalized,colour,cuda->stream);
                av_frame_unref(current.get());av_frame_move_ref(current.get(),normalized.get());
            }
            if(count)avok(av_hwframe_get_buffer(frames.get(),middle.get(),0),"new NVENC surface");
            interpolator.process(*current,count?middle.get():nullptr);
            if(count){writer.push(middle.get());av_frame_unref(middle.get());}
            // Per-frame HDR10+ metadata cannot describe the interpolated frames.
            av_frame_remove_side_data(current.get(),AV_FRAME_DATA_DYNAMIC_HDR_PLUS);
            writer.push(current.get());
            av_frame_unref(last.get());av_frame_move_ref(last.get(),current.get());
            if(++count%60==0)std::fprintf(stderr,"%s_FRAMES %lld\n",engine,(long long)count);
        }
    };
    int rc=0;
    while(!ended && (rc=av_read_frame(raw,packet.get()))>=0){
        if(packet->stream_index==index){avok(avcodec_send_packet(dec.get(),packet.get()),"send encoded packet");receive();}
        av_packet_unref(packet.get());
    }
    if(!ended){if(rc!=AVERROR_EOF)avok(rc,"read input");avok(avcodec_send_packet(dec.get(),nullptr),"flush NVDEC");receive();}
    if(!count)throw std::runtime_error("No frames in interval");
    writer.push(last.get());writer.push(nullptr);avok(av_write_trailer(writer.mux),"finish MP4");
    std::fprintf(stderr,"%s_GPU_DONE %lld -> %lld\n",engine,(long long)count,(long long)writer.written);return 0;
}
