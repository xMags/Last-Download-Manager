# Streamed media: the HLS and DASH model and parsers (M3 design section 6). Pure: no Win32, so
# the parsers are tested directly; the XML reader DASH needs is passed in from platform/win.
add_library(ldm_media STATIC
    DashManifest.cpp
    DashManifest.h
    DrmSystems.cpp
    DrmSystems.h
    HlsPlaylist.cpp
    HlsPlaylist.h
    M3uList.cpp
    M3uList.h
    MediaModel.cpp
    MediaModel.h
    NumberText.cpp
    NumberText.h
    QualityList.cpp
    QualityList.h
    SiteVideo.cpp
    SiteVideo.h
    UrlReference.cpp
    UrlReference.h
    XmlElement.cpp
    XmlElement.h)
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(ldm_media
    PUBLIC ldm_base
    PRIVATE nlohmann_json::nlohmann_json)
