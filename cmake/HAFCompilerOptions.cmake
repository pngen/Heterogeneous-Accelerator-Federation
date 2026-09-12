# Compiler/linker hardening and diagnostic options for Heterogeneous Accelerator Federation.
# These are applied to exported targets as INTERFACE requirements where they must
# propagate (public headers are compiled by downstream consumers too).

include_guard(GLOBAL)

option(HAF_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(HAF_ENABLE_ASAN "Build with AddressSanitizer instrumentation" OFF)
option(HAF_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer instrumentation" OFF)

function(haf_apply_warnings target)
  if(MSVC)
    # Restricted to the CXX language so that the same helper can be reused on a
    # target that also contains CUDA translation units without leaking raw MSVC
    # switches into nvcc.
    target_compile_options(${target} PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8;/Zc:__cplusplus;/Zc:preprocessor;/EHsc;/MP>)
    if(HAF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/WX>)
    endif()
    # Deterministic builds: suppress the embedded absolute source path in __FILE__.
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/d1trimfile:${CMAKE_SOURCE_DIR}/>)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
      -Wold-style-cast -Wcast-qual -Wnon-virtual-dtor -Woverloaded-virtual
      -Wdouble-promotion -Wformat=2 -Wundef -Wnull-dereference)
    if(HAF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(HAF_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
      target_link_options(${target} PRIVATE /INCREMENTAL:NO)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
  if(HAF_ENABLE_UBSAN AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer -g)
    target_link_options(${target} PRIVATE -fsanitize=undefined)
  endif()
endfunction()
