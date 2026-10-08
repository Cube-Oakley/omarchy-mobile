cmake_minimum_required(VERSION 3.21)
project(omarchy-link LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network DBus)

add_executable(omarchy-link
    src/main.cpp
    src/config.cpp
    src/lan.cpp
    src/device.cpp
    src/daemon.cpp
    src/notifier.cpp
    src/theme.cpp
    src/phone.cpp
    src/desktop.cpp
)
target_link_libraries(omarchy-link PRIVATE Qt6::Core Qt6::Network Qt6::DBus)
install(TARGETS omarchy-link)
