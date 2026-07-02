cmake_minimum_required(VERSION 3.15)
project(exi_demo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Resolved from the Conan-generated package config (exificientConfig.cmake).
find_package(exificient CONFIG REQUIRED)

add_executable(exi-demo exi_demo.cpp)
target_link_libraries(exi-demo PRIVATE exificient::exificient)
