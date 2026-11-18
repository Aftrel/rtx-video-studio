// SPDX-License-Identifier: GPL-3.0-or-later
// Offline RGBA input/output on the same Vulkan queue used for optical flow.
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <io.h>

namespace {
void writeFrame(const std::vector<uint8_t>& frame) {
    if (std::fwrite(frame.data(), 1, frame.size(), stdout) != frame.size())
        throw std::runtime_error("Encoder closed the output pipe");
}
bool readFrame(std::vector<uint8_t>& frame) {
    const auto bytes = std::fread(frame.data(), 1, frame.size(), stdin);
    if (bytes == 0 && std::feof(stdin)) return false;
    if (bytes != frame.size()) throw std::runtime_error("Incomplete input frame");
    return true;
}
class MappedFrame {
public:
    MappedFrame(const vk::Vulkan& device, const std::vector<uint8_t>& initial)
        : gpu(device), buffer(device, initial.data(), initial.size(),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT) {
        if (gpu.df().MapMemory(gpu.dev(), buffer.allocation(), 0, initial.size(), 0, &bytes) != VK_SUCCESS)
            throw std::runtime_error("Cannot map staging buffer");
    }
    ~MappedFrame() { gpu.df().UnmapMemory(gpu.dev(), buffer.allocation()); }
    const vk::Vulkan& gpu;
    vk::Buffer buffer;
    void* bytes{};
};
}

void ContextImpl::streamVideo() {
    const auto& gpu = ctx.vk.get();
    const auto extent = ctx.sourceExtent;
    std::vector<uint8_t> frame(size_t(extent.width) * extent.height * 4);
    if (!readFrame(frame)) throw std::runtime_error("Decoder produced no frames");
    MappedFrame staging(gpu, frame);
    vk::CommandBuffer upload(gpu), download(gpu);
    const auto image = destImages.front().handle();
    const auto copy = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(
        gpu.fi().GetDeviceProcAddr(gpu.dev(), "vkCmdCopyImageToBuffer"));
    if (!copy) throw std::runtime_error("Image readback unavailable");
    const VkBufferImageCopy region{
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {extent.width, extent.height, 1}
    };
    const VkMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    const VkMemoryBarrier toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    size_t count = 0;
    do {
        std::memcpy(staging.bytes, frame.data(), frame.size());
        upload.begin(gpu);
        upload.copyBufferToImage(gpu, staging.buffer, fidx % 2 ? sourceImages.second : sourceImages.first);
        if (count == 0) upload.copyBufferToImage(gpu, staging.buffer, sourceImages.second);
        const VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
        gpu.df().CmdPipelineBarrier(upload.native(), VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &ready, 0, nullptr, 0, nullptr);
        upload.end(gpu);
        upload.submit(gpu);
        const auto done = idx + 1;
        syncSemaphore.signal(gpu, idx);
        scheduleFrames();
        if (!syncSemaphore.wait(gpu, done, 30000000000ULL))
            throw std::runtime_error("LSFG timed out");
        if (count > 0) {
            download.begin(gpu);
            gpu.df().CmdPipelineBarrier(download.native(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toTransfer, 0, nullptr, 0, nullptr);
            copy(download.native(), image, VK_IMAGE_LAYOUT_GENERAL, staging.buffer.handle(), 1, &region);
            gpu.df().CmdPipelineBarrier(download.native(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr, 0, nullptr);
            download.end(gpu);
            download.submit(gpu);
            if (std::fwrite(staging.bytes, 1, frame.size(), stdout) != frame.size())
                throw std::runtime_error("Encoder stopped");
        }
        writeFrame(frame);
        if (++count % 60 == 0) std::fprintf(stderr, "LSFG_FRAMES %zu\n", count);
    } while (readFrame(frame));
    // Hold the last frame for its original duration, precisely two output ticks.
    writeFrame(frame);
    if (std::fflush(stdout) != 0) throw std::runtime_error("Output write failed");
    std::fprintf(stderr, "LSFG_DONE %zu -> %zu\n", count, count * 2);
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 4) throw std::runtime_error("Usage: lsfg-file WIDTH HEIGHT Lossless.dll");
        const int width = std::stoi(argv[1]), height = std::stoi(argv[2]);
        if (width < 64 || height < 64 || width > 8192 || height > 8192)
            throw std::runtime_error("Unsupported input dimensions");
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
        Instance engine([](const std::string& name, auto, auto) {
            return name.find("NVIDIA") != std::string::npos;
        }, std::filesystem::path(argv[3]), true);
        auto& context = engine.openContext({nullptr, nullptr}, {nullptr}, nullptr,
            width, height, false, 1.0f, false);
        context.streamVideo();
        engine.closeContext(context);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "LSFG: %s\n", error.what());
        return 1;
    }
}

