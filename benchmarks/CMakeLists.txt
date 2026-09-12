add_executable(haf_bench federation_bench.cpp)
target_link_libraries(haf_bench PRIVATE haf_core haf_adapters)
target_include_directories(haf_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
haf_apply_warnings(haf_bench)
