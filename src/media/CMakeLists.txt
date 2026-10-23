# Streamed media: the HLS and DASH model and parsers (M3 design section 6). Pure: no Win32, so
# the parsers are tested directly; the XML reader DASH needs is passed in from platform/win.
add_library(ldm_media STATIC
    MediaModel.cpp
    MediaModel.h
    NumberText.cpp
    NumberText.h
    UrlReference.cpp
    UrlReference.h)
target_link_libraries(ldm_media PUBLIC ldm_base)
