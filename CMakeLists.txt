cmake_minimum_required(VERSION 3.16)
project(Selfmate VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_SOURCE_DIR}/bin")
endforeach()

find_package(Threads REQUIRED)

# Reuse Stockfish's standard chess rules, without its normal-chess search,
# evaluation, NNUE network, UCI frontend, TT, or Syzygy probes.
add_library(stockfish_rules STATIC
    vendor/stockfish/src/bitboard.cpp
    vendor/stockfish/src/movegen.cpp
    src/stockfish_position.cpp
)
target_include_directories(stockfish_rules PUBLIC vendor/stockfish/src)
target_compile_definitions(stockfish_rules PUBLIC NO_PREFETCH)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(stockfish_rules PUBLIC IS_64BIT)
endif()

add_library(selfmate_core STATIC src/engine.cpp src/search.cpp)
target_include_directories(selfmate_core PUBLIC src)
target_link_libraries(selfmate_core PUBLIC stockfish_rules Threads::Threads)

add_executable(selfmate src/main.cpp)
target_link_libraries(selfmate PRIVATE selfmate_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(selfmate_tests tests/engine_tests.cpp)
    target_link_libraries(selfmate_tests PRIVATE selfmate_core)
    add_test(NAME engine_tests COMMAND selfmate_tests)
    add_executable(selfmate_search_tests tests/search_tests.cpp)
    target_link_libraries(selfmate_search_tests PRIVATE selfmate_core)
    add_test(NAME search_tests COMMAND selfmate_search_tests)
endif()
