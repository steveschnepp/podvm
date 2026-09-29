cmake_minimum_required(VERSION 3.10)
project(podvm C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler flags
add_compile_options(-Wall -Wextra -O2)

# Main executable
add_executable(podvm src/main.c)

# Find libcurl (for future registry support)
find_package(CURL)
if(CURL_FOUND)
    target_link_libraries(podvm ${CURL_LIBRARIES})
    target_include_directories(podvm PRIVATE ${CURL_INCLUDE_DIRS})
endif()

# Install
install(TARGETS podvm DESTINATION bin)
install(DIRECTORY kernel/ DESTINATION var/lib/podvm/kernel)
