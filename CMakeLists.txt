cmake_minimum_required(VERSION 3.19)

set(AQUAMARINE_VERSION "0.1.0")
add_compile_definitions(AQUAMARINE_VERSION="${AQUAMARINE_VERSION}")

project(aquamarine
    VERSION ${AQUAMARINE_VERSION}
    DESCRIPTION "A very light linux rendering backend library"
)

include(CTest)
include(GNUInstallDirs)

set(PREFIX ${CMAKE_INSTALL_PREFIX})
set(INCLUDE ${CMAKE_INSTALL_FULL_INCLUDEDIR})
set(LIBDIR ${CMAKE_INSTALL_FULL_LIBDIR})

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET wayland-client wayland-protocols hyprutils>=0.1.2)

configure_file(aquamarine.pc.in aquamarine.pc @ONLY)

set(CMAKE_CXX_STANDARD 23)

if(CMAKE_BUILD_TYPE MATCHES Debug OR CMAKE_BUILD_TYPE MATCHES DEBUG)
    message(STATUS "Configuring aquamarine in Debug")
    add_compile_definitions(AQUAMARINE_DEBUG)
else()
    add_compile_options(-O3)
    message(STATUS "Configuring aquamarine in Release")
endif()

file(GLOB_RECURSE SRCFILES CONFIGURE_DEPENDS "src/*.cpp" "include/*.hpp")
file(GLOB_RECURSE PUBLIC_HEADERS CONFIGURE_DEPENDS "include/*.hpp")

add_library(aquamarine SHARED ${SRCFILES})
target_include_directories( aquamarine
    PUBLIC "./include"
    PRIVATE "./src"
)
set_target_properties(aquamarine PROPERTIES
    VERSION ${AQUAMARINE_VERSION}
    SOVERSION 0
)
target_link_libraries(aquamarine PkgConfig::deps)

# Installation
install(TARGETS aquamarine)
install(DIRECTORY "include/aquamarine" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_BINARY_DIR}/aquamarine.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
