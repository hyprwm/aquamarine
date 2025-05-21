cmake_minimum_required(VERSION 3.19)

file(READ "${CMAKE_SOURCE_DIR}/VERSION" VER_RAW)
string(STRIP ${VER_RAW} AQUAMARINE_VERSION)

add_compile_definitions(AQUAMARINE_VERSION="${AQUAMARINE_VERSION}")

project(
  aquamarine
  VERSION ${AQUAMARINE_VERSION}
  DESCRIPTION "A very light linux rendering backend library")

include(CTest)
include(CheckIncludeFile)
include(GNUInstallDirs)

set(PREFIX ${CMAKE_INSTALL_PREFIX})
set(INCLUDE ${CMAKE_INSTALL_FULL_INCLUDEDIR})
set(LIBDIR ${CMAKE_INSTALL_FULL_LIBDIR})

find_package(PkgConfig REQUIRED)
find_package(OpenGL REQUIRED COMPONENTS "GLES3")
find_package(hyprwayland-scanner 0.4.0 REQUIRED)
pkg_check_modules(
  deps
  REQUIRED
  IMPORTED_TARGET
  libseat>=0.8.0
  libinput>=1.26.0
  wayland-client
  wayland-protocols
  hyprutils>=0.5.2
  pixman-1
  libdrm
  gbm
  libudev
  libdisplay-info
  hwdata)

configure_file(aquamarine.pc.in aquamarine.pc @ONLY)

set(CMAKE_CXX_STANDARD 23)
add_compile_options(
  -Wall
  -Wextra
  -Wno-unused-parameter
  -Wno-unused-value
  -Wno-missing-field-initializers)
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

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
target_include_directories(
  aquamarine
  PUBLIC "./include"
  PRIVATE "./src" "./src/include" "./protocols" "${CMAKE_BINARY_DIR}")
set_target_properties(aquamarine PROPERTIES VERSION ${AQUAMARINE_VERSION}
                                            SOVERSION 7)
target_link_libraries(aquamarine OpenGL::EGL OpenGL::OpenGL PkgConfig::deps)

check_include_file("sys/timerfd.h" HAS_TIMERFD)
pkg_check_modules(epoll IMPORTED_TARGET epoll-shim)
if(NOT HAS_TIMERFD AND epoll_FOUND)
  target_link_libraries(aquamarine PkgConfig::epoll)
endif()

# Protocols
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
message(STATUS "Found wayland-protocols at ${WAYLAND_PROTOCOLS_DIR}")
pkg_get_variable(WAYLAND_SCANNER_PKGDATA_DIR wayland-scanner pkgdatadir)
message(STATUS "Found wayland-scanner pkgdatadir at ${WAYLAND_SCANNER_PKGDATA_DIR}")

function(protocolNew protoPath protoName external)
  if(external)
    set(path ${CMAKE_SOURCE_DIR}/${protoPath})
  else()
    set(path ${WAYLAND_PROTOCOLS_DIR}/${protoPath})
  endif()
  add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/protocols/${protoName}.cpp
           ${CMAKE_SOURCE_DIR}/protocols/${protoName}.hpp
    COMMAND hyprwayland-scanner --client ${path}/${protoName}.xml
            ${CMAKE_SOURCE_DIR}/protocols/
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  target_sources(aquamarine PRIVATE protocols/${protoName}.cpp
                                    protocols/${protoName}.hpp)
endfunction()
function(protocolWayland)
  add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/protocols/wayland.cpp
           ${CMAKE_SOURCE_DIR}/protocols/wayland.hpp
    COMMAND hyprwayland-scanner --wayland-enums --client
            ${WAYLAND_SCANNER_PKGDATA_DIR}/wayland.xml ${CMAKE_SOURCE_DIR}/protocols/
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  target_sources(aquamarine PRIVATE protocols/wayland.cpp protocols/wayland.hpp)
endfunction()

protocolwayland()

protocolnew("stable/xdg-shell" "xdg-shell" false)
protocolnew("stable/linux-dmabuf" "linux-dmabuf-v1" false)

# Generate hwdata info
pkg_get_variable(HWDATA_DIR hwdata pkgdatadir)
message(
  STATUS "Running ${CMAKE_SOURCE_DIR}/data/hwdata.sh < ${HWDATA_DIR}/pnp.ids")
execute_process(
  COMMAND /bin/sh -c
          "${CMAKE_SOURCE_DIR}/data/hwdata.sh < ${HWDATA_DIR}/pnp.ids"
  RESULT_VARIABLE HWDATA_PNP_RESULT
  OUTPUT_VARIABLE HWDATA_PNP_IDS ENCODING UTF8)

if(NOT HWDATA_PNP_RESULT MATCHES 0)
  message(WARNING "hwdata gathering pnps failed")
endif()

configure_file(data/hwdata.hpp.in hwdata.hpp @ONLY)

# tests
add_custom_target(tests)

add_executable(simpleWindow "tests/SimpleWindow.cpp")
target_link_libraries(simpleWindow PRIVATE PkgConfig::deps aquamarine)
add_test(
  NAME "simpleWindow"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests
  COMMAND simpleWindow "simpleWindow")
add_dependencies(tests simpleWindow)

add_executable(attachments "tests/Attachments.cpp")
target_link_libraries(attachments PRIVATE PkgConfig::deps aquamarine)
add_test(
  NAME "attachments"
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests
  COMMAND attachments "attachments")
add_dependencies(tests attachments)

# Installation
install(TARGETS aquamarine)
install(DIRECTORY "include/aquamarine" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_BINARY_DIR}/aquamarine.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
