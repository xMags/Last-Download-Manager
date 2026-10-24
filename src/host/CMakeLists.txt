# The native messaging host. Kept apart from the rest of LDM: it links only the protocol and the
# pipe plumbing, and is written next to ldm.exe, which it starts when LDM is not running.
add_executable(ldm-host WIN32 main.cpp ldm-host.rc)
target_link_libraries(ldm-host PRIVATE ldm_browser ldm_pipe shell32)
set_target_properties(ldm-host PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/src/app)
set_source_files_properties(ldm-host.rc PROPERTIES
    COMPILE_DEFINITIONS
    "LDM_VERSION_MAJOR=${PROJECT_VERSION_MAJOR};LDM_VERSION_MINOR=${PROJECT_VERSION_MINOR};LDM_VERSION_PATCH=${PROJECT_VERSION_PATCH};LDM_VERSION_STRING=\"${PROJECT_VERSION}\"")
