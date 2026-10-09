cmake_minimum_required(VERSION 3.10)
project(podvm C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler flags
add_compile_options(-Wall -Wextra -O2)

# Main executable
add_executable(podvm src/main.c src/strparse.c src/image.c src/json.c src/store.c src/sha256.c src/imgcfg.c src/container.c)

# Find libcurl (registry), zlib (gzip) and libzstd (zstd layers)
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

# libzstd has no standard Find module; use pkg-config, else probe the lib
# directly. Optional: without it zstd layers fail at link/run, gzip still
# works, so we warn rather than hard-fail.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(ZSTD QUIET libzstd)
endif()
if(ZSTD_FOUND)
    target_link_libraries(podvm ${ZSTD_LIBRARIES})
    target_include_directories(podvm PRIVATE ${ZSTD_INCLUDE_DIRS})
    target_link_directories(podvm PRIVATE ${ZSTD_LIBRARY_DIRS})
else()
    find_library(ZSTD_LIB zstd)
    if(ZSTD_LIB)
        target_link_libraries(podvm ${ZSTD_LIB})
    else()
        message(WARNING "libzstd not found: zstd image layers will not decompress")
    endif()
endif()

# Install
install(TARGETS podvm DESTINATION bin)
install(DIRECTORY kernel/ DESTINATION var/lib/podvm/kernel)
