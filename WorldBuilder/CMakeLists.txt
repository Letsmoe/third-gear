# WorldBuilder: compiles OSM, terrain and tree data into the world tiles the game streams (issue #94).
#
#   cmake -S WorldBuilder -B WorldBuilder/Build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build WorldBuilder/Build
#
# The libraries have no Unreal dependency, so the game can later compile the same sources into a plugin.
#
# Layout: Source/<Part>/ holds one library each (Core: OSM, projection, terrain, rasters, geometry and the tile format;
# Streets: the street model; Buildings: building typing and roofs; Traffic: signals, signs, lamps, zebras and parked
# cars; World: the build of a region's tiles from all of them). Programs/<name>.cpp each become an executable:
# worldbuilder links everything, Streets*.cpp, Buildings*.cpp and Traffic*.cpp only what their part needs, so the
# parts build independently.
cmake_minimum_required(VERSION 3.24)
project(WorldBuilder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# Header-only and small libraries, fetched at a pinned version instead of copied into the repository.
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
FetchContent_Declare(clipper2
	GIT_REPOSITORY https://github.com/AngusJohnson/Clipper2.git
	GIT_TAG Clipper2_1.5.4
	GIT_SHALLOW TRUE
	SOURCE_SUBDIR _none)
FetchContent_MakeAvailable(protozero libosmium clipper2)

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

# Clipper2 (Boost licence): polygon booleans and offsets on integer coordinates.
file(GLOB Clipper2Sources ${clipper2_SOURCE_DIR}/CPP/Clipper2Lib/src/*.cpp)
add_library(Clipper2 STATIC ${Clipper2Sources})
target_include_directories(Clipper2 SYSTEM PUBLIC ${clipper2_SOURCE_DIR}/CPP/Clipper2Lib/include)

set(WorldBuilderWarnings -Wall -Wextra)
# The command-line build runs where it is compiled, so it may use every instruction the machine has (the filters and
# rasters vectorise); a build for other machines or for the game's plugin turns this off.
option(WORLD_BUILDER_NATIVE "Optimise for the building machine's CPU" ON)
if(WORLD_BUILDER_NATIVE)
	add_compile_options(-march=native)
endif()

# One part's library from Source/<Folder>, linked to the libraries after the folder.
function(world_builder_library Name Folder)
	file(GLOB_RECURSE Sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Source/${Folder}/*.cpp)
	# A part with no sources yet is an empty interface library, so everything that links it still configures.
	if(Sources)
		add_library(${Name} STATIC ${Sources})
		target_include_directories(${Name} PUBLIC Source/${Folder})
		target_compile_options(${Name} PRIVATE ${WorldBuilderWarnings})
		target_link_libraries(${Name} PUBLIC ${ARGN})
	else()
		add_library(${Name} INTERFACE)
		target_link_libraries(${Name} INTERFACE ${ARGN})
	endif()
endfunction()

world_builder_library(WorldBuilderCore Core Clipper2 ZLIB::ZLIB Threads::Threads)
target_include_directories(WorldBuilderCore SYSTEM PRIVATE ${protozero_SOURCE_DIR}/include ${libosmium_SOURCE_DIR}/include)
world_builder_library(WorldBuilderStreets Streets WorldBuilderCore)
world_builder_library(WorldBuilderBuildings Buildings WorldBuilderCore)
world_builder_library(WorldBuilderTraffic Traffic WorldBuilderCore WorldBuilderStreets)
world_builder_library(WorldBuilderWorld World WorldBuilderCore WorldBuilderStreets WorldBuilderBuildings WorldBuilderTraffic)
target_compile_definitions(WorldBuilderWorld PRIVATE WORLD_BUILDER_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")

file(GLOB Programs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Programs/*.cpp)
foreach(Program ${Programs})
	get_filename_component(ProgramName ${Program} NAME_WE)
	add_executable(${ProgramName} ${Program})
	target_compile_options(${ProgramName} PRIVATE ${WorldBuilderWarnings})
	if(ProgramName MATCHES "^Streets")
		target_link_libraries(${ProgramName} PRIVATE WorldBuilderStreets)
	elseif(ProgramName MATCHES "^Buildings")
		target_link_libraries(${ProgramName} PRIVATE WorldBuilderBuildings)
	elseif(ProgramName MATCHES "^Traffic")
		target_link_libraries(${ProgramName} PRIVATE WorldBuilderTraffic)
	else()
		target_link_libraries(${ProgramName} PRIVATE WorldBuilderWorld)
	endif()
endforeach()
