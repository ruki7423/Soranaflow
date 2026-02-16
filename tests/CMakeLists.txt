# ── Sorana Flow Unit Tests ─────────────────────────────────────────
# Uses Qt Test + CTest.
#
# Usage from build/:
#   cmake .. -DBUILD_TESTS=ON && make -j$(sysctl -n hw.ncpu) && ctest --output-on-failure

find_package(Qt6 REQUIRED COMPONENTS Test)

# Helper: add_sorana_test(NAME src.cpp SOURCES extra.cpp ... LIBS Qt6::Sql ...)
function(add_sorana_test)
    cmake_parse_arguments(ARG "" "NAME" "SOURCES;LIBS" ${ARGN})

    set(TEST_TARGET "test_${ARG_NAME}")
    add_executable(${TEST_TARGET} ${ARG_SOURCES})

    target_link_libraries(${TEST_TARGET} PRIVATE
        Qt6::Core
        Qt6::Widgets
        Qt6::Sql
        Qt6::Network
        Qt6::Concurrent
        Qt6::Test
        ${ARG_LIBS}
    )

    target_include_directories(${TEST_TARGET} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/core
        ${CMAKE_SOURCE_DIR}/src/core/audio
        ${CMAKE_SOURCE_DIR}/src/core/dsp
        ${CMAKE_SOURCE_DIR}/src/core/library
        ${CMAKE_SOURCE_DIR}/src/ui
        ${CMAKE_SOURCE_DIR}/src/metadata
        ${CMAKE_BINARY_DIR}/generated
    )

    set_target_properties(${TEST_TARGET} PROPERTIES
        AUTOMOC ON
        CXX_STANDARD 20
    )

    add_test(NAME ${ARG_NAME} COMMAND ${TEST_TARGET})
endfunction()

# ── Test suites ───────────────────────────────────────────────────

add_sorana_test(
    NAME QueueManager
    SOURCES
        tst_QueueManager.cpp
        ${CMAKE_SOURCE_DIR}/src/core/QueueManager.cpp
        ${CMAKE_SOURCE_DIR}/src/core/MusicData.cpp
)

add_sorana_test(
    NAME MusicKitStateMachine
    SOURCES
        tst_MusicKitStateMachine.cpp
        ${CMAKE_SOURCE_DIR}/src/apple/MusicKitStateMachine.cpp
)

add_sorana_test(
    NAME SignalPathInfo
    SOURCES
        tst_SignalPathInfo.cpp
)

add_sorana_test(
    NAME SignalPathBuilder
    SOURCES
        tst_SignalPathBuilder.cpp
        ${CMAKE_SOURCE_DIR}/src/core/audio/SignalPathBuilder.cpp
)

add_sorana_test(
    NAME VolumeLevelingManager
    SOURCES
        tst_VolumeLevelingManager.cpp
        stubs_VolumeLeveling.cpp
        ${CMAKE_SOURCE_DIR}/src/core/audio/VolumeLevelingManager.cpp
        ${CMAKE_SOURCE_DIR}/src/core/Settings.cpp
        ${CMAKE_SOURCE_DIR}/src/core/MusicData.cpp
        ${CMAKE_SOURCE_DIR}/src/core/library/LibraryDatabase.h
)

add_sorana_test(
    NAME ServiceLocator
    SOURCES
        tst_ServiceLocator.cpp
)
