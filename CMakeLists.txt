cmake_minimum_required(VERSION 3.24)
project(kilrogg LANGUAGES CXX)

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
  src/sender/dummy_source.cpp
  src/sender/dxgi_capture.cpp
)
target_include_directories(kilrogg-send PRIVATE ${lz4_SOURCE_DIR}/lib)
target_link_libraries(kilrogg-send PRIVATE krg_common ${KRG_LZ4} d3d11 dxgi user32)

add_executable(kilrogg-recv
  src/receiver/main.cpp
  src/receiver/presenter.cpp
)
target_include_directories(kilrogg-recv PRIVATE ${lz4_SOURCE_DIR}/lib)
target_link_libraries(kilrogg-recv PRIVATE krg_common ${KRG_LZ4} d3d11 dxgi d3dcompiler user32)
