cmake_minimum_required(VERSION 3.24)
project(kilrogg LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4 /MP)
endif()

include(FetchContent)
set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  lz4
  GIT_REPOSITORY https://github.com/lz4/lz4.git
  GIT_TAG        v1.10.0
  SOURCE_SUBDIR  build/cmake
)
FetchContent_MakeAvailable(lz4)

foreach(cand lz4_static lz4 LZ4::lz4_static LZ4::lz4)
  if(TARGET ${cand})
    set(KRG_LZ4 ${cand})
    break()
  endif()
endforeach()
if(NOT KRG_LZ4)
  message(FATAL_ERROR "lz4 library target not found after FetchContent")
endif()

add_library(krg_common STATIC
  src/common/net.cpp
)
target_include_directories(krg_common PUBLIC src)
target_compile_definitions(krg_common PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(krg_common PUBLIC ws2_32)

add_executable(kilrogg-send
  src/sender/main.cpp
  src/sender/control_receiver.cpp
  src/sender/cursor_shape.cpp
  src/sender/displays.cpp
  src/sender/dummy_source.cpp
  src/sender/dxgi_capture.cpp
  src/sender/mf_encoder.cpp
  src/sender/packet_sender.cpp
  src/sender/rate_control.cpp
  src/sender/kilrogg-send.rc
)
target_include_directories(kilrogg-send PRIVATE ${lz4_SOURCE_DIR}/lib)
target_link_libraries(kilrogg-send PRIVATE krg_common ${KRG_LZ4} d3d11 dxgi user32 winmm
  mf mfplat mfuuid evr ole32 oleaut32)

add_executable(kilrogg-recv
  src/receiver/main.cpp
  src/receiver/presenter.cpp
  src/receiver/mf_decoder.cpp
  src/receiver/kilrogg-recv.rc
)
target_include_directories(kilrogg-recv PRIVATE ${lz4_SOURCE_DIR}/lib)
target_link_libraries(kilrogg-recv PRIVATE krg_common ${KRG_LZ4} d3d11 dxgi d3dcompiler user32
  gdi32 mf mfplat mfuuid evr ole32 oleaut32)

# Tests cover the parts of the pipeline that are decisions rather than device
# calls: rate control, the frame-rate budget, the clock-offset estimate, wire
# validation, cursor shape decoding, display selection. None of them need a GPU,
# a socket or a screen, which is why they are worth having — everything else in
# this project can only be tested by pointing it at a real desktop.
# -DKRG_BUILD_TESTS=OFF skips the doctest fetch entirely.
option(KRG_BUILD_TESTS "Build the kilrogg test suite" ON)
if(KRG_BUILD_TESTS)
  # SOURCE_SUBDIR names a directory with no CMakeLists.txt in it, which is how
  # FetchContent is told to fetch this and configure nothing. doctest is a
  # single header; its own build system declares a cmake_minimum_required that
  # CMake 4 refuses outright, and none of what it builds is wanted here anyway.
  FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    SOURCE_SUBDIR  doctest
  )
  FetchContent_MakeAvailable(doctest)
  add_library(krg_doctest INTERFACE)
  # SYSTEM so /W4 applies to the tests and not to doctest's header, which trips
  # a conformance warning of its own on current MSVC.
  target_include_directories(krg_doctest SYSTEM INTERFACE ${doctest_SOURCE_DIR})

  enable_testing()
  add_executable(kilrogg-tests
    tests/main.cpp
    tests/test_clock_sync.cpp
    tests/test_cursor_shape.cpp
    tests/test_displays.cpp
    tests/test_frame_budget.cpp
    tests/test_mailbox.cpp
    tests/test_protocol.cpp
    tests/test_rate_control.cpp
    tests/test_rate_memory.cpp
    # Under test, not merely linked: these are the only two translation units
    # with logic in them that is not already header-only.
    src/sender/cursor_shape.cpp
    src/sender/displays.cpp
    src/sender/rate_control.cpp
  )
  target_link_libraries(kilrogg-tests PRIVATE krg_common krg_doctest dxgi user32)

  include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
  # One CTest case per TEST_CASE rather than one for the whole binary, so a
  # failure names itself in the ctest output instead of just the executable.
  doctest_discover_tests(kilrogg-tests)
endif()
