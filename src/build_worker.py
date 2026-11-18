"""Build with llvm-mingw, Khronos Vulkan-Headers and 56TR/lsfg-vk-windows b03458e.
Usage: python build_worker.py path/to/clang++.exe lsfg-source Vulkan-Headers scratch-dir
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import shutil
import subprocess
import sys

compiler, upstream, headers, scratch = map(Path, sys.argv[1:])
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
replace(bridge, "void scheduleFrames();", "void scheduleFrames();\n        void streamVideo();")
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
with bridge.open("a", encoding="utf-8") as f:
    f.write('\n#include "offline.inc"\n')
shutil.copyfile(base / "offline.inc", bridge.parent / "offline.inc")
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
sources = [*(common / "src/vulkan").glob("*.cpp"), common / "src/helpers/errors.cpp",
           *(backend / "src").rglob("*.cpp")]
def compile_unit(src):
    obj = scratch / (str(src.relative_to(tree)).replace("\\","_").replace("/","_") + ".o")
    subprocess.run([compiler, *flags, "-c", src, "-o", obj], check=True)
    return obj
with ThreadPoolExecutor(max_workers=6) as pool:
    objects = list(pool.map(compile_unit, sources))
subprocess.run([compiler, "-static", "-municode", *objects, "-o", base.parent / "bin/lsfg-file.exe"], check=True)

