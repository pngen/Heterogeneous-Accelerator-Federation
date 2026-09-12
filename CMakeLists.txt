cmake_minimum_required(VERSION 3.20)

project(HeterogeneousAcceleratorFederation
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral C++20 runtime for federating heterogeneous accelerator fleets"
  HOMEPAGE_URL "https://github.com/pngen/Heterogeneous-Accelerator-Federation"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(HAFCompilerOptions)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

option(HAF_BUILD_APPS "Build haf_coordinator, haf_agent, and haf_cli" ON)
option(HAF_BUILD_TESTS "Build the test suites" ON)
option(HAF_BUILD_EXAMPLES "Build the examples" ON)
option(HAF_BUILD_BENCHMARKS "Build the benchmarks" ON)
option(HAF_ENABLE_CUDA "Build the real CUDA adapter when a CUDA toolkit is present" ON)
set(HAF_CUDA_ARCHITECTURES "" CACHE STRING
  "CUDA architectures to target, for example \"120;90\". Empty detects the local GPU and falls back to the toolkit default.")

set(HAF_CUDA_AVAILABLE OFF)
if(HAF_ENABLE_CUDA)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    set(HAF_CUDA_AVAILABLE ON)
  endif()
endif()

if(HAF_CUDA_AVAILABLE)
  if(HAF_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES "${HAF_CUDA_ARCHITECTURES}")
    message(STATUS "CUDA architectures (configured): ${CMAKE_CUDA_ARCHITECTURES}")
  else()
    # CMake's "native" mode compiles and runs a probe executable. On a host with
    # more than one CUDA toolkit installed, an unrelated runtime DLL earlier on
    # PATH can shadow the toolkit the probe was linked against, and the probe
    # then reports that no GPU exists. The compute capability is queried
    # directly instead, which does not depend on the probe's runtime lookup.
    find_program(HAF_NVIDIA_SMI nvidia-smi)
    if(HAF_NVIDIA_SMI)
      execute_process(
        COMMAND "${HAF_NVIDIA_SMI}" --query-gpu=compute_cap --format=csv,noheader
        OUTPUT_VARIABLE HAF_GPU_COMPUTE_CAPS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE HAF_GPU_QUERY_RESULT)
      if(HAF_GPU_QUERY_RESULT EQUAL 0 AND NOT HAF_GPU_COMPUTE_CAPS STREQUAL "")
        string(REPLACE "\n" ";" HAF_GPU_CAP_LINES "${HAF_GPU_COMPUTE_CAPS}")
        set(HAF_DETECTED_ARCHITECTURES "")
        foreach(haf_cap_line IN LISTS HAF_GPU_CAP_LINES)
          string(STRIP "${haf_cap_line}" haf_cap_line)
          string(REPLACE "." "" haf_cap_line "${haf_cap_line}")
          string(REPLACE "," "" haf_cap_line "${haf_cap_line}")
          if(haf_cap_line MATCHES "^[0-9]+$" AND NOT haf_cap_line IN_LIST HAF_DETECTED_ARCHITECTURES)
            list(APPEND HAF_DETECTED_ARCHITECTURES "${haf_cap_line}")
          endif()
        endforeach()
        if(HAF_DETECTED_ARCHITECTURES)
          list(SORT HAF_DETECTED_ARCHITECTURES COMPARE NATURAL)
          set(CMAKE_CUDA_ARCHITECTURES "${HAF_DETECTED_ARCHITECTURES}")
          message(STATUS "CUDA architectures (detected from the local GPU): ${CMAKE_CUDA_ARCHITECTURES}")
        endif()
      endif()
    endif()
    if(NOT CMAKE_CUDA_ARCHITECTURES)
      message(STATUS "No CUDA GPU was detected; the toolkit default architecture list will be used")
    endif()
  endif()
endif()

if(MSVC)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/bigobj>)
endif()

add_subdirectory(src)
if(HAF_BUILD_APPS)
  add_subdirectory(apps)
endif()
if(HAF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(HAF_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(HAF_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Installation and package export
# ---------------------------------------------------------------------------
install(TARGETS haf_core haf_net haf_adapters
  EXPORT HeterogeneousAcceleratorFederationTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

if(TARGET haf_adapter_cuda)
  install(TARGETS haf_adapter_cuda
    EXPORT HeterogeneousAcceleratorFederationTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

if(HAF_BUILD_APPS)
  install(TARGETS haf_coordinator haf_agent haf_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT HeterogeneousAcceleratorFederationTargets
  FILE HeterogeneousAcceleratorFederationTargets.cmake
  NAMESPACE HeterogeneousAcceleratorFederation::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HeterogeneousAcceleratorFederation)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/HeterogeneousAcceleratorFederationConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/HeterogeneousAcceleratorFederationConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HeterogeneousAcceleratorFederation)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/HeterogeneousAcceleratorFederationConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/HeterogeneousAcceleratorFederationConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/HeterogeneousAcceleratorFederationConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HeterogeneousAcceleratorFederation)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/HeterogeneousAcceleratorFederation)

message(STATUS "Heterogeneous Accelerator Federation ${PROJECT_VERSION}")
message(STATUS "  build type .................. ${CMAKE_BUILD_TYPE}")
message(STATUS "  CUDA adapter ................ ${HAF_CUDA_AVAILABLE}")
message(STATUS "  tests / examples / benchmarks ${HAF_BUILD_TESTS} / ${HAF_BUILD_EXAMPLES} / ${HAF_BUILD_BENCHMARKS}")
message(STATUS "  warnings as errors .......... ${HAF_WARNINGS_AS_ERRORS}")
message(STATUS "  AddressSanitizer ............ ${HAF_ENABLE_ASAN}")
