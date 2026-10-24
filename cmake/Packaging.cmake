# Release archives with fixed names, so the README's download buttons (which link to
# releases/latest/download/<name>) always fetch the newest release:
#   LastDownloadManager-windows-x64.zip       ldm.exe, ldm-host.exe, ffmpeg.exe, ffprobe.exe, LICENSE,
#                                             THIRD-PARTY-NOTICES.txt
#   LastDownloadManager-browser-extension.zip  chromium/ and firefox/, to load unpacked
# Build with: cmake --build --preset windows-release --target ldm_package
# The archives are written to <build dir>/package/.

set(LDM_PACKAGE_DIR "${CMAKE_BINARY_DIR}/package")
set(LDM_APP_STAGE "${LDM_PACKAGE_DIR}/LastDownloadManager")
set(LDM_EXTENSION_STAGE "${LDM_PACKAGE_DIR}/LastDownloadManager-browser-extension")

# The versions and license texts come from the vcpkg ports that were built (ThirdPartyNotices.cmake).
set(LDM_VCPKG_SHARE "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
set(LDM_NOTICES "${LDM_PACKAGE_DIR}/THIRD-PARTY-NOTICES.txt")

set(LDM_PACKAGE_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${LDM_APP_STAGE}" "${LDM_EXTENSION_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LDM_APP_STAGE}"
    COMMAND ${CMAKE_COMMAND} "-DVCPKG_SHARE=${LDM_VCPKG_SHARE}" "-DOUTPUT=${LDM_NOTICES}"
            -P "${PROJECT_SOURCE_DIR}/cmake/ThirdPartyNotices.cmake"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:ldm>" "$<TARGET_FILE:ldm-host>"
            "$<TARGET_FILE_DIR:ldm>/ffmpeg.exe" "$<TARGET_FILE_DIR:ldm>/ffprobe.exe"
            "${PROJECT_SOURCE_DIR}/LICENSE" "${LDM_NOTICES}" "${LDM_APP_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E chdir "${LDM_PACKAGE_DIR}"
            ${CMAKE_COMMAND} -E tar cf LastDownloadManager-windows-x64.zip --format=zip LastDownloadManager)
set(LDM_PACKAGE_DEPENDS ldm ldm-host)

if(TARGET ldm_extension)
    list(APPEND LDM_PACKAGE_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/build/extension" "${LDM_EXTENSION_STAGE}"
        COMMAND ${CMAKE_COMMAND} -E chdir "${LDM_PACKAGE_DIR}"
                ${CMAKE_COMMAND} -E tar cf LastDownloadManager-browser-extension.zip --format=zip
                LastDownloadManager-browser-extension)
    list(APPEND LDM_PACKAGE_DEPENDS ldm_extension)
endif()

add_custom_target(ldm_package
    ${LDM_PACKAGE_COMMANDS}
    COMMENT "Writing the release archives to ${LDM_PACKAGE_DIR}"
    VERBATIM)
add_dependencies(ldm_package ${LDM_PACKAGE_DEPENDS})
