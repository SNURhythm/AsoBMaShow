# Keep UIKit-owned layer mutations on main, without moving drawable acquisition
# or GPU waits there. Applied to a generated copy of the pinned bgfx source.
function(asobmashow_patch_ios_bgfx_main_thread_layer content_variable)
    set(content "${${content_variable}}")
    set(anchor "#import <Foundation/Foundation.h>")
    string(FIND "${content}" "${anchor}" offset)
    if(offset EQUAL -1)
        message(FATAL_ERROR "bgfx Metal imports changed; review main-thread layer patch")
    endif()
    set(helper [=[
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>

static void asobmashowConfigureMetalLayer(void (^operation)(void))
{
    if ([NSThread isMainThread]) operation();
    else dispatch_sync(dispatch_get_main_queue(), operation);
}
]=])
    string(REPLACE "${anchor}" "${helper}" content "${content}")
    set(initialization [=[			m_metalLayer.device              = s_renderMtl->m_device;
			m_metalLayer.magnificationFilter = kCAFilterNearest;

			const Resolution& resolution = s_renderMtl->m_resolution;
			m_metalLayer.pixelFormat     = (resolution.reset & BGFX_RESET_SRGB_BACKBUFFER)
				? s_textureFormat[resolution.formatColor].m_fmtSrgb
				: s_textureFormat[resolution.formatColor].m_fmt
				;]=])
    set(resize [=[			m_metalLayer.drawableSize = CGSizeMake(_width, _height);
			m_metalLayer.pixelFormat  = (resetFlags & BGFX_RESET_SRGB_BACKBUFFER)
				? s_textureFormat[formatColor].m_fmtSrgb
				: s_textureFormat[formatColor].m_fmt
				;]=])
    foreach(part IN ITEMS initialization resize)
        string(FIND "${content}" "${${part}}" offset)
        if(offset EQUAL -1)
            message(FATAL_ERROR "bgfx Metal layer ${part} changed; review main-thread layer patch")
        endif()
        string(REPLACE "${${part}}"
            "            asobmashowConfigureMetalLayer(^{\n${${part}}\n            });"
            content "${content}")
    endforeach()
    set(${content_variable} "${content}" PARENT_SCOPE)
endfunction()
