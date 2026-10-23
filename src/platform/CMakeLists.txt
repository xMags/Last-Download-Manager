find_package(nlohmann_json CONFIG REQUIRED)

add_library(ldm_platform STATIC
    win/InstanceChannel.cpp
    win/InstanceChannel.h
    win/RunKeyStartup.cpp
    win/RunKeyStartup.h
    win/SystemPorts.cpp
    win/SystemPorts.h
    win/WinHttpProxyResolver.cpp
    win/WinHttpProxyResolver.h
    win/WinShellActions.cpp
    win/WinShellActions.h
    win/WinSystemActions.cpp
    win/WinSystemActions.h)
target_link_libraries(ldm_platform
    PUBLIC ldm_core ldm_ui
    PRIVATE nlohmann_json::nlohmann_json winhttp bcrypt shell32 ole32 winmm powrprof advapi32)
