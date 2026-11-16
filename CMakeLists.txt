cmake_minimum_required(VERSION 3.21)
project(framepipe LANGUAGES CXX)

message(STATUS "Compiler: ${CMAKE_CXX_COMPILER}")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
cmake_policy(SET CMP0091 NEW)


option(FRAMEPIPE_GENERATE_STUBS "Generate Python stubs" ON)
# sanitizers
set(SANITIZER "none" CACHE STRING "Sanitizer: none, address, thread")
set_property(CACHE SANITIZER PROPERTY STRINGS none address thread)
if(NOT SANITIZER MATCHES "^(none|address|thread)$")
  message(FATAL_ERROR "SANITIZER must be none|address|thread, got '${SANITIZER}'")
endif()

if(NOT SANITIZER STREQUAL "none")
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Sanitizer presets are supported on Linux only")
  endif()
endif()

# MSVC
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# dependencies
enable_language(CUDA)
find_package(CUDAToolkit REQUIRED)
find_package(Python 3.9 COMPONENTS Interpreter Development.Module REQUIRED)

execute_process(
  COMMAND "${Python_EXECUTABLE}" -c "import nanobind; print(nanobind.cmake_dir())"
  OUTPUT_VARIABLE NANOBIND_DIR OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE NB_RESULT)
if(NOT NB_RESULT EQUAL 0)
  message(FATAL_ERROR "nanobind not found via ${Python_EXECUTABLE}")
endif()

file(TO_CMAKE_PATH "${NANOBIND_DIR}" NANOBIND_DIR)

find_package(nanobind CONFIG REQUIRED PATHS "${NANOBIND_DIR}" NO_DEFAULT_PATH)

find_package(FFMPEG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(OpenCL QUIET)
endif()
if(OpenCL_FOUND)
  file(READ
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/nv12_convert.cl"
    FRAMEPIPE_OPENCL_SOURCE
  )

  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/opencl_kernel_source.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/opencl_kernel_source.hpp"
    @ONLY
  )
endif()

# main .so (CPU + VAAPI)
nanobind_add_module(_framepipe NB_STATIC
  src/reader.cpp
  src/bindings.cpp
  src/post_processors/impls/cpu.cpp
  src/post_processors/impls/vaapi.cpp
  src/cuda_loader.cpp
)
if(OpenCL_FOUND)
  target_sources(_framepipe PRIVATE
    src/post_processors/impls/opencl.cpp
  )
  target_compile_definitions(_framepipe PRIVATE
    FRAMEPIPE_HAS_OPENCL=1
  )
  target_include_directories(_framepipe PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
  )
  target_link_libraries(_framepipe PRIVATE
    OpenCL::OpenCL
  )
  message(STATUS "framepipe: OpenCL enabled")
else()
  message(STATUS "framepipe: OpenCL unavailable")
endif()

if(SANITIZER STREQUAL "address")
  target_compile_options(_framepipe PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fsanitize=address,undefined;-fno-omit-frame-pointer;-g;-O1>
  )
  target_link_options(_framepipe PRIVATE -fsanitize=address,undefined -shared-libsan)
elseif(SANITIZER STREQUAL "thread")
  target_compile_options(_framepipe PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fsanitize=thread;-fno-omit-frame-pointer;-g;-O1>
  )
  target_link_options(_framepipe PRIVATE -fsanitize=thread -shared-libsan)
endif()

target_include_directories(_framepipe SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(_framepipe PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(_framepipe PRIVATE
  ${FFMPEG_LIBRARIES}
  ${CMAKE_DL_LIBS}
  spdlog::spdlog
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND SANITIZER STREQUAL "none")
  target_link_options(_framepipe PRIVATE "LINKER:-Bsymbolic")
endif()

# cuda .so extension
add_library(_framepipe_cuda SHARED
  src/post_processors/impls/cuda.cpp
  src/gpu/cuda.cu
)
target_include_directories(_framepipe_cuda SYSTEM PRIVATE
  ${FFMPEG_INCLUDE_DIRS}
)
target_link_directories(_framepipe_cuda PRIVATE
  ${FFMPEG_LIBRARY_DIRS}
)
target_link_libraries(_framepipe_cuda PRIVATE
  ${FFMPEG_LIBRARIES}
  CUDA::cudart
  CUDA::cuda_driver
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND SANITIZER STREQUAL "none")
  target_link_options(_framepipe_cuda PRIVATE "LINKER:-Bsymbolic")
endif()

set_target_properties(_framepipe_cuda PROPERTIES
  PREFIX ""
  OUTPUT_NAME "_framepipe_cuda"
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CUDA_STANDARD 20
  CUDA_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/framepipe"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/framepipe"
)
if(WIN32)
  set_target_properties(_framepipe_cuda PROPERTIES
    SUFFIX ".pyd"
  )
endif()

# stubs
if(SANITIZER STREQUAL "none" AND FRAMEPIPE_GENERATE_STUBS)
  nanobind_add_stub(framepipe_stub
    MODULE _framepipe
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/framepipe/_framepipe.pyi
    PYTHON_PATH $<TARGET_FILE_DIR:_framepipe>
    PATTERN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/stub_patterns.txt
    DEPENDS _framepipe
  )
  add_custom_target(stubs ALL DEPENDS framepipe_stub)
endif()

set_target_properties(_framepipe PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/framepipe
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CUDA_STANDARD 20
  CUDA_STANDARD_REQUIRED ON
)
add_dependencies(_framepipe _framepipe_cuda)

install(TARGETS _framepipe LIBRARY DESTINATION framepipe RUNTIME DESTINATION framepipe)
install(TARGETS _framepipe_cuda LIBRARY DESTINATION framepipe RUNTIME DESTINATION framepipe)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/framepipe/py.typed DESTINATION framepipe)
