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
  target_compile_options(colonysim_flags INTERFACE /W4 /WX /permissive- /GR- /EHs-c- /external:W0
                                              /bigobj)  # Save.cpp has more sections than a plain object file allows (UBT uses /bigobj too)
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

add_executable(simrun Tools/simrun/main.cpp Tools/simrun/ColonyScenario.cpp Tools/simrun/PowerScenario.cpp)
target_link_libraries(simrun PRIVATE colonysim colonysim_thirdparty)
target_compile_definitions(simrun PRIVATE COLONY_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../data")

enable_testing()
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS Tests/*.cpp)
add_executable(colonysim_tests ${TEST_SOURCES})
target_link_libraries(colonysim_tests PRIVATE colonysim colonysim_thirdparty)
target_compile_definitions(colonysim_tests PRIVATE COLONY_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../data"
                           COLONY_TEST_SAVES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Tests/saves")
add_test(NAME unit COMMAND colonysim_tests)
add_test(NAME simrun_smoke COMMAND simrun --seed 1 --size 64 --days 1 --png ${CMAKE_BINARY_DIR}/smoke.png)
add_test(NAME simrun_regions COMMAND simrun --seed 1 --size 128 --days 0 --regions ${CMAKE_BINARY_DIR}/regions.png)
add_test(NAME simrun_plants COMMAND simrun --seed 42 --size 256 --days 3 --check --png ${CMAKE_BINARY_DIR}/plants.png)
# Light and darkness (E3): a roofed room, lamps and a campfire at night; the picture is the check.
add_test(NAME simrun_light COMMAND simrun --seed 42 --size 128 --days 0 --light ${CMAKE_BINARY_DIR}/light.png --hour 22)
add_test(NAME simrun_sight COMMAND simrun --seed 1 --size 128 --days 0 --sight ${CMAKE_BINARY_DIR}/sight.png)
# Mining, chopping and harvesting (C3): the Crashlanded colonists work the marked rock and plants for two days.
add_test(NAME simrun_gather COMMAND simrun --seed 42 --size 256 --days 2 --start Crashlanded --gather 60 --check
         --png ${CMAKE_BINARY_DIR}/gather.png)
# Food, rest and schedules (C6): the Crashlanded colonists live three days on their starting food, nobody dies.
add_test(NAME simrun_needs COMMAND simrun --seed 42 --size 128 --days 3 --start Crashlanded --check)
# Raids (F6): pirates walk in, the alarm sounds, the armed colonists fight, and the raid ends
# (beaten, routed or gone) within a quarter of a day.
add_test(NAME simrun_raid COMMAND simrun --seed 1 --size 256 --days 0.25 --start Crashlanded --raid 300 --armed --check)

# Temperature (E2, docs/design/temperature.md): the Crashlanded colonists live two days in 4 degrees weather
# (just above the safe range their T-shirts and pants give them) and nobody falls ill or dies.
add_test(NAME simrun_temperature COMMAND simrun --seed 42 --size 128 --days 2 --start Crashlanded --outdoor 4 --check)

# The colony scenario (C10, docs/design/colony-scenario.md): six colonists build a room, mine, chop, pick berries and
# cook for five days; fails if anyone dies or starves, stands stuck for a game hour, or a mark is left undone.
# A second seed, because the first run on seed 7 found a bug seed 42 did not.
add_test(NAME colony_scenario COMMAND simrun --scenario colony --seed 42)
add_test(NAME colony_scenario_seed7 COMMAND simrun --scenario colony --seed 7)

# The power check (E5, docs/design/power.md): solar, batteries, lamps and a switch for three days.
add_test(NAME power_check COMMAND simrun --power)

# Replays: play the scripted scenario and record it, then play the recording and check every
# state hash matches. CI compares the recordings from Linux and Windows (docs/design/bridge.md).
set(REPLAY_TEST_DATA ${CMAKE_CURRENT_SOURCE_DIR}/Tests/data)
set(REPLAY_RECORDED ${CMAKE_BINARY_DIR}/haul-and-build.recorded.json)
add_test(NAME replay_record COMMAND simrun --data ${REPLAY_TEST_DATA}
         --replay ${CMAKE_CURRENT_SOURCE_DIR}/Tests/replays/haul-and-build.json --record ${REPLAY_RECORDED})
add_test(NAME replay_check COMMAND simrun --data ${REPLAY_TEST_DATA} --replay ${REPLAY_RECORDED})
set_tests_properties(replay_record PROPERTIES FIXTURES_SETUP replay_recording)
set_tests_properties(replay_check PROPERTIES FIXTURES_REQUIRED replay_recording)

# The same for building (C4), on the game's real data: blueprints, deliveries, frames, a deconstruct.
set(BUILD_REPLAY_RECORDED ${CMAKE_BINARY_DIR}/colony-build.recorded.json)
add_test(NAME replay_build_record COMMAND simrun
         --replay ${CMAKE_CURRENT_SOURCE_DIR}/Tests/replays/colony-build.json --record ${BUILD_REPLAY_RECORDED})
add_test(NAME replay_build_check COMMAND simrun --replay ${BUILD_REPLAY_RECORDED})
set_tests_properties(replay_build_record PROPERTIES FIXTURES_SETUP build_replay_recording)
set_tests_properties(replay_build_check PROPERTIES FIXTURES_REQUIRED build_replay_recording)

# And for rooms (C5): colonists wall in a room with a door, a bed and a table, walking through the
# door as they go; the room ends up roofed with a bedroom role.
set(ROOMS_REPLAY_RECORDED ${CMAKE_BINARY_DIR}/colony-rooms.recorded.json)
add_test(NAME replay_rooms_record COMMAND simrun
         --replay ${CMAKE_CURRENT_SOURCE_DIR}/Tests/replays/colony-rooms.json --record ${ROOMS_REPLAY_RECORDED})
add_test(NAME replay_rooms_check COMMAND simrun --replay ${ROOMS_REPLAY_RECORDED})
set_tests_properties(replay_rooms_record PROPERTIES FIXTURES_SETUP rooms_replay_recording)
set_tests_properties(replay_rooms_check PROPERTIES FIXTURES_REQUIRED rooms_replay_recording)

# And for bills and cooking (C7): a stove with bills in every mode, cooks fetching berries.
set(COOK_REPLAY_RECORDED ${CMAKE_BINARY_DIR}/colony-cook.recorded.json)
add_test(NAME replay_cook_record COMMAND simrun
         --replay ${CMAKE_CURRENT_SOURCE_DIR}/Tests/replays/colony-cook.json --record ${COOK_REPLAY_RECORDED})
add_test(NAME replay_cook_check COMMAND simrun --replay ${COOK_REPLAY_RECORDED})
set_tests_properties(replay_cook_record PROPERTIES FIXTURES_SETUP cook_replay_recording)
set_tests_properties(replay_cook_check PROPERTIES FIXTURES_REQUIRED cook_replay_recording)

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

# Wild animals (N1, docs/design/animal-life.md): herds by biome graze, sleep and walk for two days; fails if
# none were placed, one stands on ground it can't walk, or the wild herd falls under half its density.
# (The 1 km run with about 300 animals, `--size 1024 --days 1 --animals`, is for the nightly job.)
add_test(NAME simrun_animals COMMAND simrun --seed 42 --size 512 --days 2 --animals --check
         --png ${CMAKE_BINARY_DIR}/animals.png)

# A chicken farm (N4, docs/design/breeding.md): tame hens and roosters mate, lay and cost the breeding pass next to nothing.
# (`--farm 500 --days 3` is the nightly one; 100 chickens over a day and a half keep CI quick.)
add_test(NAME simrun_farm COMMAND simrun --seed 42 --size 128 --days 1.6 --farm 100 --check)

# Pathfinding load test (S3): 100 pawns hauling on a 1 km map for a day, no failed job or search.
add_test(NAME simrun_paths COMMAND simrun --seed 42 --size 1024 --days 1 --pawns 100 --items 2000 --check
         --data ${CMAKE_CURRENT_SOURCE_DIR}/Tests/data)
