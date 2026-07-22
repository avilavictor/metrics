cmake_minimum_required(VERSION 3.10)
project(metrics 
        VERSION 0.1.0
        LANGUAGES C)

set(CMAKE_C_STANDARD 99)

add_executable(metrics ./metrics.c)
target_link_libraries(metrics)

set_target_properties(metrics PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)

message(STATUS "Build configuration:")
message(STATUS "  Project: ${PROJECT_NAME}")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Output directory: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")