find_package(nlohmann_json CONFIG REQUIRED)

# Site Grabber (M4 design section 6): link scanning, filters and the project definition. Pure: no
# Win32, so every decision is tested directly.
add_library(ldm_grabber STATIC
    GrabberFilters.cpp
    GrabberFilters.h
    HtmlLinks.cpp
    HtmlLinks.h
    ProjectDefinition.cpp
    ProjectDefinition.h)
target_link_libraries(ldm_grabber
    PUBLIC ldm_core
    PRIVATE nlohmann_json::nlohmann_json)
