# Release archives with fixed names, so the README's download buttons (which link to
# releases/latest/download/<name>) always fetch the newest release:
#   LastDownloadManager-windows-x64.zip       ldm.exe, ldm-host.exe, ffmpeg.exe, ffprobe.exe, LICENSE,
#                                             THIRD-PARTY-NOTICES.txt (-arm64.zip from the
#                                             windows-release-arm64 preset)
#   LastDownloadManager-browser-extension.zip  chromium/ and firefox/, to load unpacked
#   LastDownloadManager-chromium.zip,
#   LastDownloadManager-firefox.zip            the extension packages for the browser stores
# Build with: cmake --build --preset windows-release --target ldm_package
# The archives are written to <build dir>/package/.

# The processor the programs are built for names the archive and the installer.
if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
    set(LDM_PACKAGE_ARCH arm64)
elseif(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    set(LDM_PACKAGE_ARCH x64)
else()
    message(FATAL_ERROR "Unsupported target processor: ${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
endif()

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
            ${CMAKE_COMMAND} -E tar cf LastDownloadManager-windows-${LDM_PACKAGE_ARCH}.zip --format=zip LastDownloadManager)
set(LDM_PACKAGE_DEPENDS ldm ldm-host)

if(TARGET ldm_extension)
    list(APPEND LDM_PACKAGE_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/build/extension" "${LDM_EXTENSION_STAGE}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${LDM_EXTENSION_STAGE}/LastDownloadManager-chromium.zip"
                "${LDM_EXTENSION_STAGE}/LastDownloadManager-firefox.zip"
        COMMAND ${CMAKE_COMMAND} -E chdir "${LDM_PACKAGE_DIR}"
                ${CMAKE_COMMAND} -E tar cf LastDownloadManager-browser-extension.zip --format=zip
                LastDownloadManager-browser-extension
        COMMAND ${CMAKE_COMMAND} -E copy "${PROJECT_SOURCE_DIR}/build/extension/LastDownloadManager-chromium.zip"
                "${PROJECT_SOURCE_DIR}/build/extension/LastDownloadManager-firefox.zip" "${LDM_PACKAGE_DIR}")
    list(APPEND LDM_PACKAGE_DEPENDS ldm_extension)
endif()

add_custom_target(ldm_package
    ${LDM_PACKAGE_COMMANDS}
    COMMENT "Writing the release archives to ${LDM_PACKAGE_DIR}"
    VERBATIM)
add_dependencies(ldm_package ${LDM_PACKAGE_DEPENDS})
