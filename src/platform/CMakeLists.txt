add_library(ldm_platform STATIC
    win/SystemPorts.cpp
    win/SystemPorts.h
    win/WinHttpProxyResolver.cpp
    win/WinHttpProxyResolver.h)
target_link_libraries(ldm_platform
    PUBLIC ldm_core
    PRIVATE winhttp bcrypt)
