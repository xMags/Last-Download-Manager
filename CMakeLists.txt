cmake_minimum_required(VERSION 3.16)
project(LastDM VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set vcpkg toolchain if available
if(DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
elseif(EXISTS "C:/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

# Find packages
find_package(wxWidgets REQUIRED COMPONENTS core base net xml adv)
include(${wxWidgets_USE_FILE})

find_package(CURL REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

# Source files
set(SOURCES
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/CategoriesPanel.cpp
    src/ui/DownloadsTable.cpp
    src/ui/OptionsDialog.cpp
    src/ui/SchedulerDialog.cpp
    src/ui/SpeedGraphPanel.cpp
    src/core/Download.cpp
    src/core/DownloadEngine.cpp
    src/core/DownloadManager.cpp
    src/database/DatabaseManager.cpp
    src/utils/Settings.cpp
    src/utils/ThemeManager.cpp
    src/utils/HashUtils.cpp
)

# Header files
set(HEADERS
    src/ui/MainWindow.h
    src/ui/CategoriesPanel.h
    src/ui/DownloadsTable.h
    src/ui/OptionsDialog.h
    src/ui/SchedulerDialog.h
    src/ui/SpeedGraphPanel.h
    src/core/Download.h
    src/core/DownloadEngine.h
    src/core/DownloadManager.h
    src/database/DatabaseManager.h
    src/utils/Settings.h
    src/utils/ThemeManager.h
    src/utils/HashUtils.h
)

# Windows resource file
if(WIN32)
    set(SOURCES ${SOURCES} resources/app.rc)
endif()

# Create Windows executable (GUI, not console)
add_executable(LastDM WIN32 ${SOURCES} ${HEADERS})

# Link libraries
target_link_libraries(LastDM PRIVATE
    ${wxWidgets_LIBRARIES}
    CURL::libcurl
    unofficial::sqlite3::sqlite3
    bcrypt
)

# Include directories
target_include_directories(LastDM PRIVATE 
    src
    ${wxWidgets_INCLUDE_DIRS}
)

# Windows-specific settings
if(WIN32)
    # Enable Unicode
    target_compile_definitions(LastDM PRIVATE UNICODE _UNICODE wxUSE_GUI=1)
endif()

# Copy required DLLs to output directory (for Debug builds)
if(WIN32)
    add_custom_command(TARGET LastDM POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:LastDM>
        $<TARGET_FILE_DIR:LastDM>
        COMMAND_EXPAND_LISTS
    )
endif()
