# --- haf_core: vendor-neutral federation runtime --------------------------
add_library(haf_core
  core/ids.cpp
  core/status.cpp
  core/version.cpp
  core/hash.cpp
  core/bytes.cpp
  core/idgen.cpp
  core/time.cpp
  model/taxonomy.cpp
  model/capability_key.cpp
  model/capability.cpp
  model/evidence.cpp
  model/accelerator.cpp
  model/portability.cpp
  model/workload.cpp
  model/policy.cpp
  model/compatibility.cpp
  model/migration.cpp
  model/membership.cpp
  engine/portability_engine.cpp
  engine/compatibility_engine.cpp
  engine/ranking.cpp
  engine/migration_planner.cpp
  engine/audit.cpp
  federation/snapshot.cpp
  federation/authority.cpp
  federation/events.cpp
  federation/federation.cpp
  persist/store.cpp
  persist/file_store.cpp
)
add_library(HeterogeneousAcceleratorFederation::haf_core ALIAS haf_core)
target_include_directories(haf_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(haf_core PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(haf_core PUBLIC Threads::Threads)
haf_apply_warnings(haf_core)

# --- haf_net: framed TCP control plane ------------------------------------
add_library(haf_net
  net/socket.cpp
  net/frame.cpp
  net/messages.cpp
  net/server.cpp
  net/client.cpp
  net/control_plane.cpp
)
add_library(HeterogeneousAcceleratorFederation::haf_net ALIAS haf_net)
target_include_directories(haf_net PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(haf_net PUBLIC haf_core)
if(WIN32)
  target_link_libraries(haf_net PUBLIC ws2_32)
endif()
haf_apply_warnings(haf_net)

# --- haf_adapters: vendor adapters behind narrow interfaces ---------------
add_library(haf_adapters
  adapters/adapter.cpp
  adapters/synthetic.cpp
  adapters/rocm_synthetic.cpp
  adapters/registry.cpp
)
add_library(HeterogeneousAcceleratorFederation::haf_adapters ALIAS haf_adapters)
target_include_directories(haf_adapters PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(haf_adapters PUBLIC haf_core)
haf_apply_warnings(haf_adapters)

# --- Optional real CUDA adapter -------------------------------------------
if(HAF_CUDA_AVAILABLE)
  find_package(CUDAToolkit REQUIRED)
  add_library(haf_adapter_cuda
    adapters/cuda.cpp
    adapters/cuda_probe_kernel.cu
  )
  add_library(HeterogeneousAcceleratorFederation::haf_adapter_cuda ALIAS haf_adapter_cuda)
  target_include_directories(haf_adapter_cuda PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(haf_adapter_cuda PUBLIC haf_core CUDA::cudart)
  set_target_properties(haf_adapter_cuda PROPERTIES CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON)
  # The architecture list comes from the top-level project (see
  # HAF_CUDA_ARCHITECTURES), which detects the local GPU without running a
  # probe executable. No target-level override is applied here.
  target_compile_definitions(haf_adapter_cuda PRIVATE HAF_ENABLE_CUDA=1)
  # The helper applies CXX-only switches; the CUDA translation unit gets the
  # equivalent host-compiler switches through -Xcompiler so that nvcc never
  # receives a raw MSVC flag it would mistake for an input file.
  haf_apply_warnings(haf_adapter_cuda)
  if(MSVC)
    target_compile_options(haf_adapter_cuda PRIVATE
      "$<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/W4,/permissive-,/utf-8,/Zc:__cplusplus,/EHsc>")
    if(HAF_WARNINGS_AS_ERRORS)
      target_compile_options(haf_adapter_cuda PRIVATE "$<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/WX>")
    endif()
    set_target_properties(haf_adapter_cuda PROPERTIES CUDA_PROPAGATE_HOST_FLAGS OFF)
  endif()
else()
  message(STATUS "CUDA toolkit not found: the CUDA adapter is UNSUPPORTED in this build")
endif()
