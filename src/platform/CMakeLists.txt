find_package(nlohmann_json CONFIG REQUIRED)

# Pipe plumbing shared with ldm-host.exe, which must not pull in the rest of LDM.
add_library(ldm_pipe STATIC
    win/PipeIo.cpp
    win/PipeIo.h)
target_link_libraries(ldm_pipe PUBLIC ldm_base PRIVATE advapi32)

add_library(ldm_platform STATIC
    win/BrowserBridge.cpp
    win/BrowserBridge.h
    win/BrowserHostRegistration.cpp
    win/BrowserHostRegistration.h
    win/ChildProcess.cpp
    win/ChildProcess.h
    win/FfmpegCommands.cpp
    win/FfmpegCommands.h
    win/FfmpegMediaTools.cpp
    win/FfmpegMediaTools.h
    win/InstanceChannel.cpp
    win/InstanceChannel.h
    win/RunKeyStartup.cpp
    win/RunKeyStartup.h
    win/SystemPorts.cpp
    win/SystemPorts.h
    win/WinHttpProxyResolver.cpp
    win/WinHttpProxyResolver.h
    win/WinBrowserWindows.cpp
    win/WinBrowserWindows.h
    win/WinShellActions.cpp
    win/WinShellActions.h
    win/WinSystemActions.cpp
    win/WinSystemActions.h
    win/XmlLiteReader.cpp
    win/XmlLiteReader.h)
target_link_libraries(ldm_platform
    PUBLIC ldm_browser ldm_core ldm_media ldm_pipe ldm_ui
    PRIVATE nlohmann_json::nlohmann_json winhttp bcrypt shell32 shlwapi xmllite dwmapi ole32 winmm powrprof advapi32)
