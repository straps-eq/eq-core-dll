cmake_minimum_required(VERSION 3.20)
project(eqcore CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "Build x64: the ToB client is a 64-bit process.")
endif()

add_library(dinput8 SHARED src/dllmain.cpp src/patcher.cpp src/patches.cpp src/nametags.cpp src/map.cpp src/dinput8.def)
target_compile_definitions(dinput8 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(dinput8 PRIVATE /W4)

add_executable(eqcore_check tools/eqcore_check.cpp src/patcher.cpp src/patches.cpp src/nametags.cpp src/map.cpp)
target_compile_definitions(eqcore_check PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(eqcore_check PRIVATE /W4)
