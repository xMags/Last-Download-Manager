find_package(nlohmann_json CONFIG REQUIRED)

add_executable(ldm_integration_tests
    CoreTests.cpp
    EngineTests.cpp
    FixtureServer.cpp
    FixtureServer.h)
target_link_libraries(ldm_integration_tests PRIVATE
    ldm_engine
    ldm_platform
    ldm_storage
    nlohmann_json::nlohmann_json
    GTest::gtest
    GTest::gtest_main)
target_compile_definitions(ldm_integration_tests PRIVATE
    LDM_FIXTURE_SCRIPT="${PROJECT_SOURCE_DIR}/tools/research/http-reference-fixture.mjs")
gtest_discover_tests(ldm_integration_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS fixture TIMEOUT 300)
