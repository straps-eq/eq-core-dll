CMAKE_MINIMUM_REQUIRED(VERSION 3.7)

SET(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/" ${CMAKE_MODULE_PATH})

IF(POLICY CMP0074)
	CMAKE_POLICY(SET CMP0074 NEW)
ENDIF()

PROJECT(DLL)

IF(NOT CMAKE_BUILD_TYPE)
	SET(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Choose the type of build." FORCE)
ENDIF(NOT CMAKE_BUILD_TYPE)

SET(CMAKE_CXX_STANDARD 14)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)
SET(CMAKE_CXX_EXTENSIONS OFF)

FILE(GLOB SRCS src/*.cpp)
FILE(GLOB HDRS include/*.h)
set(WIN32 True)
add_executable(main WIN32 ${SRCS} ${HDRS})

target_include_directories(main PRIVATE 	
	${PROJECT_SOURCE_DIR}/dependencies/dx9/include
	${PROJECT_SOURCE_DIR}/dependencies/detours/inc
	${PROJECT_SOURCE_DIR}/dependencies/boost
)

target_link_libraries(main
	${PROJECT_SOURCE_DIR}/dependencies/detours/lib
	${PROJECT_SOURCE_DIR}/dependencies/dx8/Lib
)

