@echo off
setlocal

set BUILD_DIR=%1
set CPP_DIR=%2
set SDK_VER=10.0.26100.0
set SDK_INCLUDE=C:\Program Files (x86)\Windows Kits\10\Include\%SDK_VER%
set SDK_LIB=C:\Program Files (x86)\Windows Kits\10\Lib\%SDK_VER%
set VC_INCLUDE=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.50.35717

cl.exe /EHsc /std:c++20 /W4 /O2 ^
  /I"%BUILD_DIR%" ^
  /I"%VC_INCLUDE%\include" ^
  /I"%SDK_INCLUDE%\um" ^
  /I"%SDK_INCLUDE%\shared" ^
  /I"%SDK_INCLUDE%\winrt" ^
  /I"%SDK_INCLUDE%\ucrt" ^
  "%CPP_DIR%\main.cpp" ^
  golib.lib ^
  /link /MACHINE:X64 ^
  /LIBPATH:"%SDK_LIB%\um\x64" ^
  /LIBPATH:"%SDK_LIB%\ucrt\x64" ^
  WindowsApp.lib ole32.lib oleaut32.lib ^
  /OUT:test_winrt.exe

endlocal
