# The browser extension is plain JavaScript; building it means copying files, which
# extension/build.mjs does with Node's standard library. Without Node the C++ build still
# works and the extension is simply not built.
find_program(LDM_NODE node)
if(NOT LDM_NODE)
    message(STATUS "Node.js not found: the browser extension is not built or tested")
    return()
endif()

# One fixed folder for every preset (build/extension), so the folder a browser loaded unpacked is
# always the one the latest build updated.
add_custom_target(ldm_extension ALL
    COMMAND ${LDM_NODE} ${PROJECT_SOURCE_DIR}/extension/build.mjs
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Building the browser extension into build/extension"
    VERBATIM)

if(LDM_BUILD_TESTS)
    add_test(NAME extension_tests
        COMMAND ${LDM_NODE} --test "extension/test/*.test.mjs"
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
endif()
