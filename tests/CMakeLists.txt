# Shared harness: every suite links this and uses the same main().
add_library(haf_test_framework STATIC
  framework/test_harness.cpp
  support/environment.cpp
  support/profiles.cpp
  support/process.cpp
)
target_include_directories(haf_test_framework PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(haf_test_framework PUBLIC haf_core haf_net haf_adapters)
if(TARGET haf_adapter_cuda)
  target_link_libraries(haf_test_framework PUBLIC haf_adapter_cuda)
  target_compile_definitions(haf_test_framework PUBLIC HAF_HAVE_CUDA_ADAPTER=1)
endif()
haf_apply_warnings(haf_test_framework)

# The artifact locations are compile-time constants for the whole test tree.
# They are defined on the framework library, which is where the helpers that
# consume them are compiled; defining them per suite would leave the library
# itself without them.
target_compile_definitions(haf_test_framework PUBLIC
  HAF_SUITE_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
  HAF_APPS_BINARY_DIR="${CMAKE_BINARY_DIR}/apps")

function(haf_add_suite name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE haf_test_framework)
  haf_apply_warnings(${name})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 1800)
endfunction()

haf_add_suite(haf_unit_tests unit/core_tests.cpp unit/model_tests.cpp unit/engine_tests.cpp)
haf_add_suite(haf_property_tests property/property_tests.cpp)
haf_add_suite(haf_concurrency_tests concurrency/concurrency_tests.cpp)
haf_add_suite(haf_adversarial_tests adversarial/adversarial_tests.cpp)
haf_add_suite(haf_persistence_tests persistence/persistence_tests.cpp)
haf_add_suite(haf_multiprocess_tests multiprocess/multiprocess_tests.cpp)
haf_add_suite(haf_integration_tests integration/integration_tests.cpp)
if(TARGET haf_adapter_cuda)
  haf_add_suite(haf_cuda_tests hardware/cuda_tests.cpp)
endif()
