# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(external_lib)

target_sources(app PRIVATE main.cpp)

# The external static library that we are linking with does not know
# how to build for this platform so we export all the flags used in
# this zephyr build to the external build system.
#
# Other external build systems may be self-contained enough that they
# do not need any build information from zephyr. Or they may be
# incompatible with certain zephyr options and need them to be
# filtered out.
zephyr_get_include_directories_for_lang_as_string(       C includes)
zephyr_get_system_include_directories_for_lang_as_string(C system_includes)
zephyr_get_compile_definitions_for_lang_as_string(       C definitions)
zephyr_get_compile_options_for_lang_as_string(           C options)

set(external_project_cflags
  "${includes} ${definitions} ${options} ${system_includes}"
  )

  include(ExternalProject)

# ardupilot needs these... see also the Makefiles in AP_HAL and AP_Common
  add_definitions(-D__AP_LINE__=17)
  add_definitions(-DCONFIG_HAL_BOARD=HAL_BOARD_ESP32)
  add_definitions(-DCONFIG_HAL_BOARD_SUBTYPE=HAL_BOARD_SUBTYPE_ESP32_S3EMPTY) 
  add_definitions(-I.)
  add_definitions(-I..)
  add_definitions(-I../..)
  add_definitions(-I../../..)
  # add_definitions(-I../../../..)
  # add_definitions(-I../../../../..)

  # add_definitions(-I./board)
  # add_definitions(-I../board)
  # add_definitions(-I../../board)
  # add_definitions(-I../../../board)
  # add_definitions(-I../../../../board)


  # add_definitions(-I./AP_HAL/board)
  # add_definitions(-I../AP_HAL/board)
  # add_definitions(-I../../AP_HAL/board)
  # add_definitions(-I../../../AP_HAL/board)
  # add_definitions(-I../../../../AP_HAL/board)
#-fsingle-precision-constant


target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/AP_Common
  ${CMAKE_CURRENT_SOURCE_DIR}/AP_HAL
  ${CMAKE_CURRENT_SOURCE_DIR}/AP_HAL/board
  )


# Add an external project to be able download and build the third
# party library. In this case downloading is not necessary as it has
# been committed to the repository.
set(AP_Common_src_dir   ${CMAKE_CURRENT_SOURCE_DIR}/AP_Common)
set(AP_Common_build_dir ${CMAKE_CURRENT_BINARY_DIR}/AP_Common)

set(AP_HAL_src_dir   ${CMAKE_CURRENT_SOURCE_DIR}/AP_HAL)
set(AP_HAL_build_dir ${CMAKE_CURRENT_BINARY_DIR}/AP_HAL)

set(MYLIB_LIB_DIR1     ${AP_Common_build_dir}/lib)
set(MYLIB_INCLUDE_DIR ${AP_Common_src_dir})

set(MYLIB_LIB_DIR2     ${AP_HAL_build_dir}/lib)
set(MYLIB_INCLUDE_DIR ${AP_HAL_src_dir})

if(CMAKE_GENERATOR STREQUAL "Unix Makefiles")
# https://www.gnu.org/software/make/manual/html_node/MAKE-Variable.html
set(submake "$(MAKE)")
else() # Obviously no MAKEFLAGS. Let's hope a "make" can be found somewhere.
set(submake "make")
endif()

ExternalProject_Add(
  AP_Common_project                 # Name for custom target
  PREFIX     ${AP_Common_build_dir} # Root dir for entire project
  SOURCE_DIR ${AP_Common_src_dir}
  BINARY_DIR ${AP_Common_src_dir} # This particular build system is invoked from the root
  CONFIGURE_COMMAND ""    # Skip configuring the project, e.g. with autoconf
  BUILD_COMMAND
  ${submake}
  PREFIX=${AP_Common_build_dir}
  CC=${CMAKE_C_COMPILER}
  AR=${CMAKE_AR}
  CFLAGS=${external_project_cflags}
  INSTALL_COMMAND ""      # This particular build system has no install command
  BUILD_BYPRODUCTS ${MYLIB_LIB_DIR1}/libAP_Common.a
  )

ExternalProject_Add(
  AP_HAL_project                 # Name for custom target
  PREFIX     ${AP_HAL_build_dir} # Root dir for entire project
  SOURCE_DIR ${AP_HAL_src_dir}
  BINARY_DIR ${AP_HAL_src_dir} # This particular build system is invoked from the root
  CONFIGURE_COMMAND ""    # Skip configuring the project, e.g. with autoconf
  BUILD_COMMAND
  ${submake}
  PREFIX=${AP_HAL_build_dir}
  CC=${CMAKE_C_COMPILER}
  AR=${CMAKE_AR}
  CFLAGS=${external_project_cflags}
  INSTALL_COMMAND ""      # This particular build system has no install command
  BUILD_BYPRODUCTS ${MYLIB_LIB_DIR2}/libAP_HAL.a
  )

# Create a wrapper CMake library that our app can link with
add_library(AP_Common_lib STATIC IMPORTED GLOBAL)
add_library(AP_HAL_lib STATIC IMPORTED GLOBAL)
add_dependencies(
  AP_Common_lib
  AP_Common_project
  )
add_dependencies(
    AP_HAL_lib
    AP_HAL_project
    )
set_target_properties(AP_Common_lib PROPERTIES IMPORTED_LOCATION             ${MYLIB_LIB_DIR1}/libAP_Common.a)
set_target_properties(AP_Common_lib PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${MYLIB_INCLUDE_DIR})

set_target_properties(AP_HAL_lib PROPERTIES IMPORTED_LOCATION             ${MYLIB_LIB_DIR2}/libAP_HAL.a)
set_target_properties(AP_HAL_lib PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${MYLIB_INCLUDE_DIR})

target_link_libraries(app PUBLIC AP_Common_lib)
target_link_libraries(app PUBLIC AP_HAL_lib)
