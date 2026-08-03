cmake_minimum_required(VERSION 3.15)
project(rline CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# No FetchContent, no network access at configure time, no third-party packages.
# Everything this needs ships in the repo or in the Windows SDK.

# NOMINMAX: windows.h otherwise defines min/max as macros, which breaks any
# use of std::max. WIN32_LEAN_AND_MEAN trims the header considerably.
add_definitions(-D_UNICODE -DUNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN
                -D_CRT_SECURE_NO_WARNINGS)

# ---------------------------------------------------------------- the overlay

add_executable(rline
    src/main.cpp
    src/refline.cpp
    src/event-cue.cpp
    src/components/rendering-comp.cpp
    src/systems/irtelemetry-sys.cpp
    src/systems/demo-telemetry-sys.cpp
    src/systems/refline-overlay-sys.cpp
    src/systems/brake-audio-sys.cpp
    src/systems/window-rendering-sys.cpp
    src/irsdk/irsdk_client.cpp
    src/irsdk/irsdk_utils.cpp
    src/irsdk/yaml_parser.cpp
)

# winmm: PlaySound, for the braking countdown.
target_link_libraries(rline winmm)

if(MSVC)
    target_compile_options(rline PRIVATE /W3 /permissive-)
endif()

# Put the reference lap next to the exe so it is found with no arguments.
add_custom_command(TARGET rline POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/data/lap.csv"
            "$<TARGET_FILE_DIR:rline>/lap.csv")

# ------------------------------------------------------------------- the tests
#
# The geometry, the lateral reconstruction, the overlay's draw output and the
# countdown timing are all free of windows.h, so these build and run anywhere,
# with no Windows SDK and no running copy of iRacing. Only the window,
# telemetry and audio layers are Win32-only.

add_executable(test-refline
    tests/test-refline.cpp
    src/refline.cpp
)

add_executable(test-overlay
    tests/test-overlay.cpp
    src/refline.cpp
    src/components/rendering-comp.cpp
    src/systems/refline-overlay-sys.cpp
)

add_executable(test-lateral
    tests/test-lateral.cpp
    src/refline.cpp
)

add_executable(test-event-cue
    tests/test-event-cue.cpp
    src/event-cue.cpp
    src/refline.cpp
)

# ------------------------------------------------------------- the diagnostic
#
# Lists the channels a live session exposes and can capture them to CSV. Used to
# establish that iRacing publishes no live position. Not shipped.

add_executable(irdump
    tools/irdump.cpp
    src/irsdk/irsdk_client.cpp
    src/irsdk/irsdk_utils.cpp
    src/irsdk/yaml_parser.cpp
)

# --------------------------------------------------------------------- common

# Static CRT: every resulting exe runs on a clean Windows install with no
# Visual C++ redistributable and no dev tools.
if(MSVC)
    foreach(t rline test-refline test-overlay test-lateral test-event-cue irdump)
        set_property(TARGET ${t} PROPERTY
                     MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
endif()

enable_testing()
add_test(NAME refline
         COMMAND test-refline "${CMAKE_CURRENT_SOURCE_DIR}/data/lap.csv")
add_test(NAME overlay
         COMMAND test-overlay "${CMAKE_CURRENT_SOURCE_DIR}/data/lap.csv")
add_test(NAME event-cue
         COMMAND test-event-cue "${CMAKE_CURRENT_SOURCE_DIR}/data/lap.csv")
add_test(NAME lateral
         COMMAND test-lateral "${CMAKE_CURRENT_SOURCE_DIR}/data/lap.csv"
                              "${CMAKE_CURRENT_SOURCE_DIR}/data/drive-roadamerica.csv")
