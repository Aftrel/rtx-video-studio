# RTX Video Studio

[简体中文](README.zh-CN.md) · [Download for Windows x64](https://github.com/Aftrel/rtx-video-studio/releases/latest)

Turn SDR footage into a 4K HDR10 MP4 at twice the source frame rate. You can enable RTX Video Super Resolution, RTX Video HDR, and frame interpolation independently. To compare interpolation engines before a full export, render the same five-second clip with each one.

## Getting started

1. Download the Windows x64 ZIP from the release page and extract the entire folder.
2. Install the [.NET 10 Desktop Runtime for Windows x64](https://dotnet.microsoft.com/download/dotnet/10.0). Double-click `Install-Runtime.cmd` to download about 1 GB of dependencies. The installer verifies each download with SHA-256.
3. Open `RTX Video Studio.exe`, drag a video into the window, and select the effects you want. The app starts in Simplified Chinese on Chinese-language Windows installations and in English elsewhere. Use the language selector at the top right to switch at any time between jobs. The app remembers your choice. Choose **Preview 5 seconds** or **Export full video**. By default, the app saves the result next to the input and adds a number if the filename is already in use.

## HDR look

**Gentle** (default) rolls off bright SDR tones before RTX HDR expands them, with a 500-nit peak target. Near white, the curve gives closely spaced tones more separation, so textures such as ripples on bright water remain visible. It preserves linear RGB ratios. At native-size VSR, Gentle uses an additional GPU pass to apply the curve. **Classic** preserves the original 1,000-nit mapping. To compare, choose the same preview start time and export once with each look. On the command line, use `--hdr-look gentle` or `--hdr-look classic`.

## Export quality

The **Export quality** menu controls HEVC compression. Efficient (高效), Standard (标准, default), and High (高质量) correspond to roughly **20, 40, and 80 Mbps** of video for the 4K60 HDR calibration clip. NVENC uses target-quality VBR (CQ) and varies the bitrate with the footage. Other resolutions and frame rates can produce substantially different rates. The app displays the measured video bitrate after export.

Use `--quality efficient`, `--quality standard`, or `--quality high` on the command line. Previews and full exports use the same setting.

## Frame interpolation

| Engine | Requirements and behavior |
| --- | --- |
| Lossless Scaling (LSFG) | Uses your local Steam installation of Lossless Scaling. Tested with version 3.2.2. This is the default engine. |
| NVIDIA Maxine VFG | Uses High quality mode with 2× frame interpolation and automatic scene-cut detection. Supports Ada and Blackwell GPUs. |

For eligible 8-bit, limited-range BT.709 inputs with a constant frame rate, both engines use NVDEC for decoding and NVENC for encoding. LSFG shares a buffer in video memory between CUDA and its Vulkan interpolation engine. The status line shows **GPU-memory path** during accelerated interpolation. Other supported inputs use the compatibility path automatically.

## Input and output

- **Video** — Accepts SDR inputs with even dimensions, square pixels, and no rotation metadata. Upscaling preserves the aspect ratio and fits the image within 3840 × 2160. TrueHDR writes 10-bit HEVC with BT.2020 primaries and the PQ transfer function.
- **Tracks** — Exports the first main video stream and the first audio track. Full exports copy compatible AAC audio. Previews and other supported audio formats are encoded as AAC. Allow disk space for intermediate video files.
- **Tested export** — On Windows x64 with a GeForce RTX 5070 Ti and driver 621.24, a 129.5-second 1080p30 clip produced 7,770 frames of 2160p60 HDR10 video. The exported AAC stream matched the source audio hash.

## Command line

From the extracted folder, run either command. The second renders a five-second preview starting at 20 seconds.

```powershell
dotnet "RTX Video Studio.dll" --render "input.mp4" "output" --nvidia-vfg
dotnet "RTX Video Studio.dll" --render "input.mp4" "output" --preview 20 --nvidia-vfg
```

Omit `--nvidia-vfg` to use LSFG. Use `--no-vsr`, `--no-hdr`, or `--no-fg` to disable upscaling, HDR conversion, or frame interpolation, respectively.

## Source and build

The desktop app and native workers are in `src/`. To build the Windows Forms app with the .NET 10 SDK, run the following from the repository root.

```powershell
dotnet publish src/Studio.csproj -c Release -o build
```

The LSFG worker includes the corresponding upstream source snapshot and GPL license from [lsfg-vk-windows](https://github.com/56TR/lsfg-vk-windows). The NVIDIA worker uses the [Maxine Video Effects SDK](https://docs.nvidia.com/maxine/vfx/latest/Filters/VideoFrameGeneration.html). Upscaling and HDR conversion use the RTX Video SDK through [NVEncC](https://github.com/rigaya/NVEnc). For same-size VSR, the app uses the CQ encoding path in sdr2hdr. The installer obtains it and the standalone FFmpeg tools from the [sdr2hdr release](https://github.com/zizhenliu0427/sdr2hdr/releases/tag/v1.1.0). For native build dependencies and component versions, see the [component notes (Chinese)](src/组件来源.txt). Each third-party component is subject to its upstream license.
