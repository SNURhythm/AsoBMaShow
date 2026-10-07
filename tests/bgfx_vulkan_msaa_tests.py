"""Compile the actual Vulkan attachment flag expression and check sample counts."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--include", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--compiler-id", default="Clang")
    args = parser.parse_args()
    source = Path(args.source).read_text()
    attachment_code = source.split("VkResult SwapChainVK::createAttachments(", 1)[1]
    expression = re.search(r"const uint64_t textureFlags = (.*?);", attachment_code)[1]
    # The selector is decoded by TextureVK::create. Check every reset level,
    # including 8x (also changed by OR-ing BGFX_TEXTURE_RT) without needing a GPU.
    program = r'''
#include <bgfx/defines.h>
#include <cstdint>
constexpr uint64_t attachmentFlags(uint32_t reset) {
    const uint32_t samplerIndex = (reset & BGFX_RESET_MSAA_MASK) >> BGFX_RESET_MSAA_SHIFT;
    return EXPRESSION;
}
constexpr uint32_t samples(uint32_t reset) {
    const auto selector = (attachmentFlags(reset) & BGFX_TEXTURE_RT_MSAA_MASK) >> BGFX_TEXTURE_RT_MSAA_SHIFT;
    return 1u << (selector - 1);
}
static_assert(samples(0) == 1);
static_assert(samples(BGFX_RESET_MSAA_X2) == 2);
static_assert(samples(BGFX_RESET_MSAA_X4) == 4);
static_assert(samples(BGFX_RESET_MSAA_X8) == 8);
static_assert(samples(BGFX_RESET_MSAA_X16) == 16);
static_assert(samples(BGFX_RESET_MSAA_X2 | BGFX_RESET_VSYNC) == 2);
static_assert((attachmentFlags(BGFX_RESET_MSAA_X2) & BGFX_TEXTURE_RT_WRITE_ONLY) != 0);
'''.replace("EXPRESSION", expression)
    with tempfile.TemporaryDirectory(prefix="bgfx-msaa-test-") as temporary:
        fixture = Path(temporary) / "samples.cpp"
        fixture.write_text(program)
        flags = (["/std:c++17", "/Zs", "/I" + args.include]
                 if args.compiler_id == "MSVC"
                 else ["-std=c++17", "-fsyntax-only", "-I" + args.include])
        subprocess.run([args.compiler, *flags, str(fixture)], check=True)
    print("Vulkan attachment sample counts: 1x, 2x, 4x, 8x, 16x passed")


if __name__ == "__main__":
    main()
