# The Flutter tooling requires that developers have CMake 3.10 or later
# installed. You should not increase this version, as doing so will cause
# the plugin to fail to compile for some customers of the plugin.
cmake_minimum_required(VERSION 3.10)

project(looper_engine_library VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(looper_engine SHARED
  "miniaudio_impl.cpp"
  "engine.cpp"
  "api.cpp"
)

set_target_properties(looper_engine PROPERTIES
  PUBLIC_HEADER looper_api.h
  OUTPUT_NAME "looper_engine"
)

target_compile_definitions(looper_engine PUBLIC DART_SHARED_LIB)

find_package(Threads REQUIRED)
target_link_libraries(looper_engine PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

if (ANDROID)
  # Support Android 15 16k page size
  target_link_options(looper_engine PRIVATE "-Wl,-z,max-page-size=16384")
  target_link_libraries(looper_engine PRIVATE log)
endif()
