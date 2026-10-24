find_package(nlohmann_json CONFIG REQUIRED)

add_executable(ldm_unit_tests
    base/Base64Tests.cpp
    base/PathsTests.cpp
    base/ResultTests.cpp
    base/UtfTests.cpp
    browser/ProtocolTests.cpp
    core/CaptureCasesTests.cpp
    core/CoordinatorTests.cpp
    core/MediaPiecesTests.cpp
    core/NetworkAddressTests.cpp
    core/QuotaTests.cpp
    core/SegmentPlannerTests.cpp
    core/UrlAndNamesTests.cpp
    engine/Aes128CbcTests.cpp
    engine/FailurePolicyTests.cpp
    engine/HttpHeadersTests.cpp
    engine/TokenBucketTests.cpp
    media/DashManifestTests.cpp
    media/HlsPlaylistTests.cpp
    media/NumberTextTests.cpp
    media/QualityListTests.cpp
    media/UrlReferenceTests.cpp
    model/DownloadTests.cpp
    model/SettingsTests.cpp
    platform/FfmpegCommandsTests.cpp
    platform/XmlLiteReaderTests.cpp
    storage/SqliteStoreTests.cpp
    ui/BatchAndHistoryTests.cpp
    ui/BrowserSessionsTests.cpp
    ui/CaptureFollowUpTests.cpp
    ui/ExportAndLaunchTests.cpp
    ui/ListModelTests.cpp
    ui/MediaCatalogTests.cpp
    ui/MediaPanelMenuTests.cpp
    ui/PageLinksTests.cpp
    ui/PanelPlacementTests.cpp
    ui/StreamChoiceTests.cpp
    # The app's own manifest: the UI code under test needs version 6 of the common controls
    # (SetWindowSubclass and LoadIconMetric exist only there), as ldm.exe gets it.
    ${PROJECT_SOURCE_DIR}/src/resources/ldm.manifest)
target_link_libraries(ldm_unit_tests PRIVATE
    ldm_browser
    ldm_engine
    ldm_media
    ldm_platform
    ldm_storage
    ldm_ui
    nlohmann_json::nlohmann_json
    GTest::gtest
    GTest::gtest_main
    bcrypt)
target_compile_definitions(ldm_unit_tests PRIVATE
    LDM_CAPTURE_CASES="${PROJECT_SOURCE_DIR}/extension/test/capture-cases.json"
    LDM_MEDIA_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/media")
gtest_discover_tests(ldm_unit_tests DISCOVERY_MODE PRE_TEST)
