cmake_minimum_required(VERSION 3.18)
cmake_policy(SET CMP0091 NEW)

project(ombd_runtime LANGUAGES CXX)

# Chrono's installer ships Release libraries only, so Debug builds cannot link.
set(CMAKE_CONFIGURATION_TYPES "Release;RelWithDebInfo" CACHE STRING "" FORCE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ChronoImport)

add_subdirectory(bench)
