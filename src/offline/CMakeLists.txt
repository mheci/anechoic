cmake_minimum_required(VERSION 3.16)
project(anechoic_offline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(anechoic_offline offline_main.cpp)

set_target_properties(anechoic_offline PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

target_include_directories(anechoic_offline PRIVATE
        ${CMAKE_SOURCE_DIR}/src/common/include)

target_link_libraries(anechoic_offline PRIVATE AnechoicCore RnNoise)

include(GNUInstallDirs)
install(TARGETS anechoic_offline RUNTIME DESTINATION bin)