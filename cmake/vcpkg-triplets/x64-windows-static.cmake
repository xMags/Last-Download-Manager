# vcpkg's stock x64-windows-static triplet, plus one change: FFmpeg is built in Release only.
# LDM never links FFmpeg's libraries; it runs the ffmpeg.exe and ffprobe.exe this build produces,
# which vcpkg takes from the Release build anyway. A Debug build of FFmpeg would only double its
# build time. CMakePresets.json points VCPKG_OVERLAY_TRIPLETS here.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

if(PORT STREQUAL "ffmpeg")
    set(VCPKG_BUILD_TYPE release)
endif()
