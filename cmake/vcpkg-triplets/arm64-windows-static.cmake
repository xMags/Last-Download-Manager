# vcpkg's stock arm64-windows-static triplet, with the same change as x64-windows-static.cmake:
# FFmpeg is built in Release only. Used by the windows-release-arm64 preset.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

if(PORT STREQUAL "ffmpeg")
    set(VCPKG_BUILD_TYPE release)
endif()
