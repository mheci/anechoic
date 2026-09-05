cmake_minimum_required(VERSION 3.16)
project(AnechoicCore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CORE_SRC
        include/anechoic/SpeechEnhancer.h
        src/SpeechEnhancer.cpp)

add_library(AnechoicCore STATIC ${CORE_SRC})

set(LIBRARIES RnNoise)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    list(APPEND LIBRARIES atomic)
endif()

target_link_libraries(AnechoicCore ${LIBRARIES})

target_include_directories(AnechoicCore PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        PRIVATE src)

if (BUILD_TESTS)
    set(TESTS_SRC
            src/tests/tests.cpp)
    add_executable(common_plugin_tests ${TESTS_SRC})
    target_include_directories(common_plugin_tests PRIVATE
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/external/catch2>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_link_libraries(common_plugin_tests PRIVATE AnechoicCore ${LIBRARIES})
    if (NOT MSVC)
        target_compile_options(common_plugin_tests PRIVATE -fsanitize=undefined)
        target_link_options(common_plugin_tests PRIVATE -fsanitize=undefined)
    endif ()

    include(CTest)
    include(Catch)
    catch_discover_tests(common_plugin_tests)
endif ()