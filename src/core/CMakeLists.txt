add_library(ldm_core STATIC
    Commands.h
    Coordinator.cpp
    Coordinator.h
    CoreThread.cpp
    CoreThread.h
    FailureText.cpp
    FailureText.h
    FileNames.cpp
    FileNames.h
    LaunchRequest.cpp
    LaunchRequest.h
    ManifestFetches.cpp
    ManifestFetches.h
    Matching.cpp
    Matching.h
    Quota.cpp
    Quota.h
    Ports.h
    SegmentPlanner.cpp
    SegmentPlanner.h
    Snapshot.h
    Store.h
    TransferEngine.h
    Url.cpp
    Url.h)
target_link_libraries(ldm_core PUBLIC ldm_model)
