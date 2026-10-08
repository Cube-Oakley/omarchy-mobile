# OmarchyCamera: the camera engine as a QML module, on libcamera's public API
# only, so it runs on any phone libcamera supports. Board specifics stay in
# the kernel and the sensor's libcamera tuning file.
cmake_minimum_required(VERSION 3.21)
project(omarchy-camera LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick Qml Concurrent)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED IMPORTED_TARGET libcamera)
pkg_check_modules(GLES REQUIRED IMPORTED_TARGET egl glesv2)
pkg_check_modules(EXIV2 IMPORTED_TARGET exiv2)
# Video recording, when GStreamer is there; its encoders are found at run time.
pkg_check_modules(GSTREAMER IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0 gstreamer-tag-1.0)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_qml_module(omarchycamera
    URI OmarchyCamera
    VERSION 1.0
    PLUGIN_TARGET omarchycamera
    SOURCES
        src/framequeue.h
        src/camerasession.h src/camerasession.cpp
        src/viewfinder.h src/viewfinder.cpp
        src/stillwriter.h src/stillwriter.cpp
        src/rawmerge.h src/rawmerge.cpp
        src/videorecorder.h src/videorecorder.cpp
)

target_link_libraries(omarchycamera PRIVATE
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::Concurrent
    PkgConfig::LIBCAMERA PkgConfig::GLES)

# The generated QML type registrations include the headers by bare name.
target_include_directories(omarchycamera PRIVATE src)

# libcamera's Signal has a member named emit, which Qt's keyword macros break.
target_compile_definitions(omarchycamera PRIVATE QT_NO_KEYWORDS)

if(EXIV2_FOUND)
    target_link_libraries(omarchycamera PRIVATE PkgConfig::EXIV2)
    target_compile_definitions(omarchycamera PRIVATE HAVE_EXIV2)
endif()

if(GSTREAMER_FOUND)
    target_link_libraries(omarchycamera PRIVATE PkgConfig::GSTREAMER)
    target_compile_definitions(omarchycamera PRIVATE HAVE_GSTREAMER)
endif()
