cmake_minimum_required(VERSION 3.21)
project(omarchymobiletheme LANGUAGES CXX)

# Uses Qt's private generic Unix theme: rebuild with every Qt update (as
# quickshell is). install.sh builds it on the phone.
find_package(Qt6 REQUIRED COMPONENTS Gui GuiPrivate)
set(CMAKE_AUTOMOC ON)
add_library(omarchymobiletheme MODULE omarchymobiletheme.cpp)
target_link_libraries(omarchymobiletheme PRIVATE Qt6::Gui Qt6::GuiPrivate)
