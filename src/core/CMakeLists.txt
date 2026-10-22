add_library(ldm_core STATIC
    SegmentPlanner.cpp
    SegmentPlanner.h
    Store.h
    TransferEngine.h)
target_link_libraries(ldm_core PUBLIC ldm_model)
