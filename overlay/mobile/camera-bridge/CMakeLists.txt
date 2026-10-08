cmake_minimum_required(VERSION 3.21)
project(omarchy-camera-bridge LANGUAGES CXX)

# The phone's libcamera (devices/pixel7pro, built into /usr/local).
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED IMPORTED_TARGET libcamera)
set(CMAKE_CXX_STANDARD 20)
add_executable(omarchy-camera-bridge camera-bridge.cpp)
target_compile_options(omarchy-camera-bridge PRIVATE -O3 -Wall)
target_link_libraries(omarchy-camera-bridge PRIVATE PkgConfig::LIBCAMERA)
install(TARGETS omarchy-camera-bridge)
