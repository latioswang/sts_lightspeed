# Using sts_lightspeed

What you can do with this simulator, from the quickest start to running against the real game. Every command here was run from a fresh clone on 2026-11-09.

| You want to | You need | Go to |
|---|---|---|
| Play runs in a terminal, or script them from Python or C++ | A C++17 compiler and CMake | [Build](#build), [Console](#play-in-the-console), [Python](#python), [C++](#c) |
| Train or test an agent on a simulator that plays like the game | The same | [Python](#python), [C++](#c), [Benchmarks](#benchmarks) |
| Check your own simulator change against 1,765 real runs | Also [latioswang/sts](https://github.com/latioswang/sts) and `zstd` | [Check a change](#check-a-change-against-1765-real-runs) |
| Play seeds on the real game and in the simulator side by side | Also your own copy of Slay the Spire, with three mods | [Real game](#run-against-the-real-game) |

The simulator and the tools that check it live in two repositories:

- **[latioswang/sts_lightspeed](https://github.com/latioswang/sts_lightspeed)** (this one): the simulator. It works on its own.
- **[latioswang/sts](https://github.com/latioswang/sts)**, under `research/`: the agent, the bridge to the real game, and the 1,765 recorded real runs. The bridge is built through this repository's CMake, so it needs both.

## Build

You need CMake 3.19 or newer (CMake 4 works) and a C++17 compiler. This was checked on macOS on Apple silicon with Apple clang 21 and CMake 4.3. The research loop builds an earlier commit of it on x86-64 Linux with GCC. Windows hasn't been tried since upstream's MinGW setup.

```bash
git clone --recurse-submodules https://github.com/latioswang/sts_lightspeed.git
cd sts_lightspeed
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
```

This takes a minute or two and builds:

| Target | What it is |
|---|---|
| `main` | The console game |
| `test` | Benchmarks and tools: agent runs, replaying an action list, save file to JSON |
| `stscore` | The simulator as a static library, for your own programs |
| `slaythespire` | The Python module, built only if CMake finds pybind11 (see [Python](#python)) |
| `test_defect_cards`, `test_silent_cards`, `test_watcher_cards`, `test_class_relics` | Checks of the other characters' cards and relics against the game's rules. Not built by default: `cmake --build build --target test_silent_cards` |

Release builds use link-time optimisation. Add `-DSTS_LTO=OFF` for a faster build and a slower simulator.

## Play in the console

```bash
./build/main
```

Type a seed, a character (`I`, `S`, `D` or `W`) and an ascension, such as `LIGHTSPEED W 0`. The seed is the text the game shows, so you can play the same run as in the game. Then pick each option by its number. Outside combat, `print deck`, `print relics` and `print all` show the state, and `set gold 999` and `set hp 1` change it. In combat, the console lists the moves by number, with `drink N` and `discard N` for potions. `quit` ends the run.

## Python

The module takes pybind11 from pip:

```bash
python3 -m pip install pybind11
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -Dpybind11_DIR=$(python3 -m pybind11 --cmakedir) -DPYTHON_EXECUTABLE=$(which python3)
cmake --build build --target slaythespire -j 8
```

Then, with `build` on the path:

```python
import slaythespire as sts

gc = sts.GameContext(sts.CharacterClass.SILENT, sts.get_seed_long("LIGHTSPEED"), 0)
print(gc.cur_hp, gc.max_hp, gc.gold, len(gc.deck), gc.boss)   # 70 70 99 12 MonsterEncounter.HEXAGHOST

agent = sts.Agent()               # upstream's tree search agent
agent.simulation_count_base = 200
agent.playout(gc)                 # plays the whole run
print(gc.outcome, gc.floor_num)

obs = sts.getNNInterface().getObservation(gc)   # 412 numbers describing the run, for a neural network
```

The observation is upstream's, and it encodes Ironclad's and colorless cards only: every other character's cards land in one slot.

```bash
PYTHONPATH=build python3 my_script.py
```

The Python API is upstream's: runs, card rewards, the deck, relics, the map and an observation vector. Combat is played by the built-in agent rather than step by step. `sts.Agent` is upstream's search agent, which searches with the game's own random numbers, so it knows what will happen. Use it to play runs and generate data, not as a fair opponent. A leak-free agent that only uses what a player can see is in [latioswang/sts](https://github.com/latioswang/sts) as `research/cpp/FairAgent`. More of the game in Python would be a welcome contribution.

## C++

Put this repository in your project (as a submodule, say) and link `stscore`:

```cmake
cmake_minimum_required(VERSION 3.19)
project(myagent LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
add_subdirectory(sts_lightspeed EXCLUDE_FROM_ALL)
add_executable(myagent main.cpp)
target_link_libraries(myagent stscore)
```

```cpp
#include <iostream>

#include "game/Game.h"
#include "game/GameContext.h"
#include "sim/search/SimpleAgent.h"

using namespace sts;

int main() {
    int wins = 0;
    double floors = 0;
    for (int i = 0; i < 1000; ++i) {
        GameContext gc(CharacterClass::IRONCLAD, SeedHelper::getLong("LIGHTSPEED") + i, 0);
        search::SimpleAgent agent;   // a quick rule-based player
        agent.playout(gc);
        wins += gc.outcome == GameOutcome::PLAYER_VICTORY;
        floors += gc.floorNum;
    }
    std::cout << "wins " << wins << " of 1000, average floor " << floors / 1000 << '\n';
}
```

That plays 1,000 whole runs in about 0.15 seconds on one core. For your own agent, `GameContext` is the run outside combat, and `BattleContext` (`include/combat/BattleContext.h`) is a fight, built from a `GameContext` when one starts. `search::GameAction` and `search::Action` list and take the legal moves. `src/sim/search/SimpleAgent.cpp` and `ScumSearchAgent2.cpp` show how a run is driven from start to end.

## Benchmarks

Upstream's own benchmarks are in `test`, on one thread:

```bash
./build/test simple_agent_mt 1 100000 5000      # threads, first seed, runs: a rule-based player
./build/test agent_mt 1 200 0 5000 120 0        # threads, simulations per move, ascension, first seed, runs, print level: tree search
```

To compare with upstream, build it the same way. With CMake 4 it needs `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`, and `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` gives it link-time optimisation too. Time several interleaved rounds and keep the fastest, since one run varies a lot on a busy machine. Compare floors per second, because the two simulators play differently and runs end on different floors. The results on an Apple M3 are in the [README](README.md#speed).

## Check a change against 1,765 real runs

No game needed. The recordings are in [latioswang/sts](https://github.com/latioswang/sts), and the bridge plays each one back through this simulator and compares every state with what the game showed.

```bash
git clone https://github.com/latioswang/sts.git
git clone --recurse-submodules https://github.com/latioswang/sts_lightspeed.git
cd sts_lightspeed
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSTS_RESEARCH_DIR=$PWD/../sts/research
cmake --build build --target sts_bridge -j 8
cd ../sts
python3 -I research/realgame/tools/recordings.py unpack
python3 -I research/realgame/tools/run_regression.py --bridge ../sts_lightspeed/build/sts_bridge
```

`recordings.py unpack` needs `zstd` on the path (`brew install zstd` or `apt install zstd`) and takes a few minutes. The replay takes a minute or two on 4 cores, longer on a busy machine, and on today's simulator ends with:

```
1765 recordings in ...: 1466 pass, 251 limited, 48 fail
commands checked: 754533 of 823344
by status: completed 1382, game_nondeterminism 161, trace_end 84, diverged 82, trace_mismatch 39, no_rng 15, past_real_stop 2
known failures still failing: 48
```

It exits 1 if any run got worse than `research/realgame/regression_baseline.tsv`, and `research/realgame/logs/replay_results.jsonl` says where each run stopped and why. A fix to the simulator should make runs get further. Make your change, rebuild `sts_bridge`, and run it again. `--only 20261107` replays one day's runs, for a quicker check.

## Run against the real game

You need your own copy of Slay the Spire on Steam, with ModTheSpire, BaseMod and [Communication Mod](https://github.com/ForgottenArbiter/CommunicationMod). The setup scripts were written for an Apple silicon Mac. Everything else is in the bridge's [README](https://github.com/latioswang/sts/blob/HEAD/research/realgame/README.md#setup) and [USAGE](https://github.com/latioswang/sts/blob/HEAD/research/realgame/USAGE.md): mirror mode, which plays a seed on both and stops at the first difference, and live mode, which lets the agent play the game.

## Contributing

Forks and pull requests are welcome. See [Contributing](README.md#contributing) for what a change needs. In short: evidence from the game for every change in behaviour, a run of the replay test before and after, and no decompiled code.
