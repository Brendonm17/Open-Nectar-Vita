cmake_minimum_required(VERSION 3.16)
project(nectar_paths CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(nectar_paths STATIC src/paths.cpp)
set_property(TARGET nectar_paths PROPERTY INCLUDE_DIRECTORIES "")
target_include_directories(nectar_paths PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(NOT MSVC)
    target_compile_options(nectar_paths PRIVATE -Wall -Wextra)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_executable(nectar_paths_test tests/paths_test.cpp)
    target_link_libraries(nectar_paths_test PRIVATE nectar_paths)
    add_test(NAME nectar_paths_test COMMAND nectar_paths_test)
endif()
