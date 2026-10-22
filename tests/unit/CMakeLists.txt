add_executable(ldm_unit_tests
    base/PathsTests.cpp
    base/ResultTests.cpp
    base/UtfTests.cpp
    core/CoordinatorTests.cpp
    core/SegmentPlannerTests.cpp
    core/UrlAndNamesTests.cpp
    engine/FailurePolicyTests.cpp
    engine/HttpHeadersTests.cpp
    engine/TokenBucketTests.cpp
    model/DownloadTests.cpp
    model/SettingsTests.cpp
    storage/SqliteStoreTests.cpp)
target_link_libraries(ldm_unit_tests PRIVATE
    ldm_engine
    ldm_storage
    GTest::gtest
    GTest::gtest_main)
gtest_discover_tests(ldm_unit_tests DISCOVERY_MODE PRE_TEST)
