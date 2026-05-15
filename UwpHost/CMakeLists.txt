cmake_minimum_required(VERSION 3.20)
project(UwpHost LANGUAGES CXX)

set(CMAKE_SYSTEM_NAME "WindowsStore")
set(CMAKE_SYSTEM_VERSION "10.0")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(UwpHost WIN32
    App.cpp
    pch.h
    pch.cpp
    Package.appxmanifest
)

set_property(TARGET UwpHost PROPERTY VS_WINRT_COMPONENT FALSE)
set_property(TARGET UwpHost PROPERTY VS_PACKAGE_REFERENCES "Microsoft.Windows.CppWinRT_2.0.240405.15")

target_compile_options(UwpHost PRIVATE /bigobj /await)
target_precompile_headers(UwpHost PRIVATE pch.h)
