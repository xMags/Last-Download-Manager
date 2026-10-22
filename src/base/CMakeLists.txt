add_library(ldm_base STATIC
    Result.h
    Utf.cpp
    Utf.h)
target_include_directories(ldm_base PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(ldm_base PUBLIC ldm_build_settings)
