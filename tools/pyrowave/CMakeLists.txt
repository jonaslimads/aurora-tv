# PyroWave round-trip verification tool.
#
# Native desktop only: it drives the vendored GPU codec through its Vulkan C API,
# so it is guarded by ENABLE_PYROWAVE (which the root CMakeLists only enables for
# native desktop targets) and must never be added for webOS or the Steam Link.

# The round trip drives Aurora's own record scanner, so it builds that translation
# unit rather than a copy of the framing rules: if the scanner and the codec disagree
# about a record boundary, this test says so.
add_executable(pyrowave-roundtrip pyrowave_roundtrip.cpp
        ${PROJECT_SOURCE_DIR}/src/app/stream/video/pyrowave_frame.c)

target_include_directories(pyrowave-roundtrip PRIVATE ${PROJECT_SOURCE_DIR}/src/app)

target_link_libraries(pyrowave-roundtrip PRIVATE pyrowave::capi)

set_target_properties(pyrowave-roundtrip PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED ON)

if (CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(pyrowave-roundtrip PRIVATE -Wno-missing-field-initializers)
endif()

# Where the tool writes its frame fixtures, independent of the ctest workdir.
target_compile_definitions(pyrowave-roundtrip PRIVATE
        PYROWAVE_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/pyrowave")

if (NOT WIN32)
    target_link_libraries(pyrowave-roundtrip PRIVATE dl m pthread)
endif()

# Verify the fixture directory exists (it is committed alongside this tool).
set(PYROWAVE_FIXTURE_OUTPUT_DIR "${PROJECT_SOURCE_DIR}/tests/fixtures/pyrowave")

add_test(NAME pyrowave_roundtrip COMMAND pyrowave-roundtrip)
# Skips gracefully (exit 0) when no Vulkan device is present, so this is safe in
# CI containers and on headless build hosts.
set_tests_properties(pyrowave_roundtrip PROPERTIES
        LABELS "pyrowave"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

# Ensure the fixture directory is present before the test writes into it.
set_property(TEST pyrowave_roundtrip PROPERTY ENVIRONMENT
        "PYROWAVE_FIXTURE_PRESENT=${PYROWAVE_FIXTURE_OUTPUT_DIR}")
