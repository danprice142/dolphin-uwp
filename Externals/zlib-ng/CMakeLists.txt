set(ZLIB_ENABLE_TESTS OFF)
set(ZLIB_COMPAT ON)
set(ZLIB_ALIASES OFF CACHE BOOL "Disable zlib-ng aliases to avoid duplicate targets" FORCE)
set(SKIP_INSTALL_ALL ON)

option(BUILD_SHARED_LIBS "Build shared library" OFF)

add_subdirectory(zlib-ng)

# Set ZLIB variables for find_package used by other projects
set(ZLIB_INCLUDE_DIR ${CMAKE_BINARY_DIR}/zlib-ng CACHE STRING "Path to zlib include directory")
set(ZLIB_LIBRARY ZLIB::ZLIB CACHE STRING "Path to zlib library")

# Setup zlib alias project so FindZLIB doesn't recreate it
if(NOT TARGET ZLIB::ZLIB)
  if(TARGET zlib)
    get_target_property(_zlib_aliased zlib ALIASED_TARGET)
    if(_zlib_aliased)
      add_library(ZLIB::ZLIB ALIAS ${_zlib_aliased})
    else()
      add_library(ZLIB::ZLIB ALIAS zlib)
    endif()
  elseif(TARGET zlibstatic)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
  endif()
endif()

if(TARGET zlib)
  get_target_property(_zlib_aliased zlib ALIASED_TARGET)
  if(_zlib_aliased)
    dolphin_disable_warnings(${_zlib_aliased})
  else()
    dolphin_disable_warnings(zlib)
  endif()
elseif(TARGET zlibstatic)
  dolphin_disable_warnings(zlibstatic)
endif()
