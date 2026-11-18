// SPDX-License-Identifier: GPL-3.0-or-later
// Included in the LSFG backend translation unit by build_worker.py.
#include <fstream>
#include <cstring>
#include "gpu-video.hpp"

namespace lsfgvk::backend {
namespace {
void checkVk(VkResult result,const char* action) {
    if(result!=VK_SUCCESS) throw ls::vulkan_error(result,action);
}
struct SharedHandle {
    HANDLE value{};
    ~SharedHandle(){if(value) CloseHandle(value);}
};

// One device-local RGBA buffer. CUDA converts NV12 in place on either side of
// LSFG, and Vulkan copies between the buffer and its private tiled images.
struct Exchange {
    const vk::Vulkan& gpu;
    VkBuffer buffer{};
    VkDeviceMemory allocation{};
    CUexternalMemory memory{};
    CUdeviceptr pixels{};
    CUexternalSemaphore semaphore{};
    CUmodule module{};
    CUfunction unpack{},pack{};
    CUstream stream{};
    explicit Exchange(const vk::Vulkan& device):gpu(device){}
    Exchange(const Exchange&)=delete;
    ~Exchange(){
        if(stream) cuStreamSynchronize(stream);
        gpu.df().DeviceWaitIdle(gpu.dev());
        if(module) cuModuleUnload(module);
        if(semaphore) cuDestroyExternalSemaphore(semaphore);
        if(pixels) cuMemFree(pixels);
        if(memory) cuDestroyExternalMemory(memory);
        if(buffer) gpu.df().DestroyBuffer(gpu.dev(),buffer,nullptr);
        if(allocation) gpu.df().FreeMemory(gpu.dev(),allocation,nullptr);
        if(stream) cuStreamDestroy(stream);
    }
    void open(size_t bytes,HANDLE timeline,const std::filesystem::path& ptx,bool ten){
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,&id};
        gpu.fi().GetPhysicalDeviceProperties2(gpu.physdev(),&properties);
        CUdevice device{};CUuuid uuid{};
        cuok(cuCtxGetDevice(&device),"current CUDA adapter");
        cuok(cuDeviceGetUuid(&uuid,device),"CUDA adapter UUID");
        if(std::memcmp(uuid.bytes,id.deviceUUID,VK_UUID_SIZE)!=0)
            throw std::runtime_error("LSFG and NVDEC selected different adapters");
        const VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,nullptr,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT};
        const VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,&external,0,bytes,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_SHARING_MODE_EXCLUSIVE};
        checkVk(gpu.df().CreateBuffer(gpu.dev(),&info,nullptr,&buffer),"create shared buffer");
        VkMemoryRequirements req{};
        gpu.df().GetBufferMemoryRequirements(gpu.dev(),buffer,&req);
        auto type=gpu.findMemoryTypeIndex(req.memoryTypeBits,false);
        if(!type) throw std::runtime_error("No device-local memory for LSFG");
        const VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,nullptr,VK_NULL_HANDLE,buffer};
        const VkExportMemoryAllocateInfo exported{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,&dedicated,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT};
        const VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,&exported,req.size,*type};
        checkVk(gpu.df().AllocateMemory(gpu.dev(),&alloc,nullptr,&allocation),"allocate shared memory");
        checkVk(gpu.df().BindBufferMemory(gpu.dev(),buffer,allocation,0),"bind shared memory");
        SharedHandle handle;
        const VkMemoryGetWin32HandleInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,nullptr,allocation,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT};
        checkVk(gpu.df().GetMemoryWin32HandleKHR(gpu.dev(),&get,&handle.value),"export shared memory");
        CUDA_EXTERNAL_MEMORY_HANDLE_DESC desc{};
        desc.type=CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;desc.handle.win32.handle=handle.value;
        desc.size=req.size;desc.flags=CUDA_EXTERNAL_MEMORY_DEDICATED;
        cuok(cuImportExternalMemory(&memory,&desc),"import Vulkan memory");
        CUDA_EXTERNAL_MEMORY_BUFFER_DESC view{};view.size=bytes;
        cuok(cuExternalMemoryGetMappedBuffer(&pixels,memory,&view),"map shared GPU buffer");
        CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC sem{};
        sem.type=CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TIMELINE_SEMAPHORE_WIN32;sem.handle.win32.handle=timeline;
        cuok(cuImportExternalSemaphore(&semaphore,&sem),"import Vulkan timeline");
        cuok(cuStreamCreate(&stream,CU_STREAM_NON_BLOCKING),"LSFG CUDA stream");
        std::ifstream file(ptx,std::ios::binary);
        if(!file) throw std::runtime_error("Colour kernel missing");
        std::string code{std::istreambuf_iterator<char>(file),{}};
        cuok(cuModuleLoadData(&module,code.c_str()),"load colour kernels");
        cuok(cuModuleGetFunction(&unpack,module,ten?"unpackHalf":"unpack709"),"unpack kernel");
        cuok(cuModuleGetFunction(&pack,module,ten?"packHalf":"pack709"),"pack kernel");
    }
    void wait(uint64_t value){
        CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS params{};params.params.fence.value=value;
        cuok(cuWaitExternalSemaphoresAsync(&semaphore,&params,1,stream),"wait for Vulkan");
    }
    void signal(uint64_t value){
        CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS params{};params.params.fence.value=value;
        cuok(cuSignalExternalSemaphoresAsync(&semaphore,&params,1,stream),"release to Vulkan");
    }
};
std::string cudaPciAddress(){
    CUdevice device{};int bus{},slot{};
    cuok(cuCtxGetDevice(&device),"current CUDA adapter");
    cuok(cuDeviceGetAttribute(&bus,CU_DEVICE_ATTRIBUTE_PCI_BUS_ID,device),"CUDA PCI bus");
    cuok(cuDeviceGetAttribute(&slot,CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID,device),"CUDA PCI slot");
    return std::to_string(bus)+":"+std::to_string(slot)+".0";
}
}

class GpuFrames {
    int width,height,pitch,colourFlags;
    Instance instance;
    Context& context;
    const vk::Vulkan& gpu;
    SharedHandle timelineHandle;
    vk::TimelineSemaphore timeline;
    vk::CommandBuffer upload,download;
    Exchange shared;
    PFN_vkCmdCopyImageToBuffer copyBack;
    uint64_t value=1;
    VkBufferImageCopy region{};
    void ownership(const vk::CommandBuffer& cmd,bool release,VkAccessFlags access){
        const uint32_t local=gpu.familyIndex();
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,nullptr,
            release?access:0,release?0:access,
            release?local:VK_QUEUE_FAMILY_EXTERNAL,release?VK_QUEUE_FAMILY_EXTERNAL:local,
            shared.buffer,0,VK_WHOLE_SIZE};
        gpu.df().CmdPipelineBarrier(cmd.native(),
            release?VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            release?VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,nullptr,1,&barrier,0,nullptr);
    }
public:
    GpuFrames(int w,int h,const std::filesystem::path& dll,const std::filesystem::path& ptx,const Colour& colour)
        :width(w),height(h),pitch(w*(colour.ten?8:4)),colourFlags(colour.kernelFlags()),
        instance([pci=cudaPciAddress()](const std::string&,auto,const auto& address){return address==pci;},dll,true),
        context(instance.openContext({nullptr,nullptr},{nullptr},nullptr,w,h,colour.ten,1.0f,false)),
        gpu(context.ctx.vk.get()),timeline(gpu,0,std::nullopt,&timelineHandle.value),
        upload(gpu),download(gpu),shared(gpu),
        copyBack(reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(gpu.fi().GetDeviceProcAddr(gpu.dev(),"vkCmdCopyImageToBuffer"))){
        if(!copyBack) throw std::runtime_error("Vulkan image transfer unavailable");
        shared.open(size_t(pitch)*height,timelineHandle.value,ptx,colour.ten);
        region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        region.imageExtent={uint32_t(w),uint32_t(h),1};
        upload.begin(gpu);ownership(upload,true,0);upload.end(gpu);
        upload.submit(gpu);
        timeline.signal(gpu,value);
    }
    void process(AVFrame& src,AVFrame* mid){
        shared.wait(value);
        void* inArgs[]={&src.data[0],&src.linesize[0],&src.data[1],&src.linesize[1],&shared.pixels,&pitch,&width,&height,&colourFlags};
        cuok(cuLaunchKernel(shared.unpack,(width+15)/16,(height+15)/16,1,16,16,1,0,shared.stream,inArgs,nullptr),"NV12 to shared RGBA");
        shared.signal(++value);

        upload.begin(gpu);ownership(upload,false,VK_ACCESS_TRANSFER_READ_BIT);
        auto writeImage=[&](const vk::Image& image){
            gpu.df().CmdCopyBufferToImage(upload.native(),shared.buffer,image.handle(),VK_IMAGE_LAYOUT_GENERAL,1,&region);
        };
        writeImage(context.fidx%2?context.sourceImages.second:context.sourceImages.first);
        if(!context.fidx) writeImage(context.sourceImages.second);
        upload.end(gpu);
        upload.submit(gpu,{},timeline.handle(),value,{},context.syncSemaphore.handle(),context.idx);
        context.scheduleFrames();

        download.begin(gpu);
        if(mid) copyBack(download.native(),context.destImages.front().handle(),VK_IMAGE_LAYOUT_GENERAL,shared.buffer,1,&region);
        ownership(download,true,mid?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_TRANSFER_READ_BIT);
        download.end(gpu);
        download.submit(gpu,{},context.syncSemaphore.handle(),context.idx-1,{},timeline.handle(),++value);
        shared.wait(value);
        if(mid){
            void* outArgs[]={&shared.pixels,&pitch,&mid->data[0],&mid->linesize[0],&mid->data[1],&mid->linesize[1],&width,&height,&colourFlags};
            cuok(cuLaunchKernel(shared.pack,(width+31)/32,(height+31)/32,1,16,16,1,0,shared.stream,outArgs,nullptr),"shared RGBA to NV12");
        }
        cuok(cuStreamSynchronize(shared.stream),"finish LSFG frame");
    }
};
}

int wmain(int argc,wchar_t** argv){
    if(argc!=7 && argc!=8){std::fprintf(stderr,"Usage: lsfg-gpu input output start length fps Lossless.dll [final-cq]\n");return 2;}
    try{
        av_log_set_level(AV_LOG_ERROR);
        AVRational rate{};avok(av_parse_video_rate(&rate,utf8(argv[5]).c_str()),"frame rate");
        wchar_t own[32768];GetModuleFileNameW(nullptr,own,32768);
        const auto ptx=std::filesystem::path(own).parent_path()/L"colour.ptx";
        const auto quality=argc==8?utf8(argv[7]):std::string{};
        return renderGpu("LSFG",[&](int w,int h,const Colour& colour){return lsfgvk::backend::GpuFrames(w,h,argv[6],ptx,colour);},ptx,
            argv[1],utf8(argv[2]).c_str(),std::stod(argv[3]),std::stod(argv[4]),rate,argc==8?quality.c_str():nullptr);
    }catch(const std::exception& e){std::fprintf(stderr,"LSFG GPU path: %s\n",e.what());return 1;}
}
