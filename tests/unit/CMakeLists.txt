find_package(nlohmann_json CONFIG REQUIRED)

add_executable(ldm_unit_tests
    base/PathsTests.cpp
    base/ResultTests.cpp
    base/UtfTests.cpp
    browser/ProtocolTests.cpp
    core/CaptureCasesTests.cpp
    core/CoordinatorTests.cpp
    core/QuotaTests.cpp
    core/SegmentPlannerTests.cpp
    core/UrlAndNamesTests.cpp
    engine/FailurePolicyTests.cpp
    engine/HttpHeadersTests.cpp
    engine/TokenBucketTests.cpp
    model/DownloadTests.cpp
    model/SettingsTests.cpp
    storage/SqliteStoreTests.cpp
    ui/BatchAndHistoryTests.cpp
    ui/ExportAndLaunchTests.cpp
    ui/ListModelTests.cpp)
target_link_libraries(ldm_unit_tests PRIVATE
    ldm_browser
    ldm_engine
    ldm_platform
    ldm_storage
    ldm_ui
    nlohmann_json::nlohmann_json
    GTest::gtest
    GTest::gtest_main)
target_compile_definitions(ldm_unit_tests PRIVATE
    LDM_CAPTURE_CASES="${PROJECT_SOURCE_DIR}/extension/test/capture-cases.json")
gtest_discover_tests(ldm_unit_tests DISCOVERY_MODE PRE_TEST)
