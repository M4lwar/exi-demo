cmake_minimum_required(VERSION 3.15)
project(exi_demo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Resolved from the Conan-generated package config (exificientConfig.cmake).
find_package(exificient CONFIG REQUIRED)

find_package(Threads REQUIRED)

add_executable(exi-demo exi_demo.cpp)
target_link_libraries(exi-demo PRIVATE exificient::exificient)

# Standalone example: correct GraalVM isolate/threading + async schema load.
add_executable(threading-example threading_example.cpp)
target_link_libraries(threading-example PRIVATE exificient::exificient Threads::Threads)

# The conanfile copies libexificient next to the executable at install time;
# $ORIGIN makes the Linux loader look there. Windows searches the exe's own
# directory natively and ignores this property.
set_target_properties(exi-demo threading-example PROPERTIES BUILD_RPATH "$ORIGIN")
