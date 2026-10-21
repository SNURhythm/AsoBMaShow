# Owned external textures use the ordinary Metal resource lifetime. A retained
# texture view carries its CoreVideo owner through command-buffer completion.
# Other callers of bgfx's borrowed external-texture API remain unchanged.
function(asobmashow_patch_bgfx_metal_video_surfaces content_variable)
    set(source "${${content_variable}}")
    set(anchor "#define UNIFORM_BUFFER_SIZE (8*1024*1024)")
    set(registry [=[
#include <mutex>
#include <unordered_set>

namespace {
std::mutex asoMetalTextureMutex;
std::unordered_set<uintptr_t> asoMetalOwnedTextures;
bool asoTakeMetalTexture(uintptr_t texture)
{
    std::lock_guard<std::mutex> lock(asoMetalTextureMutex);
    return asoMetalOwnedTextures.erase(texture) != 0;
}
}
extern "C" bool asobmashowMetalRegisterOwnedTexture(uintptr_t texture)
{
    try
    {
        std::lock_guard<std::mutex> lock(asoMetalTextureMutex);
        if (!asoMetalOwnedTextures.insert(texture).second) return false;
        [(id<MTLTexture>)texture retain];
        return true;
    }
    catch (...) { return false; }
}
extern "C" void asobmashowMetalCancelOwnedTexture(uintptr_t texture)
{
    if (asoTakeMetalTexture(texture)) [(id<MTLTexture>)texture release];
}
]=])
    set(external "\t\t\t\tm_ptr    = id<MTLTexture>(_external);\n\t\t\t\tm_flags |= BGFX_SAMPLER_INTERNAL_SHARED;")
    set(external_fixed [=[
                m_ptr = id<MTLTexture>(_external);
                if (!asoTakeMetalTexture(_external))
                    m_flags |= BGFX_SAMPLER_INTERNAL_SHARED;
]=])
    set(create "\tvoid TextureMtl::create(const Memory* _mem, uint64_t _flags, uint8_t _skip, uint64_t _external)")
    set(device [=[
    // Called only between successful bgfx::init and renderer shutdown. The
    // device is immutable in that interval; no render-thread state is mutated.
    extern "C" uintptr_t asobmashowMetalRetainDevice()
    {
        return (uintptr_t)[(id<MTLDevice>)s_renderMtl->m_device retain];
    }
]=])
    foreach(pattern IN ITEMS anchor external create)
        string(FIND "${source}" "${${pattern}}" match)
        if(match EQUAL -1)
            message(FATAL_ERROR "bgfx Metal source changed; review video surface ownership (${pattern})")
        endif()
    endforeach()
    string(REPLACE "${anchor}" "${registry}\n${anchor}" source "${source}")
    string(REPLACE "${external}" "${external_fixed}" source "${source}")
    string(REPLACE "${create}" "${device}\n${create}" source "${source}")
    set(${content_variable} "${source}" PARENT_SCOPE)
endfunction()
