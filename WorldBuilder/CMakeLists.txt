# WorldBuilder: compiles OSM, terrain and tree data into the world tiles the game streams (issue #94).
#
#   cmake -S WorldBuilder -B WorldBuilder/Build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build WorldBuilder/Build
#
# The library has no Unreal dependency, so the game can later compile the same sources into a plugin.
cmake_minimum_required(VERSION 3.24)
project(WorldBuilder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# Header-only libraries, fetched at a pinned version instead of copied into the repository.
include(FetchContent)
FetchContent_Declare(protozero
	GIT_REPOSITORY https://github.com/mapbox/protozero.git
	GIT_TAG v1.8.0
	GIT_SHALLOW TRUE
	SOURCE_SUBDIR _none)
FetchContent_Declare(libosmium
	GIT_REPOSITORY https://github.com/osmcode/libosmium.git
	GIT_TAG v2.22.0
	GIT_SHALLOW TRUE
	SOURCE_SUBDIR _none)
FetchContent_MakeAvailable(protozero libosmium)

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

add_library(WorldBuilderCore STATIC
	Source/Projection.cpp
	Source/OsmReader.cpp)
target_include_directories(WorldBuilderCore
	PUBLIC Source
	PRIVATE ${protozero_SOURCE_DIR}/include ${libosmium_SOURCE_DIR}/include)
target_link_libraries(WorldBuilderCore PRIVATE ZLIB::ZLIB Threads::Threads)
target_compile_options(WorldBuilderCore PRIVATE -Wall -Wextra)

add_executable(worldbuilder Programs/WorldBuilderMain.cpp)
target_link_libraries(worldbuilder PRIVATE WorldBuilderCore)
target_compile_options(worldbuilder PRIVATE -Wall -Wextra)
