"""Exercise the compiled texture-readback methods with explicit Vulkan cache state."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    body = source.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--compiler-id", default="Clang")
    args = parser.parse_args()
    source = Path(args.source).read_text()
    read_texture = function(source, "void readTexture(TextureHandle _handle,").replace(" override", "")
    readback = function(source, "void ReadbackVK::readback(")
    host_buffer = function(source, "VkResult createHostBuffer(")
    program = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
using VkDeviceMemory = unsigned;
using VkDeviceSize = unsigned;
using VkBuffer = unsigned;
using VkResult = unsigned;
using VkBufferUsageFlags = unsigned;
using VkMemoryPropertyFlags = unsigned;
constexpr unsigned VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 2;
constexpr unsigned VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 4;
constexpr unsigned VK_MEMORY_PROPERTY_HOST_CACHED_BIT = 8;
constexpr unsigned VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE = 6;
constexpr unsigned VK_WHOLE_SIZE = ~0u;
constexpr unsigned VK_NULL_HANDLE = 0;
constexpr unsigned VK_SUCCESS = 0;
constexpr unsigned VK_ERROR_UNKNOWN = 1;
constexpr unsigned VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12;
constexpr unsigned VK_SHARING_MODE_EXCLUSIVE = 0;
constexpr unsigned VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 1;
constexpr unsigned VK_BUFFER_USAGE_TRANSFER_DST_BIT = 2;
#define BX_TRACE(...) ((void)0)
#define VK_CHECK(expr) assert((expr) == VK_SUCCESS)
#define BGFX_PROFILER_SCOPE(...) ((void)0)
constexpr unsigned kOffset = 16;
static std::vector<uint8_t> deviceBytes, hostBytes;
static bool completed, mapped, coherent, cachedAvailable = true;
static unsigned invalidations, selectedFlags, bufferUsage;
static std::vector<unsigned> allocationRequests;
struct VkBufferCreateInfo {
  unsigned sType; const void* pNext; unsigned flags, size, queueFamilyIndexCount;
  const unsigned* pQueueFamilyIndices; unsigned sharingMode, usage;
};
struct VkMemoryRequirements { unsigned size, memoryTypeBits; };
unsigned vkCreateBuffer(unsigned, const VkBufferCreateInfo* info, void*, VkBuffer* buffer) {
  bufferUsage = info->usage;
  deviceBytes.assign(info->size+kOffset, 0); hostBytes.assign(info->size+kOffset, 0);
  *buffer = 1;
  return VK_SUCCESS;
}
void vkGetBufferMemoryRequirements(unsigned, VkBuffer, VkMemoryRequirements* req) {
  req->size = unsigned(deviceBytes.size());
  // Mirrors this Adreno device: a TRANSFER_SRC buffer cannot use type 0xb.
  req->memoryTypeBits = cachedAvailable ? ((bufferUsage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ? 2 : 3) : 4;
}
unsigned vkBindBufferMemory(unsigned, VkBuffer, VkDeviceMemory, unsigned) { return VK_SUCCESS; }
struct VkMappedMemoryRange {
  unsigned sType; const void* pNext; VkDeviceMemory memory;
  VkDeviceSize offset, size;
};
unsigned vkMapMemory(unsigned, VkDeviceMemory, unsigned offset, unsigned size,
                     unsigned, void** out) {
  assert(completed && !mapped && offset == 0 && size == VK_WHOLE_SIZE);
  mapped = true;
  if (coherent) hostBytes = deviceBytes;
  *out = hostBytes.data();
  return VK_SUCCESS;
}
unsigned vkInvalidateMappedMemoryRanges(unsigned, unsigned count, const VkMappedMemoryRange* range) {
  assert(mapped && completed && count == 1);
  assert(range->sType == VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
  assert(range->offset == 0 && range->size == VK_WHOLE_SIZE);
  std::copy(deviceBytes.begin(), deviceBytes.end(), hostBytes.begin());
  ++invalidations;
  return VK_SUCCESS;
}
void vkUnmapMemory(unsigned, VkDeviceMemory) { assert(mapped); mapped = false; }
void vkDestroy(VkBuffer) { assert(!mapped); }
namespace bx {
void memCopy(void* dst, const void* src, unsigned bytes) { std::memcpy(dst,src,bytes); }
unsigned uint32_max(unsigned a, unsigned b) { return std::max(a,b); }
void gather(void* dst, const void* src, unsigned sourceStride, unsigned stride, unsigned rows) {
  assert(mapped);
  for (unsigned row = 0; row < rows; ++row)
    std::memcpy(static_cast<uint8_t*>(dst) + row*stride,
                static_cast<const uint8_t*>(src) + row*sourceStride, stride);
}
}
struct TextureHandle { unsigned idx; };
struct DeviceMemoryAllocationVK { VkDeviceMemory mem; unsigned offset; };
struct ReadbackVK {
  unsigned m_width = 8, m_height = 4, m_image = 1;
  unsigned pitch(unsigned mip) const { return std::max(1u, m_width >> mip)*4; }
  void copyImageToBuffer(unsigned, VkBuffer, unsigned, unsigned, unsigned mip) {
    assert(!mapped);
    for (unsigned n = 0; n < pitch(mip)*std::max(1u, m_height >> mip); ++n)
      deviceBytes[kOffset+n] = static_cast<uint8_t>(n + mip + 1);
    completed = false;
  }
  void readback(VkDeviceMemory, VkDeviceSize, void*, uint8_t) const;
};
struct TextureVK { unsigned m_height = 4, m_currentImageLayout = 0, m_aspectFlags = 1; ReadbackVK m_readback; };
struct Renderer {
  TextureVK m_textures[1]; unsigned m_commandBuffer = 1, m_device = 1;
  void* m_allocatorCb = nullptr;
  unsigned allocateMemory(const VkMemoryRequirements* req, unsigned flags,
                          DeviceMemoryAllocationVK* memory, bool) {
    allocationRequests.push_back(flags);
    constexpr unsigned types[] = {11,15,6};
    for (unsigned index=0;index<3;++index) {
      if ((req->memoryTypeBits & (1u<<index)) && (types[index]&flags)==flags) {
        selectedFlags = types[index];
        coherent = (selectedFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        memory->mem = 1; memory->offset = kOffset;
        return VK_SUCCESS;
      }
    }
    return VK_ERROR_UNKNOWN;
  }
  HOST_BUFFER
  unsigned createReadbackBuffer(unsigned size, VkBuffer* buffer, DeviceMemoryAllocationVK* memory) {
    return createHostBuffer(size, 14, buffer, memory, true, nullptr);
  }
  void kick(bool wait) { assert(wait); completed = true; }
  void recycleMemory(DeviceMemoryAllocationVK) { assert(!mapped); }
  READ_TEXTURE
};
Renderer renderer;
Renderer* s_renderVK = &renderer;
READBACK
int main() {
  for (bool available : {true, false}) {
    cachedAvailable = available;
    for (unsigned mip : {0u,1u,2u,3u}) {
      invalidations = 0;
      allocationRequests.clear();
      const unsigned size = std::max(1u,8u>>mip)*std::max(1u,4u>>mip)*4;
      std::vector<uint8_t> output(size+2, 0xEE);
      renderer.readTexture({0}, output.data()+1, mip);
      assert(output.front()==0xEE && output.back()==0xEE);
      for (unsigned n=0;n<size;++n) assert(output[n+1] == uint8_t(n+mip+1));
#if BX_PLATFORM_ANDROID
      assert(bufferUsage == VK_BUFFER_USAGE_TRANSFER_DST_BIT);
      if (available) assert(selectedFlags == 11);
      assert(allocationRequests == (available ? std::vector<unsigned>{10} : std::vector<unsigned>{10,2}));
      assert(invalidations == 1);
#else
      assert(bufferUsage == (VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
      assert(coherent && invalidations == 0);
      assert(allocationRequests == (available ? std::vector<unsigned>{14} : std::vector<unsigned>{14,6}));
#endif
      assert(!mapped);
    }
  }
}
'''.replace("READ_TEXTURE", read_texture).replace("READBACK", readback).replace("HOST_BUFFER", host_buffer)
    with tempfile.TemporaryDirectory(prefix="bgfx-readback-test-") as temporary:
        fixture = Path(temporary) / "readback.cpp"
        fixture.write_text(program)
        for android in (0, 1):
            binary = Path(temporary) / (f"readback-{android}.exe")
            if args.compiler_id == "MSVC":
                flags = ["/std:c++17", f"/DBX_PLATFORM_ANDROID={android}", str(fixture), f"/Fe:{binary}"]
            else:
                flags = ["-std=c++17", f"-DBX_PLATFORM_ANDROID={android}", str(fixture), "-o", str(binary)]
            subprocess.run([args.compiler, *flags], check=True, cwd=temporary)
            subprocess.run([str(binary)], check=True)
    print("Vulkan texture readback: cached memory, invalidation, fallback, offsets and mip sizes passed")


if __name__ == "__main__":
    main()
