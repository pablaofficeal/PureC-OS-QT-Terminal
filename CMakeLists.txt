cmake_minimum_required(VERSION 3.21)
project(qt-terminal LANGUAGES C CXX)
set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_executable(qt-terminal
    src/main.cpp
    src/vtscreen.cpp src/vtscreen.h
    src/ptysession.cpp src/ptysession.h
    src/terminalwidget.cpp src/terminalwidget.h)
target_link_libraries(qt-terminal PRIVATE Qt6::Widgets Qt6::QPurecIntegrationPlugin)
