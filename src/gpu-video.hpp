#pragma once
// GPU-resident SDR BT.709 CFR video worker. Audio and final mux belong to Studio.
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
static AVPixelFormat requireCuda(AVCodecContext*,const AVPixelFormat* choices) {
    for(;*choices!=AV_PIX_FMT_NONE;++choices)if(*choices==AV_PIX_FMT_CUDA)return *choices;
    return AV_PIX_FMT_NONE;
}
class Writer {
public:
    Codec enc;
    AVFormatContext* mux{};
    AVStream* video{};
    Packet packet{av_packet_alloc()};
    int64_t written=0;
    ~Writer(){if(mux){avio_closep(&mux->pb);avformat_free_context(mux);}}
    void open(const char* filename,int w,int h,AVRational fps,AVBufferRef* frames,const char* finalCq){
        avok(avformat_alloc_output_context2(&mux,nullptr,"mp4",filename),"MP4 output");
        const auto* codec=avcodec_find_encoder_by_name("hevc_nvenc");
        if(!codec)throw std::runtime_error("NVENC unavailable");
        enc.reset(avcodec_alloc_context3(codec));
        enc->width=w;enc->height=h;enc->pix_fmt=AV_PIX_FMT_CUDA;enc->sw_pix_fmt=AV_PIX_FMT_NV12;
        enc->hw_frames_ctx=av_buffer_ref(frames);enc->framerate=fps;enc->time_base=av_inv_q(fps);
        enc->colorspace=AVCOL_SPC_BT709;enc->color_trc=AVCOL_TRC_BT709;enc->color_primaries=AVCOL_PRI_BT709;
        enc->color_range=AVCOL_RANGE_MPEG;enc->chroma_sample_location=AVCHROMA_LOC_LEFT;
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
static int renderGpu(const char* engine,Factory makeInterpolator,const std::filesystem::path& file,const char* dst,double start,double length,AVRational rate,const char* finalCq){
    AVFormatContext* raw=nullptr;avok(avformat_open_input(&raw,utf8(file).c_str(),nullptr,nullptr),"open input");Input input(raw);
    avok(avformat_find_stream_info(raw,nullptr),"inspect video");
    int index=-1;
    for(unsigned i=0;i<raw->nb_streams;i++)if(raw->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO && !(raw->streams[i]->disposition&AV_DISPOSITION_ATTACHED_PIC)){index=i;break;}
    if(index<0)throw std::runtime_error("No video");
    auto* s=raw->streams[index];const auto* codec=avcodec_find_decoder(s->codecpar->codec_id);
    if(!codec)throw std::runtime_error("No decoder");
    Codec dec(avcodec_alloc_context3(codec));avok(avcodec_parameters_to_context(dec.get(),s->codecpar),"decoder parameters");
    AVBufferRef* dev=nullptr;
    avok(av_hwdevice_ctx_create(&dev,AV_HWDEVICE_TYPE_CUDA,"0",nullptr,AV_CUDA_USE_PRIMARY_CONTEXT),"CUDA primary context");Buffer device(dev);
    auto* cuda=static_cast<AVCUDADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(dev->data)->hwctx);
    cuok(cuCtxSetCurrent(cuda->cuda_ctx),"select GPU");
    dec->hw_device_ctx=av_buffer_ref(dev);dec->get_format=requireCuda;dec->pkt_timebase=s->time_base;dec->extra_hw_frames=12;
    avok(avcodec_open2(dec.get(),codec,nullptr),"open NVDEC");
    const int w=dec->width,h=dec->height;
    if(w<64||h<64||(w|h)%2)throw std::runtime_error("Unsupported dimensions");
    Buffer frames(av_hwframe_ctx_alloc(dev));if(!frames)throw std::bad_alloc();auto* hw=reinterpret_cast<AVHWFramesContext*>(frames->data);
    hw->format=AV_PIX_FMT_CUDA;hw->sw_format=AV_PIX_FMT_NV12;hw->width=w;hw->height=h;hw->initial_pool_size=8;
    avok(av_hwframe_ctx_init(frames.get()),"encoder surface pool");
    Writer writer;writer.open(dst,w,h,av_mul_q(rate,{2,1}),frames.get(),finalCq);auto interpolator=makeInterpolator(w,h);
    Frame current(av_frame_alloc()),last(av_frame_alloc()),middle(av_frame_alloc());Packet packet(av_packet_alloc());
    int64_t count=0;
    double firstTime=-1;const double fps=av_q2d(rate);const int64_t origin=s->start_time==AV_NOPTS_VALUE?0:s->start_time;
    if(start>0){int64_t ts=origin+int64_t(std::floor(start/av_q2d(s->time_base)));avok(av_seek_frame(raw,index,ts,AVSEEK_FLAG_BACKWARD),"seek");avcodec_flush_buffers(dec.get());}
    bool ended=false;
    auto receive=[&]{
        while(!ended){
            int rc=avcodec_receive_frame(dec.get(),current.get());if(rc==AVERROR(EAGAIN)||rc==AVERROR_EOF)return;avok(rc,"decode");
            if(current->best_effort_timestamp==AV_NOPTS_VALUE || !current->hw_frames_ctx || current->format!=AV_PIX_FMT_CUDA ||
               reinterpret_cast<AVHWFramesContext*>(current->hw_frames_ctx->data)->sw_format!=AV_PIX_FMT_NV12 ||
               current->width!=w||current->height!=h||(current->flags&AV_FRAME_FLAG_INTERLACED) ||
               current->color_range==AVCOL_RANGE_JPEG ||
               (current->colorspace!=AVCOL_SPC_BT709 && current->colorspace!=AVCOL_SPC_UNSPECIFIED) ||
               (current->color_trc!=AVCOL_TRC_BT709 && current->color_trc!=AVCOL_TRC_UNSPECIFIED) ||
               (current->color_primaries!=AVCOL_PRI_BT709 && current->color_primaries!=AVCOL_PRI_UNSPECIFIED) ||
               (current->chroma_location!=AVCHROMA_LOC_LEFT && current->chroma_location!=AVCHROMA_LOC_UNSPECIFIED))
                throw std::runtime_error("Input requires compatibility path");
            double t=(current->best_effort_timestamp-origin)*av_q2d(s->time_base);
            if(t<start-1e-7){av_frame_unref(current.get());continue;}
            if(length>0 && t>=start+length-1e-7){ended=true;return;}
            if(!count)firstTime=t;
            if(std::abs((t-firstTime)*fps-count)>.12)throw std::runtime_error("Variable frame timestamps");
            // The decoder and interpolator share the primary context. Finish NVDEC's copy before consuming its CUDA surface.
            cuok(cuStreamSynchronize(cuda->stream),"decoded surface ready");
            if(count)avok(av_hwframe_get_buffer(frames.get(),middle.get(),0),"new NVENC surface");
            interpolator.process(*current,count?middle.get():nullptr);
            if(count){writer.push(middle.get());av_frame_unref(middle.get());}
            // NVENC retains each AVFrame reference it needs. Preserve the original NV12 pixels.
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
