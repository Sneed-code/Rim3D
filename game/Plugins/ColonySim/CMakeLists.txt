# Standalone build of the sim, its tests and the simrun tool, without Unreal.
#   cmake -S game/Plugins/ColonySim -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --parallel
#   ctest --test-dir build --output-on-failure
cmake_minimum_required(VERSION 3.22)
project(ColonySim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Same rules as Unreal: no exceptions, no RTTI. Warnings are errors.
add_library(colonysim_flags INTERFACE)
if(MSVC)
  target_compile_options(colonysim_flags INTERFACE /W4 /WX /permissive- /GR- /EHs-c- /external:W0)
  target_compile_definitions(colonysim_flags INTERFACE _HAS_EXCEPTIONS=0 _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(colonysim_flags INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Werror -fno-exceptions -fno-rtti)
endif()

file(GLOB_RECURSE SIM_SOURCES CONFIGURE_DEPENDS Source/ColonySim/Private/*.cpp)
list(FILTER SIM_SOURCES EXCLUDE REGEX ".*ColonySimModule\\.cpp$")
add_library(colonysim STATIC ${SIM_SOURCES})
target_include_directories(colonysim PUBLIC Source/ColonySim/Public)
target_link_libraries(colonysim PUBLIC colonysim_flags)
# nlohmann/json is used inside Defs.cpp only; vendored, so treat as a system header.
target_include_directories(colonysim SYSTEM PRIVATE ThirdParty)

add_library(colonysim_thirdparty INTERFACE)
target_include_directories(colonysim_thirdparty SYSTEM INTERFACE ThirdParty)

add_executable(simrun Tools/simrun/main.cpp)
target_link_libraries(simrun PRIVATE colonysim colonysim_thirdparty)

enable_testing()
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS Tests/*.cpp)
add_executable(colonysim_tests ${TEST_SOURCES})
target_link_libraries(colonysim_tests PRIVATE colonysim colonysim_thirdparty)
target_compile_definitions(colonysim_tests PRIVATE COLONY_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../data")
add_test(NAME unit COMMAND colonysim_tests)
add_test(NAME simrun_smoke COMMAND simrun --seed 1 --size 64 --days 1 --png ${CMAKE_BINARY_DIR}/smoke.png)
