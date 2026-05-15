cmake_minimum_required(VERSION 3.20)

set(CMAKE_SYSTEM_NAME "WindowsStore")
set(CMAKE_SYSTEM_VERSION "10.0")

project(UwpHost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(UwpHost WIN32
    App.cpp
    pch.h
    pch.cpp
)

target_compile_options(UwpHost PRIVATE /bigobj /await)
target_compile_definitions(UwpHost PRIVATE _CRT_SECURE_NO_WARNINGS)
target_precompile_headers(UwpHost PRIVATE pch.h)
