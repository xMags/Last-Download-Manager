add_library(ldm_core INTERFACE)
target_sources(ldm_core INTERFACE FILE_SET HEADERS BASE_DIRS ${PROJECT_SOURCE_DIR}/src FILES
    Store.h)
target_link_libraries(ldm_core INTERFACE ldm_model)
