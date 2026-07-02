# Standalone build for the OpenGOAL Jak 1 Mumble plugin.
# Intentionally NOT wired into the main game build - build it separately:
#   cmake -B build -S .
#   cmake --build build --config Release
# On Windows, prefer clang-cl or MSVC (plain clang++ picks the wrong symbol
# export attribute in MumblePlugin.h and the plugin won't load).
cmake_minimum_required(VERSION 3.16)
project(opengoal_jak1_positions CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(opengoal_jak1_positions SHARED opengoal_jak1_positions.cpp)
target_include_directories(opengoal_jak1_positions PRIVATE include)

if(UNIX)
  target_link_libraries(opengoal_jak1_positions PRIVATE pthread rt)
endif()
