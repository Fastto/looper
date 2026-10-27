cmake_minimum_required(VERSION 3.10)
project(looper_engine_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(engine_test engine_test.cpp ../src/engine.cpp)
target_compile_options(engine_test PRIVATE -Wall -Wextra -fsanitize=address,undefined)
target_link_options(engine_test PRIVATE -fsanitize=address,undefined)

enable_testing()
add_test(NAME engine_test COMMAND engine_test)

# Hardware smoke test (not part of ctest: needs a real audio device).
add_executable(device_smoke device_smoke.cpp ../src/engine.cpp ../src/api.cpp ../src/miniaudio_impl.cpp)
if (APPLE)
  target_link_libraries(device_smoke PRIVATE "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreFoundation")
else()
  find_package(Threads REQUIRED)
  target_link_libraries(device_smoke PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()
