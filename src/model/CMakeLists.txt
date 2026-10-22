add_library(ldm_model STATIC
    Category.h
    ConnectionRules.h
    Download.cpp
    Download.h
    Ids.h
    Settings.cpp
    Settings.h)
target_link_libraries(ldm_model PUBLIC ldm_base)
