# RTX Video Studio

[English](README.md) · 简体中文

在 Windows 上把 SDR 视频导出为 **4K、双倍帧率、HDR10** 的 MP4。可以分别开启 RTX Video Super Resolution、RTX Video HDR 和插帧，也可以先指定起点，比较同一段 5 秒预览。

## 下载与使用

1. 从 [GitHub Releases](https://github.com/Aftrel/rtx-video-studio/releases/latest) 下载 Windows x64 压缩包，将整个文件夹解压到本地。
2. 安装 [Microsoft .NET 10 桌面运行时（Windows x64）](https://dotnet.microsoft.com/download/dotnet/10.0)，然后双击 `Install-Runtime.cmd`。首次设置需要联网下载约 690 MB 的第三方组件；脚本会核对 SHA-256。
3. 打开 `RTX Video Studio.exe`，拖入视频，选择需要的效果，点「生成 5 秒预览」或「处理完整视频」。成片默认保存在原片旁边，重名时自动编号。

## 两种插帧引擎

| 引擎 | 适用条件与行为 |
| --- | --- |
| Lossless Scaling · LSFG | 使用本机 Steam 安装的 Lossless Scaling。已验证 3.2.2。 |
| NVIDIA VFG · 高质量 | 调用 NVIDIA Maxine Video Frame Generation 的 High 模式，使用两倍帧率和自动切镜检测。支持 Ada、Blackwell 架构的 GPU。 |

部分 8 位、有限范围、BT.709 恒定帧率片源能够使用 NVIDIA VFG 的「显存直通」路径，通过 NVDEC 解码，在显存中生成中间帧，再用 NVENC 编码。其他受支持的输入使用兼容路径。

## 导出规格

- 超分保持原片比例，放入 3840 × 2160 的范围。TrueHDR 以 1000 尼特为目标，输出 HEVC Main 10、BT.2020、PQ。
- 支持常见偶数尺寸、方形像素的 SDR 视频。当前导出第一条主视频和第一条音轨。HDR 输入及带旋转标记的片源暂不支持。
- 整片优先直通 AAC 音轨，预览和其他受支持的音轨会重新编码。处理期间需要为临时视频保留磁盘空间。
- 本机验证环境为 Windows x64、GeForce RTX 5070 Ti、驱动 621.24。一次整片验证将 129.5 秒的 1080p30 视频导出为 7770 帧的 2160p60 HDR10，AAC 音轨哈希与原片一致。

## 命令行

在解压目录中运行：

```powershell
dotnet "RTX Video Studio.dll" --render "input.mp4" "output" --nvidia-vfg
dotnet "RTX Video Studio.dll" --render "input.mp4" "output" --preview 20 --nvidia-vfg
```

默认插帧引擎为 LSFG。`--no-vsr`、`--no-hdr`、`--no-fg` 分别关闭超分、HDR、插帧。

## 源码与组件

桌面应用与原生处理器在 `src/`。使用 .NET 10 SDK 编译桌面入口：

```powershell
dotnet publish src/Studio.csproj -c Release -o build
```

原生处理器的依赖及构建信息见 [组件来源](src/组件来源.txt)。LSFG 适配包含 [lsfg-vk-windows](https://github.com/56TR/lsfg-vk-windows) 的对应源码快照和 GPL 许可证。NVIDIA VFG 使用 [Maxine Video Effects SDK](https://docs.nvidia.com/maxine/vfx/latest/Filters/VideoFrameGeneration.html)，超分及 TrueHDR 通过 [sdr2hdr](https://github.com/zizhenliu0427/sdr2hdr) 调用 NVIDIA RTX Video SDK。第三方组件分别适用其上游许可。
