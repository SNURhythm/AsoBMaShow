if(NOT DEFINED IMAGE_VIEW_SOURCE)
    message(FATAL_ERROR "IMAGE_VIEW_SOURCE is required")
endif()
file(READ "${IMAGE_VIEW_SOURCE}" IMAGE_VIEW_CONTENTS)
string(REGEX MATCH "stbi_|stbir_|STB_IMAGE" IMAGE_VIEW_STB_REFERENCE
    "${IMAGE_VIEW_CONTENTS}")
if(IMAGE_VIEW_STB_REFERENCE)
    message(FATAL_ERROR
        "ImageView must delegate image decode/resize to ImageFileDecoder; found ${IMAGE_VIEW_STB_REFERENCE}")
endif()
