cmake_minimum_required(VERSION 3.6)
project(flashsim)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

set(SOURCE_FILES
    src/746FlashSim.cpp
    src/746FlashSim.h
    src/libmockmalloc.cpp
    src/myFTL.cpp)

add_executable(flashsim ${SOURCE_FILES})