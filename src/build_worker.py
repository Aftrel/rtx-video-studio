"""Build with llvm-mingw, Khronos Vulkan-Headers and 56TR/lsfg-vk-windows b03458e.
Usage: python build_worker.py path/to/clang++.exe lsfg-source Vulkan-Headers scratch-dir [ffmpeg-dev cuda-runtime-dev]
The optional development directories build the GPU-resident worker in bin/vfg.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import shutil
import subprocess
import sys

if len(sys.argv) not in (5, 7):
    raise SystemExit(__doc__)
compiler, upstream, headers, scratch = map(Path, sys.argv[1:5])
gpu = len(sys.argv) == 7
base = Path(__file__).resolve().parent
scratch = scratch.resolve()
scratch.mkdir(exist_ok=True, parents=True)
tree = scratch / "source"
shutil.copytree(upstream, tree, dirs_exist_ok=True, ignore=shutil.ignore_patterns(".git"))
common = tree / "lsfg-vk-common"
backend = tree / "lsfg-vk-backend"

def replace(path, original, new):
    text = path.read_text(encoding="utf-8")
    if original not in text:
        raise RuntimeError(f"Unexpected upstream source: {path}: {original}")
    path.write_text(text.replace(original, new), encoding="utf-8")

bridge = backend / "src/lsfgvk.cpp"
extra = "friend class GpuFrames;" if gpu else "void streamVideo();"
replace(bridge, "void scheduleFrames();", "void scheduleFrames();\n        " + extra)
replace(bridge, "sourceFds.first)", "sourceFds.first ? std::optional{sourceFds.first} : std::nullopt)")
replace(bridge, "sourceFds.second)", "sourceFds.second ? std::optional{sourceFds.second} : std::nullopt)")
replace(bridge, "VK_IMAGE_USAGE_SAMPLED_BIT, sourceFds.", "VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, sourceFds.")
replace(bridge, "VK_IMAGE_USAGE_SAMPLED_BIT, fd)", "VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, fd ? std::optional{fd} : std::nullopt)")
replace(bridge, "return{vk, 0, syncFd};", "return{vk, 0, syncFd ? std::optional{syncFd} : std::nullopt};")
replace(bridge, "images.push_back(this->blackImage.handle());", """images.push_back(this->blackImage.handle());
    if (!sourceFds.first) {
        images.push_back(sourceImages.first.handle());
        images.push_back(sourceImages.second.handle());
        for (const auto& dest : destImages) images.push_back(dest.handle());
    }""")
entry = "lsfg-gpu.inc" if gpu else "offline.inc"
with bridge.open("a", encoding="utf-8") as f:
    f.write(f'\n#include "{entry}"\n')
for filename, getter in [("command_buffer", "const auto& native() const { return commandBuffer.get(); }"),
                         ("buffer", "const auto& allocation() const { return memory.get(); }")]:
    path = common / f"include/lsfg-vk-common/vulkan/{filename}.hpp"
    replace(path, "    private:", f"        {getter}\n    private:")
flags = ["-std=c++20", "-O2", f"-ffile-prefix-map={tree}=lsfg-vk",
         f"-ffile-prefix-map={compiler.resolve().parent.parent}=toolchain",
         "-Wno-macro-redefined", "-Wno-unknown-attributes",
         "-DVK_USE_PLATFORM_WIN32_KHR", "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN",
         "-include", "algorithm", "-I", str(headers.resolve() / "include"),
         "-I", str(common / "include"), "-I", str(backend / "include")]
flags.extend(["-I", str(base)])
libs = []
output = base.parent / "bin/lsfg-file.exe"
if gpu:
    ffmpeg, cuda = (Path(p).resolve() for p in sys.argv[5:])
    replace(common / "include/lsfg-vk-common/vulkan/vulkan.hpp", "bool supportsFP16()",
            "uint32_t familyIndex() const { return queueFamilyIdx; }\n        bool supportsFP16()")
    flags.extend(["-I", str(ffmpeg / "include"), "-I", str(cuda / "include")])
    libs = ["-L", str(ffmpeg / "lib"), "-lavformat", "-lavcodec", "-lavutil", str(cuda / "lib/x64/cuda.lib")]
    output = base.parent / "bin/vfg/lsfg-gpu.exe"
output.parent.mkdir(parents=True, exist_ok=True)
sources = [*(common / "src/vulkan").glob("*.cpp"), common / "src/helpers/errors.cpp",
           *(backend / "src").rglob("*.cpp")]
def compile_unit(src):
    obj = scratch / (str(src.relative_to(tree)).replace("\\","_").replace("/","_") + ".o")
    subprocess.run([compiler, *flags, "-c", src, "-o", obj], check=True)
    return obj
with ThreadPoolExecutor(max_workers=6) as pool:
    objects = list(pool.map(compile_unit, sources))
subprocess.run([compiler, "-static", "-municode", *objects, *libs, "-o", output], check=True)

