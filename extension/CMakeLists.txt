# The browser extension is plain JavaScript; building it means copying files, which
# tools/build-extension.mjs does with Node's standard library. Without Node the C++ build still
# works and the extension is simply not built.
find_program(LDM_NODE node)
if(NOT LDM_NODE)
    message(STATUS "Node.js not found: the browser extension is not built or tested")
    return()
endif()

add_custom_target(ldm_extension ALL
    COMMAND ${LDM_NODE} ${PROJECT_SOURCE_DIR}/tools/build-extension.mjs --out ${PROJECT_BINARY_DIR}/extension
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Building the browser extension into ${PROJECT_BINARY_DIR}/extension"
    VERBATIM)

if(LDM_BUILD_TESTS)
    add_test(NAME extension_tests
        COMMAND ${LDM_NODE} --test "extension/test/*.test.mjs"
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
endif()
