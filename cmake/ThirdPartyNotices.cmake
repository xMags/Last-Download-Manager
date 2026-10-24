# Writes THIRD-PARTY-NOTICES.txt for the release packages (M5 design section 2.4): every shipped
# third-party component with the version vcpkg built, its license and its source, followed by the
# license texts vcpkg installed with each port. Run in script mode:
#   cmake -DVCPKG_SHARE=<installed>/<triplet>/share -DOUTPUT=<file> -P ThirdPartyNotices.cmake

if(NOT VCPKG_SHARE OR NOT OUTPUT)
    message(FATAL_ERROR "ThirdPartyNotices.cmake needs VCPKG_SHARE and OUTPUT")
endif()

# port | name | license | source | what LDM uses it for
set(LDM_COMPONENTS
    "ffmpeg|FFmpeg|LGPL 2.1 or later (built without its GPL and nonfree parts)|https://ffmpeg.org/|ffmpeg.exe and ffprobe.exe, which join and describe streamed media"
    "curl|libcurl|curl license (MIT-style)|https://curl.se/|HTTP, HTTPS and FTP transfers, linked into ldm.exe"
    "nghttp2|nghttp2|MIT|https://nghttp2.org/|HTTP/2 for libcurl, linked into ldm.exe"
    "brotli|Brotli|MIT|https://github.com/google/brotli|compressed responses for libcurl, linked into ldm.exe"
    "zlib|zlib|zlib license|https://zlib.net/|compressed responses for libcurl, linked into ldm.exe"
    "sqlite3|SQLite|public domain|https://sqlite.org/|the download list and settings, linked into ldm.exe"
    "nlohmann-json|JSON for Modern C++|MIT|https://github.com/nlohmann/json|reading and writing JSON, compiled into ldm.exe and ldm-host.exe")

function(ldm_port_version port out)
    set(spdx "${VCPKG_SHARE}/${port}/vcpkg.spdx.json")
    if(NOT EXISTS "${spdx}")
        message(FATAL_ERROR "${spdx} is missing: is the vcpkg port ${port} installed?")
    endif()
    file(READ "${spdx}" json)
    string(JSON count LENGTH "${json}" packages)
    math(EXPR last "${count} - 1")
    foreach(index RANGE ${last})
        string(JSON name GET "${json}" packages ${index} name)
        if(name STREQUAL port)
            string(JSON version GET "${json}" packages ${index} versionInfo)
            # vcpkg's port revision ("#2") is not part of the component's own version.
            string(REGEX REPLACE "#[0-9]+$" "" version "${version}")
            set(${out} "${version}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "${spdx} names no package ${port}")
endfunction()

set(text "Last Download Manager includes the following third-party components.\n")
string(APPEND text "Last Download Manager itself is licensed under the GNU General Public License version 3 (LICENSE).\n\n")
foreach(entry IN LISTS LDM_COMPONENTS)
    string(REPLACE "|" ";" fields "${entry}")
    list(GET fields 0 port)
    list(GET fields 1 name)
    list(GET fields 2 license)
    list(GET fields 3 source)
    list(GET fields 4 use)
    ldm_port_version(${port} version)
    string(APPEND text "${name} ${version}\n  License: ${license}\n  Source: ${source}\n  Used for: ${use}\n\n")
endforeach()

ldm_port_version(ffmpeg ffmpegVersion)
string(APPEND text
    "Source code offer for FFmpeg\n"
    "  ffmpeg.exe and ffprobe.exe are FFmpeg ${ffmpegVersion}, built by vcpkg from the FFmpeg release\n"
    "  source (https://ffmpeg.org/releases/ffmpeg-${ffmpegVersion}.tar.xz) with the patches of vcpkg's\n"
    "  ffmpeg port (https://github.com/microsoft/vcpkg/tree/master/ports/ffmpeg), configured without\n"
    "  --enable-gpl, --enable-version3 or --enable-nonfree and without external libraries, so it is\n"
    "  covered by the LGPL 2.1. On request, the project provides that exact source and the\n"
    "  build instructions for at least three years from the release: open an issue at\n"
    "  https://github.com/xMags/Last-Download-Manager/issues. You may replace ffmpeg.exe and\n"
    "  ffprobe.exe with your own builds of FFmpeg.\n\n")

string(APPEND text "License texts\n=============\n\n")
foreach(entry IN LISTS LDM_COMPONENTS)
    string(REPLACE "|" ";" fields "${entry}")
    list(GET fields 0 port)
    list(GET fields 1 name)
    set(copyright "${VCPKG_SHARE}/${port}/copyright")
    if(NOT EXISTS "${copyright}")
        message(FATAL_ERROR "${copyright} is missing")
    endif()
    file(READ "${copyright}" license)
    string(APPEND text "----- ${name} -----\n\n${license}\n\n")
endforeach()

file(WRITE "${OUTPUT}" "${text}")
