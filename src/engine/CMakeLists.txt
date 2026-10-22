add_library(ldm_engine STATIC
    HttpHeaders.cpp
    HttpHeaders.h)
target_link_libraries(ldm_engine PUBLIC ldm_core)
