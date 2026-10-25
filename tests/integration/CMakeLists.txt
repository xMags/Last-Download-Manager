find_package(nlohmann_json CONFIG REQUIRED)

# Stands in for yt-dlp in SiteResolverTests.
add_executable(ldm_fake_site_resolver FakeSiteResolver.cpp)

add_executable(ldm_integration_tests
    BrowserHostTests.cpp
    ChildProcessTests.cpp
    CoreTests.cpp
    EngineTests.cpp
    FixtureServer.cpp
    FixtureServer.h
    MediaEngineTests.cpp
    MediaFixtures.cpp
    MediaFixtures.h
    MediaFixtureTests.cpp
    RecordingListener.h
    SiteResolverTests.cpp)
target_link_libraries(ldm_integration_tests PRIVATE
    ldm_engine
    ldm_grabber
    ldm_platform
    ldm_storage
    nlohmann_json::nlohmann_json
    GTest::gtest
    GTest::gtest_main)
target_compile_definitions(ldm_integration_tests PRIVATE
    LDM_FIXTURE_SCRIPT="${PROJECT_SOURCE_DIR}/tests/fixture-server/http-reference-fixture.mjs"
    LDM_HOST_EXE=L"$<TARGET_FILE:ldm-host>"
    # The copies next to ldm.exe, so the tests check exactly what ships (src/app/CMakeLists.txt).
    LDM_FFMPEG_EXE=L"$<TARGET_FILE_DIR:ldm>/ffmpeg.exe"
    LDM_FFPROBE_EXE=L"$<TARGET_FILE_DIR:ldm>/ffprobe.exe"
    LDM_FAKE_SITE_RESOLVER_EXE=L"$<TARGET_FILE:ldm_fake_site_resolver>"
    LDM_MEDIA_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/media")
add_dependencies(ldm_integration_tests ldm-host ldm ldm_fake_site_resolver)
gtest_discover_tests(ldm_integration_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS fixture TIMEOUT 300)
