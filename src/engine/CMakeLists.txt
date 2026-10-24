find_package(CURL CONFIG REQUIRED)

add_library(ldm_engine STATIC
    BufferPool.cpp
    BufferPool.h
    CurlEngine.cpp
    CurlEngine.h
    CurlHandles.h
    DiskWriter.cpp
    DiskWriter.h
    FailurePolicy.cpp
    FailurePolicy.h
    HttpHeaders.cpp
    HttpHeaders.h
    MemoryFetch.cpp
    MemoryFetch.h
    TokenBucket.cpp
    TokenBucket.h
    Transfer.cpp
    Transfer.h)
target_link_libraries(ldm_engine
    PUBLIC ldm_core CURL::libcurl
    PRIVATE winhttp)
