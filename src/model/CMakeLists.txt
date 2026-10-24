add_library(ldm_model STATIC
    Category.h
    Cookie.cpp
    Cookie.h
    ConnectionRules.h
    Download.cpp
    Download.h
    Ids.h
    Media.cpp
    Media.h
    Segment.h
    Settings.cpp
    Settings.h)
target_link_libraries(ldm_model PUBLIC ldm_base)
