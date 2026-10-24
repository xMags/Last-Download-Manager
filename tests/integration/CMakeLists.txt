find_package(nlohmann_json CONFIG REQUIRED)

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
    RecordingListener.h)
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
    LDM_FFPROBE_EXE=L"$<TARGET_FILE_DIR:ldm>/ffprobe.exe")
add_dependencies(ldm_integration_tests ldm-host ldm)
gtest_discover_tests(ldm_integration_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS fixture TIMEOUT 300)
