cmake_minimum_required(VERSION 3.19)
project(
        sts_lightspeed
        VERSION 0.1
        DESCRIPTION "High Performance, rng accurate implementation of Slay the Spire with tools for tree search and ml."
        LANGUAGES CXX)

file (GLOB_RECURSE sts_lightspeed_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS  "-Wno-shift-count-overflow -O3")

add_subdirectory(json)
find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(slaythespire bindings/slaythespire.cpp bindings/bindings-util.cpp ${sts_lightspeed_SOURCES})
target_include_directories(slaythespire PUBLIC include)
target_include_directories(slaythespire PUBLIC json/single_include)
target_include_directories(slaythespire PUBLIC bindings)


add_executable(main apps/main.cpp ${sts_lightspeed_SOURCES})
target_link_directories(main PRIVATE json::nlohmann_json)
target_include_directories(main PUBLIC include)
target_include_directories(main PUBLIC json/include)

add_executable(test apps/test.cpp ${sts_lightspeed_SOURCES})
target_include_directories(test PUBLIC include)
target_link_directories(test PUBLIC json::nlohmann_json)
target_include_directories(test PUBLIC json/include)

add_executable(small-test apps/small-test.cpp bindings/bindings-util.cpp ${sts_lightspeed_SOURCES})
target_link_directories(small-test PRIVATE json::nlohmann_json)
target_include_directories(small-test PUBLIC include)
target_include_directories(small-test PUBLIC json/include)
target_include_directories(small-test PUBLIC bindings)

# Other classes' cards checked against what the game's bytecode does.
add_executable(test_defect_cards EXCLUDE_FROM_ALL apps/test_defect_cards.cpp)
target_include_directories(test_defect_cards PUBLIC include json/single_include)
target_link_libraries(test_defect_cards stscore)

# ---- research additions (leak-free agent + frozen eval) ----
set(STS_RESEARCH_DIR /home/user/sts/research CACHE PATH "research/ directory of the latioswang/sts checkout")
add_library(stscore STATIC ${sts_lightspeed_SOURCES})
target_include_directories(stscore PUBLIC include json/single_include)
set_property(TARGET stscore PROPERTY POSITION_INDEPENDENT_CODE ON)
# NOTE: sts_card_manager_use_fixed_list (fixed-size piles, ~10% faster) was tried and REVERTED:
# piles can exceed any fixed capacity in degenerate rollouts (e.g. Dead Branch + Corruption) and the
# Release library has asserts compiled out -> silent stack corruption (3/300 val games crashed).
file(GLOB FAIR_SOURCES CONFIGURE_DEPENDS "${STS_RESEARCH_DIR}/cpp/*.cpp")
add_executable(fair_eval ${STS_RESEARCH_DIR}/eval/eval_main.cpp ${FAIR_SOURCES})
target_include_directories(fair_eval PUBLIC include ${STS_RESEARCH_DIR}/cpp ${STS_RESEARCH_DIR}/vf)
target_link_libraries(fair_eval stscore pthread)

# ---- real-game bridge (research/realgame) ----
if(EXISTS "${STS_RESEARCH_DIR}/realgame/bridge_main.cpp")
  file(GLOB BRIDGE_SOURCES CONFIGURE_DEPENDS "${STS_RESEARCH_DIR}/realgame/*.cpp")
  add_executable(sts_bridge ${BRIDGE_SOURCES} ${FAIR_SOURCES})
  target_include_directories(sts_bridge PUBLIC include ${STS_RESEARCH_DIR}/realgame ${STS_RESEARCH_DIR}/cpp ${STS_RESEARCH_DIR}/vf)
  target_link_libraries(sts_bridge stscore pthread)
endif()
