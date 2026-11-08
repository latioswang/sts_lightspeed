cmake_minimum_required(VERSION 3.19)
project(
        sts_lightspeed
        VERSION 0.1
        DESCRIPTION "High Performance, rng accurate implementation of Slay the Spire with tools for tree search and ml."
        LANGUAGES CXX)

file (GLOB_RECURSE sts_lightspeed_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS  "-Wno-shift-count-overflow -O3")

# nlohmann/json is used as headers only (json/include, json/single_include), so its own CMake project, which
# CMake 4 refuses for its old minimum version, isn't added.

# The Python module takes pybind11 from pip, since the old pybind11 submodule's CMake project has the same minimum
# version problem: pip install pybind11, then -Dpybind11_DIR=$(python3 -m pybind11 --cmakedir). Without it, the
# module is skipped and the C++ programs still build.
find_package(pybind11 CONFIG QUIET)
if(pybind11_FOUND)
  pybind11_add_module(slaythespire bindings/slaythespire.cpp bindings/bindings-util.cpp ${sts_lightspeed_SOURCES})
  target_include_directories(slaythespire PUBLIC include)
  target_include_directories(slaythespire PUBLIC json/single_include)
  target_include_directories(slaythespire PUBLIC bindings)
else()
  message(STATUS "pybind11 not found, skipping the slaythespire Python module "
                 "(pip install pybind11, then -Dpybind11_DIR=$(python3 -m pybind11 --cmakedir))")
endif()


add_executable(main apps/main.cpp ${sts_lightspeed_SOURCES})
target_include_directories(main PUBLIC include)
target_include_directories(main PUBLIC json/include)

add_executable(test apps/test.cpp ${sts_lightspeed_SOURCES})
target_include_directories(test PUBLIC include)
target_include_directories(test PUBLIC json/include)

add_executable(small-test apps/small-test.cpp bindings/bindings-util.cpp ${sts_lightspeed_SOURCES})
target_include_directories(small-test PUBLIC include)
target_include_directories(small-test PUBLIC json/include)
target_include_directories(small-test PUBLIC bindings)

# Other classes' cards checked against what the game's bytecode does.
add_executable(test_defect_cards EXCLUDE_FROM_ALL apps/test_defect_cards.cpp)
target_include_directories(test_defect_cards PUBLIC include json/single_include)
target_link_libraries(test_defect_cards stscore)
add_executable(test_silent_cards EXCLUDE_FROM_ALL apps/test_silent_cards.cpp)
target_include_directories(test_silent_cards PUBLIC include json/single_include)
target_link_libraries(test_silent_cards stscore)
add_executable(test_watcher_cards EXCLUDE_FROM_ALL apps/test_watcher_cards.cpp)
target_include_directories(test_watcher_cards PUBLIC include json/single_include)
target_link_libraries(test_watcher_cards stscore)
add_executable(test_class_relics EXCLUDE_FROM_ALL apps/test_class_relics.cpp)
target_include_directories(test_class_relics PUBLIC include json/single_include)
target_link_libraries(test_class_relics stscore)

# ---- research additions (leak-free agent + frozen eval) ----
set(STS_RESEARCH_DIR /home/user/sts/research CACHE PATH "research/ directory of the latioswang/sts checkout")
add_library(stscore STATIC ${sts_lightspeed_SOURCES})
target_include_directories(stscore PUBLIC include json/single_include)
set_property(TARGET stscore PROPERTY POSITION_INDEPENDENT_CODE ON)
# NOTE: sts_card_manager_use_fixed_list (fixed-size piles, ~10% faster) was tried and REVERTED:
# piles can exceed any fixed capacity in degenerate rollouts (e.g. Dead Branch + Corruption) and the
# Release library has asserts compiled out -> silent stack corruption (3/300 val games crashed).
# fair_eval and sts_bridge come from the research tree of latioswang/sts; a clone of this repo alone skips them.
if(EXISTS "${STS_RESEARCH_DIR}/eval/eval_main.cpp")
  file(GLOB FAIR_SOURCES CONFIGURE_DEPENDS "${STS_RESEARCH_DIR}/cpp/*.cpp")
  add_executable(fair_eval ${STS_RESEARCH_DIR}/eval/eval_main.cpp ${FAIR_SOURCES})
  target_include_directories(fair_eval PUBLIC include ${STS_RESEARCH_DIR}/cpp ${STS_RESEARCH_DIR}/vf)
  target_link_libraries(fair_eval stscore pthread)
else()
  message(STATUS "No research tree at STS_RESEARCH_DIR=${STS_RESEARCH_DIR}, skipping fair_eval and sts_bridge")
endif()

# ---- real-game bridge (research/realgame) ----
if(EXISTS "${STS_RESEARCH_DIR}/realgame/bridge_main.cpp")
  file(GLOB BRIDGE_SOURCES CONFIGURE_DEPENDS "${STS_RESEARCH_DIR}/realgame/*.cpp")
  add_executable(sts_bridge ${BRIDGE_SOURCES} ${FAIR_SOURCES})
  target_include_directories(sts_bridge PUBLIC include ${STS_RESEARCH_DIR}/realgame ${STS_RESEARCH_DIR}/cpp ${STS_RESEARCH_DIR}/vf)
  target_link_libraries(sts_bridge stscore pthread)
endif()

# Link-time optimisation for the simulator library and the research programs, in Release builds only. CMake picks the
# flags for the compiler (-flto=thin for Clang, -flto=auto for GCC, with gcc-ar for the static library). It changes no
# result: no -march, no fast-math, no FMA contraction beyond the compiler's default. -DSTS_LTO=OFF turns it off.
option(STS_LTO "link-time optimisation for the simulator's library and programs in Release builds" ON)
if(STS_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT STS_LTO_SUPPORTED OUTPUT STS_LTO_ERROR LANGUAGES CXX)
  if(STS_LTO_SUPPORTED)
    foreach(t stscore fair_eval sts_bridge main test small-test slaythespire)
      if(TARGET ${t})
        set_property(TARGET ${t} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
      endif()
    endforeach()
  else()
    message(WARNING "link-time optimisation not supported here, building without it: ${STS_LTO_ERROR}")
  endif()
endif()
