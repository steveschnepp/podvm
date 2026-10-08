cmake_minimum_required(VERSION 3.10)
project(podvm C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler flags
add_compile_options(-Wall -Wextra -O2)

# Main executable
add_executable(podvm src/main.c src/strparse.c src/image.c src/json.c src/store.c src/sha256.c)

# Find libcurl (registry) and zlib (gzip layer decompression)
find_package(CURL)
if(CURL_FOUND)
    target_link_libraries(podvm ${CURL_LIBRARIES})
    target_include_directories(podvm PRIVATE ${CURL_INCLUDE_DIRS})
endif()

find_package(ZLIB)
if(ZLIB_FOUND)
    target_link_libraries(podvm ${ZLIB_LIBRARIES})
    target_include_directories(podvm PRIVATE ${ZLIB_INCLUDE_DIRS})
endif()

# Install
install(TARGETS podvm DESTINATION bin)
install(DIRECTORY kernel/ DESTINATION var/lib/podvm/kernel)
