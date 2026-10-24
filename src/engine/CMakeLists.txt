find_package(CURL CONFIG REQUIRED)

add_library(ldm_engine STATIC
    Aes128Cbc.cpp
    Aes128Cbc.h
    BufferPool.cpp
    BufferPool.h
    CurlEngine.cpp
    CurlEngine.h
    ConnectionGuards.cpp
    ConnectionGuards.h
    CurlHandles.h
    DiskWriter.cpp
    DiskWriter.h
    FailurePolicy.cpp
    FailurePolicy.h
    HttpHeaders.cpp
    HttpHeaders.h
    MemoryFetch.cpp
    MediaRun.cpp
    MediaRun.h
    MemoryFetch.h
    Run.cpp
    Run.h
    TokenBucket.cpp
    TokenBucket.h
    Transfer.cpp
    Transfer.h)
target_link_libraries(ldm_engine
    PUBLIC ldm_core CURL::libcurl
    PRIVATE winhttp bcrypt)
