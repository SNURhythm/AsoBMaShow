include_guard(GLOBAL)

# Opt-in source/consumer lists only. Different target compile contracts get
# different objects, even when a difference might be irrelevant to one source.
# Call after all consumer compile settings and link dependencies are finalized.
function(asobmashow_share_test_sources group)
    cmake_parse_arguments(PARSE_ARGV 1 shared "" "" "SOURCES;TARGETS")
    if(shared_UNPARSED_ARGUMENTS OR NOT shared_SOURCES OR NOT shared_TARGETS)
        message(FATAL_ERROR "${group}: expected SOURCES and TARGETS")
    endif()

    set(properties
        COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FLAGS COMPILE_FEATURES
        INCLUDE_DIRECTORIES SYSTEM_INCLUDE_DIRECTORIES LINK_LIBRARIES
        C_STANDARD C_STANDARD_REQUIRED C_EXTENSIONS
        CXX_STANDARD CXX_STANDARD_REQUIRED CXX_EXTENSIONS
        POSITION_INDEPENDENT_CODE MSVC_RUNTIME_LIBRARY
        INTERPROCEDURAL_OPTIMIZATION
        INTERPROCEDURAL_OPTIMIZATION_DEBUG
        INTERPROCEDURAL_OPTIMIZATION_RELEASE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO
        INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL
        NO_SYSTEM_FROM_IMPORTED VISIBILITY_INLINES_HIDDEN
        C_VISIBILITY_PRESET CXX_VISIBILITY_PRESET
        OSX_ARCHITECTURES OSX_ARCHITECTURES_DEBUG OSX_ARCHITECTURES_RELEASE
        OSX_ARCHITECTURES_RELWITHDEBINFO OSX_ARCHITECTURES_MINSIZEREL)

    foreach(consumer IN LISTS shared_TARGETS)
        if(NOT TARGET ${consumer})
            continue()
        endif()
        get_target_property(kind ${consumer} TYPE)
        if(NOT kind STREQUAL "EXECUTABLE")
            message(FATAL_ERROR "${group}: ${consumer} must be a test executable")
        endif()
        set(contract "")
        foreach(property IN LISTS properties)
            get_target_property(value ${consumer} ${property})
            set(${consumer}_${property} "${value}")
            string(APPEND contract "${property}=[${value}]\n")
        endforeach()
        # These expressions depend on the identity of the compiling target.
        # Keeping the original compilation is safer than reinterpreting them.
        get_target_property(pch ${consumer} PRECOMPILE_HEADERS)
        get_target_property(exports ${consumer} ENABLE_EXPORTS)
        get_target_property(pic ${consumer} POSITION_INDEPENDENT_CODE)
        # An executable uses PIE while an object library uses PIC. Copying the
        # property would change compilation, so retain these sources in-place.
        if(contract MATCHES "\\$<TARGET_(PROPERTY|POLICY|OBJECTS):" OR pch OR exports OR pic)
            continue()
        endif()
        string(SHA256 signature "${contract}")
        get_target_property(sources ${consumer} SOURCES)
        foreach(source IN LISTS shared_SOURCES)
            if(source IN_LIST sources)
                string(SHA256 key "${source}\n${signature}")
                list(APPEND owners_${key} ${consumer})
                set(source_${key} "${source}")
                list(APPEND keys ${key})
            endif()
        endforeach()
    endforeach()

    list(REMOVE_DUPLICATES keys)
    foreach(key IN LISTS keys)
        list(LENGTH owners_${key} count)
        if(count LESS 2)
            continue()
        endif()
        list(GET owners_${key} 0 reference)
        string(SUBSTRING "${key}" 0 16 short_key)
        set(object_target "asobmashow_test_${group}_${short_key}")
        add_library(${object_target} OBJECT "${source_${key}}")
        foreach(property IN LISTS properties)
            set(value "${${reference}_${property}}")
            if(NOT value STREQUAL "value-NOTFOUND")
                set_property(TARGET ${object_target} PROPERTY ${property} "${value}")
            endif()
        endforeach()
        foreach(consumer IN LISTS owners_${key})
            get_target_property(sources ${consumer} SOURCES)
            list(REMOVE_ITEM sources "${source_${key}}")
            set_property(TARGET ${consumer} PROPERTY SOURCES "${sources}")
            # Keep the executable's original link interface and include exactly
            # this source's object; no other consumer's fixture is pulled in.
            target_sources(${consumer} PRIVATE $<TARGET_OBJECTS:${object_target}>)
        endforeach()
    endforeach()
endfunction()
