find_package(nlohmann_json CONFIG REQUIRED)

add_library(ldm_browser STATIC
    HostManifest.cpp
    HostManifest.h
    Protocol.cpp
    Protocol.h)
target_link_libraries(ldm_browser
    PUBLIC ldm_base
    PRIVATE nlohmann_json::nlohmann_json)
