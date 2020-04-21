cmake_minimum_required(VERSION 3.11)
project(EL)

set(CMAKE_CXX_STANDARD 11)
IF (CMAKE_SYSTEM_NAME MATCHES "Windows")
    link_libraries("D:/MinGW64/x86_64-w64-mingw32/lib/libws2_32.a")
ENDIF ()
add_executable(EL
        main.cpp
        EventLoop.hpp
        TimeEvent.hpp
        )