"""Compile the generated Vulkan result handlers for Android and other platforms."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def result_switch(source, method):
    body = source.split(method, 1)[1]
    start = body.index("switch (result)")
    opening = body.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (body[end] == "{") - (body[end] == "}")
        end += 1
    return body[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--compiler-id", default="Clang")
    args = parser.parse_args()
    source = Path(args.source).read_text()
    acquire = result_switch(source, "bool SwapChainVK::acquire(")
    present = result_switch(source, "void SwapChainVK::present(")
    # Compile the actual result switches, including their platform guards. A
    # suboptimal acquire still owns an image/semaphore and must be consumed.
    # Out-of-date and lost surfaces must continue to request recovery.
    fixture = r'''
#include <cassert>
#include <initializer_list>
#define BX_ASSERT(test, ...) assert(test)
constexpr int VK_SUCCESS = 0, VK_SUBOPTIMAL_KHR = 1000001003;
constexpr int VK_ERROR_OUT_OF_DATE_KHR = -1000001004;
constexpr int VK_ERROR_SURFACE_LOST_KHR = -1000000000;
struct Swap {
    bool m_needToRecreateSurface = false, m_needToRecreateSwapchain = false;
    bool acquire(int result) { ACQUIRE return true; }
    void present(int result) { PRESENT }
};
int main() {
    for (int result : {VK_SUCCESS, VK_SUBOPTIMAL_KHR,
                       VK_ERROR_OUT_OF_DATE_KHR, VK_ERROR_SURFACE_LOST_KHR}) {
        const bool usable = result == VK_SUCCESS ||
            (BX_PLATFORM_ANDROID && result == VK_SUBOPTIMAL_KHR);
        Swap acquire;
        assert(acquire.acquire(result) == usable);
        assert(acquire.m_needToRecreateSwapchain == !usable);
        assert(acquire.m_needToRecreateSurface == (result == VK_ERROR_SURFACE_LOST_KHR));
        Swap present;
        present.present(result);
        assert(present.m_needToRecreateSwapchain == !usable);
        assert(present.m_needToRecreateSurface == (result == VK_ERROR_SURFACE_LOST_KHR));
    }
}
'''.replace("ACQUIRE", acquire).replace("PRESENT", present)
    with tempfile.TemporaryDirectory(prefix="bgfx-swapchain-test-") as temporary:
        directory = Path(temporary)
        path = directory / "policy.cpp"
        path.write_text(fixture)
        for android in (0, 1):
            executable = directory / "policy.exe"
            if args.compiler_id == "MSVC":
                command = [args.compiler, "/std:c++17", "/EHsc",
                           f"/DBX_PLATFORM_ANDROID={android}", str(path),
                           "/Fe:" + str(executable), "/Fo:" + str(directory / "policy.obj")]
            else:
                command = [args.compiler, "-std=c++17",
                           f"-DBX_PLATFORM_ANDROID={android}", str(path), "-o", str(executable)]
            subprocess.run(command, check=True)
            subprocess.run([str(executable)], check=True)
    print("Vulkan acquire/present: Android advisory results and surface recovery passed")


if __name__ == "__main__":
    main()
