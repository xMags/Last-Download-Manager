find_package(nlohmann_json CONFIG REQUIRED)

# Site Grabber (M4 design section 6): link scanning, filters, the project definition, and the
# explorer that runs on the core thread beside the coordinator. No Win32: every part is tested
# with fakes of the core's ports.
add_library(ldm_grabber STATIC
    GrabberExplorer.cpp
    GrabberExplorer.h
    GrabberFilters.cpp
    GrabberFilters.h
    GrabberSnapshot.h
    HtmlLinks.cpp
    HtmlLinks.h
    LocalPaths.cpp
    LocalPaths.h
    ProjectDefinition.cpp
    ProjectDefinition.h)
target_link_libraries(ldm_grabber
    PUBLIC ldm_core
    PRIVATE nlohmann_json::nlohmann_json)
