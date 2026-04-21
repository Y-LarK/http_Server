cmake_minimum_required(VERSION 3.10)

project(http_server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2")

include_directories(${PROJECT_SOURCE_DIR}/src)

add_executable(http_server
    src/main.cpp
)

target_link_libraries(http_server pthread)


