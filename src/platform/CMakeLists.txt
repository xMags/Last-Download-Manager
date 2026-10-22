add_library(ldm_platform STATIC
    win/SystemPorts.cpp
    win/SystemPorts.h
    win/WinHttpProxyResolver.cpp
    win/WinHttpProxyResolver.h
    win/WinShellActions.cpp
    win/WinShellActions.h)
target_link_libraries(ldm_platform
    PUBLIC ldm_core ldm_ui
    PRIVATE winhttp bcrypt shell32 ole32)
