cmake_minimum_required(VERSION 3.21)
project(omarchy-mobile-browser LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS DBus Gui Qml Quick WebEngineCore WebEngineQuick)
add_executable(omarchy-mobile-browser main.cpp)
target_link_libraries(omarchy-mobile-browser PRIVATE
    Qt6::DBus Qt6::Gui Qt6::Qml Qt6::Quick Qt6::WebEngineCore Qt6::WebEngineQuick)
