cmake_minimum_required(VERSION 3.11)
project(EL)

set(CMAKE_CXX_STANDARD 11)
IF (CMAKE_SYSTEM_NAME MATCHES "Windows")
    link_libraries("D:/MinGW64/x86_64-w64-mingw32/lib/libws2_32.a")
ENDIF ()
add_executable(EL
        src/main.cpp
        deps/EL/EventLoop.hpp
        deps/EL/TimeEvent.hpp
        src/Service/Service.cpp
        src/Service/Service.h
        utils/utils.cpp
        utils/utils.h
        deps/http/HttpServer.cpp
        deps/http/Request.cpp
        deps/http/Response.cpp
        )