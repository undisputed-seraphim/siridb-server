cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)
project(siridb-server VERSION 2.0.27)

if (NOT DEFINED ${CMAKE_BUILD_TYPE})
	set (CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

configure_file(
	"${CMAKE_SOURCE_DIR}/version.h.in"
	"${CMAKE_SOURCE_DIR}/include/siri/version.h"
)

find_package(EXPAT REQUIRED)
find_package(PCRE2 REQUIRED)
find_package(LibUV REQUIRED)
find_package(UUID REQUIRED)
find_package(cleri REQUIRED)

configure_file(
	"${CMAKE_SOURCE_DIR}/cmake/version.h.in"
	"${CMAKE_SOURCE_DIR}/include/siri/version.h"
)

set(siridb-server_src_dir "${CMAKE_SOURCE_DIR}/src")
file(GLOB_RECURSE siridb-server_src ${siridb-server_src_dir}/*.c)

add_executable(siridb-server ${siridb-server_src} ${CMAKE_SOURCE_DIR}/main.c)
target_include_directories(siridb-server PRIVATE ${EXPAT_INCLUDE_DIRS} ${LibUV_INCLUDE_DIRS} ${PCRE2_INCLUDE_DIRS} ${UUID_INCLUDE_DIR} ${cleri_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(siridb-server PRIVATE ${EXPAT_LIBRARIES} ${LibUV_LIBRARIES} ${PCRE2_LIBRARIES} ${UUID_LIBRARIES} cleri m crypt)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

install(
	TARGETS siridb-server
	EXPORT siridb-server-targets
	RUNTIME DESTINATION bin
)

#export(EXPORT siridb-targets FILE ${CMAKE_BINARY_DIR}/siridb-targets.cmake)
