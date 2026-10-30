# Keep UIKit-owned layer mutations on main, without moving drawable acquisition
# or GPU waits there. Applied to a generated copy of the pinned bgfx source.
function(asobmashow_patch_ios_bgfx_main_thread_layer content_variable)
    set(content "${${content_variable}}")
    set(anchor "#include <metal-cpp/metal.hpp>")
    string(FIND "${content}" "${anchor}" offset)
    if(offset EQUAL -1)
        message(FATAL_ERROR "bgfx Metal imports changed; review main-thread layer patch")
    endif()
    set(helper [=[
#include <metal-cpp/metal.hpp>
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>

static void asobmashowConfigureMetalLayer(void (^operation)(void))
{
    if ([NSThread isMainThread]) operation();
    else dispatch_sync(dispatch_get_main_queue(), operation);
}
]=])
    string(REPLACE "${anchor}" "${helper}" content "${content}")
    set(initialization [=[			m_metalLayer->setDevice(s_renderMtl->m_device);
			MtlObjAccess::send<void>(m_metalLayer, sel_registerName("setMagnificationFilter:"), nsstr("nearest") );

			const Resolution& resolution = s_renderMtl->m_resolution;
			m_metalLayer->setPixelFormat( (MTL::PixelFormat)( (resolution.reset & BGFX_RESET_SRGB_BACKBUFFER)
				? s_textureFormat[resolution.formatColor].fmtSrgb
				: s_textureFormat[resolution.formatColor].fmt) )
				;]=])
    set(resize [=[			m_metalLayer->setDrawableSize(CGSizeMake(_width, _height) );
			m_metalLayer->setPixelFormat( (MTL::PixelFormat)( (resetFlags & BGFX_RESET_SRGB_BACKBUFFER)
				? s_textureFormat[formatColor].fmtSrgb
				: s_textureFormat[formatColor].fmt) )
				;]=])
    foreach(part IN ITEMS initialization resize)
        string(FIND "${content}" "${${part}}" offset)
        if(offset EQUAL -1)
            message(FATAL_ERROR "bgfx Metal layer ${part} changed; review main-thread layer patch")
        endif()
        set(configuration "${${part}}")
        if(part STREQUAL "initialization")
            # Nearest on the whole layer can force composition when iPadOS
            # scales it. Keep the normal linear layer filter on iOS; this does
            # not alter the game's texture sampler settings.
            string(REPLACE "nsstr(\"nearest\")" "nsstr(\"linear\")"
                configuration "${configuration}")
            string(APPEND configuration [=[

            // Keep three drawables available to preserve ProMotion throughput.
            m_metalLayer->setMaximumDrawableCount(3);
]=])
        endif()
        string(REPLACE "${${part}}"
            "            asobmashowConfigureMetalLayer(^{\n${configuration}\n            });"
            content "${content}")
    endforeach()
    set(${content_variable} "${content}" PARENT_SCOPE)
endfunction()
