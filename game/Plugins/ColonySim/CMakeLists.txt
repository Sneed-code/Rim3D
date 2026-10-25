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
# Vendored libraries are system headers (no warnings). EnTT appears in public sim headers;
# nlohmann/json is used inside Defs.cpp only.
target_include_directories(colonysim SYSTEM PUBLIC ThirdParty)

add_library(colonysim_thirdparty INTERFACE)
target_include_directories(colonysim_thirdparty SYSTEM INTERFACE ThirdParty)

add_executable(simrun Tools/simrun/main.cpp)
target_link_libraries(simrun PRIVATE colonysim colonysim_thirdparty)
target_compile_definitions(simrun PRIVATE COLONY_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../data")

enable_testing()
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS Tests/*.cpp)
add_executable(colonysim_tests ${TEST_SOURCES})
target_link_libraries(colonysim_tests PRIVATE colonysim colonysim_thirdparty)
target_compile_definitions(colonysim_tests PRIVATE COLONY_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../data")
add_test(NAME unit COMMAND colonysim_tests)
add_test(NAME simrun_smoke COMMAND simrun --seed 1 --size 64 --days 1 --png ${CMAKE_BINARY_DIR}/smoke.png)
add_test(NAME simrun_regions COMMAND simrun --seed 1 --size 128 --days 0 --regions ${CMAKE_BINARY_DIR}/regions.png)
add_test(NAME simrun_sight COMMAND simrun --seed 1 --size 128 --days 0 --sight ${CMAKE_BINARY_DIR}/sight.png)

# Replays: play the scripted scenario and record it, then play the recording and check every
# state hash matches. CI compares the recordings from Linux and Windows (docs/design/bridge.md).
set(REPLAY_TEST_DATA ${CMAKE_CURRENT_SOURCE_DIR}/Tests/data)
set(REPLAY_RECORDED ${CMAKE_BINARY_DIR}/haul-and-build.recorded.json)
add_test(NAME replay_record COMMAND simrun --data ${REPLAY_TEST_DATA}
         --replay ${CMAKE_CURRENT_SOURCE_DIR}/Tests/replays/haul-and-build.json --record ${REPLAY_RECORDED})
add_test(NAME replay_check COMMAND simrun --data ${REPLAY_TEST_DATA} --replay ${REPLAY_RECORDED})
set_tests_properties(replay_record PROPERTIES FIXTURES_SETUP replay_recording)
set_tests_properties(replay_check PROPERTIES FIXTURES_REQUIRED replay_recording)

# Saves: run 0.05 days and save, then load that and run 0.05 more and save, and compare with a
# straight run of 0.1 days. The two files must be identical, byte for byte. CI also compares the
# straight run's save from Linux and Windows (docs/design/saves.md).
set(SAVE_RUN_ARGS --data ${REPLAY_TEST_DATA} --seed 7 --size 128 --pawns 8 --items 80)
add_test(NAME save_first_half COMMAND simrun ${SAVE_RUN_ARGS} --days 0.05 --save ${CMAKE_BINARY_DIR}/first-half.save)
add_test(NAME save_second_half COMMAND simrun --data ${REPLAY_TEST_DATA} --load ${CMAKE_BINARY_DIR}/first-half.save
         --days 0.05 --save ${CMAKE_BINARY_DIR}/loaded.save --save-json ${CMAKE_BINARY_DIR}/loaded.save.json)
add_test(NAME save_straight_run COMMAND simrun ${SAVE_RUN_ARGS} --days 0.1 --save ${CMAKE_BINARY_DIR}/straight.save)
add_test(NAME save_compare COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_BINARY_DIR}/loaded.save ${CMAKE_BINARY_DIR}/straight.save)
set_tests_properties(save_first_half PROPERTIES FIXTURES_SETUP save_first_half)
set_tests_properties(save_second_half PROPERTIES FIXTURES_REQUIRED save_first_half FIXTURES_SETUP save_loaded)
set_tests_properties(save_straight_run PROPERTIES FIXTURES_SETUP save_straight)
set_tests_properties(save_compare PROPERTIES FIXTURES_REQUIRED "save_loaded;save_straight")
