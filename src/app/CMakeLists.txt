add_executable(ldm WIN32
    main.cpp
    ${PROJECT_SOURCE_DIR}/src/resources/ldm.rc)
target_link_libraries(ldm PRIVATE
    ldm_engine
    ldm_platform
    ldm_storage
    ldm_ui
    comctl32
    ole32)
# ldm-host.exe ships next to ldm.exe and is registered by it.
add_dependencies(ldm ldm-host)

# ffmpeg.exe and ffprobe.exe ship next to ldm.exe too (M3 design section 6, decision 5). vcpkg
# builds them from the ffmpeg port in vcpkg.json and installs them in its tools folder; tests run
# the copies next to ldm.exe, so they check exactly what ships.
set(LDM_FFMPEG_TOOLS_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/ffmpeg")
foreach(tool IN ITEMS ffmpeg ffprobe)
    if(NOT EXISTS "${LDM_FFMPEG_TOOLS_DIR}/${tool}.exe")
        message(FATAL_ERROR "${tool}.exe is missing from ${LDM_FFMPEG_TOOLS_DIR}; the ffmpeg port in vcpkg.json should install it.")
    endif()
    add_custom_command(TARGET ldm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LDM_FFMPEG_TOOLS_DIR}/${tool}.exe" "$<TARGET_FILE_DIR:ldm>"
        VERBATIM)
endforeach()

target_include_directories(ldm PRIVATE ${PROJECT_SOURCE_DIR}/src/resources)
target_compile_definitions(ldm PRIVATE "LDM_VERSION=\"${PROJECT_VERSION}\"")

# The manifest is embedded through ldm.rc, so the linker must not generate a second one.
target_link_options(ldm PRIVATE /MANIFEST:NO)

set_source_files_properties(${PROJECT_SOURCE_DIR}/src/resources/ldm.rc PROPERTIES
    COMPILE_DEFINITIONS
    "LDM_VERSION_MAJOR=${PROJECT_VERSION_MAJOR};LDM_VERSION_MINOR=${PROJECT_VERSION_MINOR};LDM_VERSION_PATCH=${PROJECT_VERSION_PATCH};LDM_VERSION_STRING=\"${PROJECT_VERSION}\"")
