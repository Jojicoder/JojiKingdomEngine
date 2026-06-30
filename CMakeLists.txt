cmake_minimum_required(VERSION 3.25)
project(JojiKingdomEngine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/CompilerFlags.cmake)

# ── Simulation library ────────────────────────────────────────────────────────
file(GLOB_RECURSE JKE_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_library(jke_sim STATIC ${JKE_SOURCES})

target_include_directories(jke_sim PUBLIC include)
target_compile_options(jke_sim PRIVATE ${JKE_COMPILE_FLAGS})

# ── CLI runner ────────────────────────────────────────────────────────────────
add_executable(jke_run tools/run_simulation.cpp)
target_link_libraries(jke_run PRIVATE jke_sim)
target_compile_options(jke_run PRIVATE ${JKE_COMPILE_FLAGS})

add_executable(jke_balance tools/balance_runner.cpp)
target_link_libraries(jke_balance PRIVATE jke_sim)
target_compile_options(jke_balance PRIVATE ${JKE_COMPILE_FLAGS})

# ── SFML Viewer (optional) ────────────────────────────────────────────────────
find_package(SFML 3 COMPONENTS Graphics Window System)
if(SFML_FOUND)
    add_executable(jke_viewer viewer/main.cpp)
    target_link_libraries(jke_viewer PRIVATE jke_sim SFML::Graphics SFML::Window SFML::System)
    target_compile_options(jke_viewer PRIVATE ${JKE_COMPILE_FLAGS})
    message(STATUS "SFML found — building jke_viewer")
else()
    message(STATUS "SFML not found — skipping jke_viewer")
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)
