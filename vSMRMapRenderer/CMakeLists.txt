cmake_minimum_required(VERSION 3.20)

project(vSMRMapRenderer LANGUAGES CXX)

option(VSMR_BUILD_MAPLIBRE_POC "Build the standalone MapLibre airport renderer proof of concept." OFF)

if(NOT VSMR_BUILD_MAPLIBRE_POC)
	message(STATUS "vSMR MapLibre POC is disabled. Reconfigure with -DVSMR_BUILD_MAPLIBRE_POC=ON when MapLibre Native x64 artifacts are available.")
	return()
endif()

set(MAPLIBRE_NATIVE_ROOT "" CACHE PATH "Path to a MapLibre Native checkout or install prefix.")
set(MAPLIBRE_NATIVE_INCLUDE_DIR "" CACHE PATH "Optional explicit MapLibre Native include directory.")
set(MAPLIBRE_NATIVE_LIBRARIES "" CACHE STRING "Semicolon-separated MapLibre Native and dependency libraries.")

if(NOT MAPLIBRE_NATIVE_ROOT AND NOT MAPLIBRE_NATIVE_INCLUDE_DIR)
	message(FATAL_ERROR "Set MAPLIBRE_NATIVE_ROOT or MAPLIBRE_NATIVE_INCLUDE_DIR to the x64 MapLibre Native headers.")
endif()

if(NOT MAPLIBRE_NATIVE_LIBRARIES)
	message(FATAL_ERROR "Set MAPLIBRE_NATIVE_LIBRARIES to mbgl-core and all required x64 dependency libraries.")
endif()

add_executable(vSMRMapRendererPoc
	src/MapLibrePocMain.cpp
)

target_compile_features(vSMRMapRendererPoc PRIVATE cxx_std_17)

target_include_directories(vSMRMapRendererPoc PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/..
	${MAPLIBRE_NATIVE_INCLUDE_DIR}
	${MAPLIBRE_NATIVE_ROOT}/include
)

target_link_libraries(vSMRMapRendererPoc PRIVATE
	${MAPLIBRE_NATIVE_LIBRARIES}
)

if(WIN32)
	target_compile_definitions(vSMRMapRendererPoc PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
	target_link_libraries(vSMRMapRendererPoc PRIVATE Psapi)
endif()

if(MSVC)
	target_compile_options(vSMRMapRendererPoc PRIVATE /W3 /permissive-)
endif()
