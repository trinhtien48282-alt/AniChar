cmake_minimum_required(VERSION 3.20)

project(AniChar LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
find_package(SQLite3 REQUIRED)

add_library(anichar_engine src/color_engine.cpp)
target_include_directories(anichar_engine PUBLIC include)
target_link_libraries(anichar_engine PUBLIC Qt6::Core)

add_library(anichar_cache src/cache.cpp)
target_include_directories(anichar_cache PUBLIC include)
target_link_libraries(anichar_cache PUBLIC anichar_engine Qt6::Core SQLite3::SQLite3)

add_library(anichar_detector src/hair_detector.cpp)
target_include_directories(anichar_detector PUBLIC include)
target_link_libraries(anichar_detector PUBLIC anichar_engine Qt6::Gui)

add_library(anichar_online src/online_search.cpp)
target_include_directories(anichar_online PUBLIC include)
target_link_libraries(anichar_online PUBLIC anichar_cache anichar_detector Qt6::Network)

add_executable(AniChar src/main.cpp)
target_link_libraries(AniChar PRIVATE
    anichar_cache
    anichar_detector
    anichar_online
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
)

include(CTest)
if(BUILD_TESTING)
    add_executable(anichar_engine_tests tests/color_engine_tests.cpp)
    target_link_libraries(anichar_engine_tests PRIVATE anichar_engine)
    add_test(NAME anichar_engine_tests COMMAND anichar_engine_tests)

    add_executable(anichar_cache_tests tests/cache_tests.cpp)
    target_link_libraries(anichar_cache_tests PRIVATE anichar_cache)
    add_test(NAME anichar_cache_tests COMMAND anichar_cache_tests)

    add_executable(anichar_detector_tests tests/hair_detector_tests.cpp)
    target_link_libraries(anichar_detector_tests PRIVATE anichar_detector)
    add_test(NAME anichar_detector_tests COMMAND anichar_detector_tests)

    add_executable(anichar_online_tests tests/online_search_tests.cpp)
    target_link_libraries(anichar_online_tests PRIVATE anichar_online Qt6::Network)
    add_test(NAME anichar_online_tests COMMAND anichar_online_tests)

    if(MINGW)
        get_filename_component(MINGW_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set_tests_properties(anichar_engine_tests anichar_cache_tests anichar_detector_tests anichar_online_tests
            PROPERTIES ENVIRONMENT "PATH=${MINGW_RUNTIME_DIR}\\;$ENV{PATH}")
    endif()
endif()
