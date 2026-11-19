# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Layering: application -> middleware, never the other way round.
# tests/ holds the hardware tests run by main (HW_TEST in hw_tests.h).
set(EXTRA_COMPONENT_DIRS middleware application tests)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(con-venience)
