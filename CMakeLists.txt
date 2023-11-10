cmake_minimum_required(VERSION 3.3 FATAL_ERROR)
project(rdmapp)
set(CMAKE_CXX_STANDARD 20)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(Threads REQUIRED)
find_package(ibverbs REQUIRED)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif ()

option(RDMAPP_BUILD_DOCS "Build docs" OFF)
option(RDMAPP_ASAN "Build with AddressSanitizer" OFF)

if (RDMAPP_BUILD_DOCS)
  # check if Doxygen is installed
  find_package(Doxygen)
  if (DOXYGEN_FOUND)
      # set input and output files
      set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)

      # request to configure the file
      message("Doxygen build started")

      # note the option ALL which allows to build the docs together with the application
      add_custom_target( doc_doxygen ALL
          COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
          WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
          COMMENT "Generating API documentation with Doxygen"
          VERBATIM )
  else (DOXYGEN_FOUND)
    message("Doxygen need to be installed to generate the doxygen documentation")
  endif (DOXYGEN_FOUND)
endif ()

set(RDMAPP_SOURCE_FILES
  src/device.cc
  src/pd.cc
  src/cq.cc
  src/qp.cc
  src/srq.cc
  src/mr.cc
)

set(RDMAPP_LINK_LIBRARIES ibverbs Threads::Threads)

add_library(rdmapp STATIC ${RDMAPP_SOURCE_FILES})

list(APPEND
  RDMAPP_COMPILE_OPTIONS
  PUBLIC
  -flto
  PRIVATE
  -fno-rtti
  -Wall 
  -Wextra 
  -pedantic 
  -Werror
)
list(APPEND
  RDMAPP_LINK_OPTIONS
  PUBLIC
  -flto
  PRIVATE
  -fno-rtti
)
if (RDMAPP_ASAN)
  list(APPEND RDMAPP_COMPILE_OPTIONS PRIVATE -fno-omit-frame-pointer -fsanitize=address)
  list(APPEND RDMAPP_LINK_OPTIONS -fno-omit-frame-pointer -fsanitize=address)
endif ()

if (CMAKE_CXX_COMPILER_ID MATCHES Clang)
  list(APPEND RDMAPP_COMPILE_OPTIONS
    PUBLIC
    -stdlib=libc++
    -fcoroutines-ts
  )
elseif (CMAKE_CXX_COMPILER_ID MATCHES GNU)
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS "11") 
    list(APPEND RDMAPP_COMPILE_OPTIONS
      PUBLIC
      -fcoroutines
    )
  endif ()
endif ()

target_compile_options(rdmapp ${RDMAPP_COMPILE_OPTIONS})
target_link_options(rdmapp ${RDMAPP_LINK_OPTIONS})
target_link_libraries(rdmapp ${RDMAPP_LINK_LIBRARIES})
target_include_directories(rdmapp PUBLIC include)

find_program(iwyu_path NAMES include-what-you-use iwyu)
if (iwyu_path)
  message("-- Using include-what-you-use ${iwyu_path}")
  set_property(TARGET rdmapp PROPERTY CXX_INCLUDE_WHAT_YOU_USE ${iwyu_path})
endif ()

include(GNUInstallDirs)
install(TARGETS rdmapp EXPORT rdmapp ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/rdmapp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
