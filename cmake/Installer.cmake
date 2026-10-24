# The Windows installer (M5 design section 2.3): installer/ldm.iss.in configured with the project
# version and compiled by Inno Setup 6 from the files ldm_package stages. Writes
# <build dir>/package/LastDownloadManager-windows-x64-setup.exe.
# Build with: cmake --build --preset windows-release --target ldm_installer
# Inno Setup is a build-machine tool, not a vcpkg dependency; without it the target is skipped.

find_program(LDM_ISCC ISCC
    HINTS
        "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6"
        "$ENV{ProgramFiles}/Inno Setup 6"
        "$ENV{ProgramW6432}/Inno Setup 6"
        "C:/Program Files (x86)/Inno Setup 6")

if(NOT LDM_ISCC)
    message(STATUS "Inno Setup 6 not found: the ldm_installer target is not available")
    return()
endif()

file(TO_NATIVE_PATH "${LDM_APP_STAGE}" LDM_INSTALLER_STAGE)
file(TO_NATIVE_PATH "${LDM_PACKAGE_DIR}" LDM_INSTALLER_OUTPUT)
file(TO_NATIVE_PATH "${PROJECT_SOURCE_DIR}/src/resources/ldm.ico" LDM_INSTALLER_ICON)
set(LDM_INSTALLER_DIR "${CMAKE_BINARY_DIR}/installer")
configure_file("${PROJECT_SOURCE_DIR}/installer/ldm.iss.in" "${LDM_INSTALLER_DIR}/ldm.iss" @ONLY)
configure_file("${PROJECT_SOURCE_DIR}/installer/sandbox.wsb.in" "${LDM_INSTALLER_DIR}/sandbox.wsb" @ONLY)

add_custom_target(ldm_installer
    COMMAND "${LDM_ISCC}" /Q "${LDM_INSTALLER_DIR}/ldm.iss"
    COMMENT "Writing the installer to ${LDM_PACKAGE_DIR}"
    VERBATIM)
add_dependencies(ldm_installer ldm_package)
