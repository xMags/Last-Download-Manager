add_executable(ldm WIN32
    main.cpp
    ${PROJECT_SOURCE_DIR}/resources/ldm.rc)
target_link_libraries(ldm PRIVATE ldm_base)
target_include_directories(ldm PRIVATE ${PROJECT_SOURCE_DIR}/resources)

# The manifest is embedded through ldm.rc, so the linker must not generate a second one.
target_link_options(ldm PRIVATE /MANIFEST:NO)

set_source_files_properties(${PROJECT_SOURCE_DIR}/resources/ldm.rc PROPERTIES
    COMPILE_DEFINITIONS
    "LDM_VERSION_MAJOR=${PROJECT_VERSION_MAJOR};LDM_VERSION_MINOR=${PROJECT_VERSION_MINOR};LDM_VERSION_PATCH=${PROJECT_VERSION_PATCH};LDM_VERSION_STRING=\"${PROJECT_VERSION}\"")
