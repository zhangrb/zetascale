add_definitions(-std=gnu99 -fgnu89-inline)

add_executable(btest_life_cycle ${test_src_file}
		btest_life_cycle.c btest_common.c)
#set_property(TARGET btest_life_cycle PROPERTY COMPILE_DEFINITIONS BTREE_MODE)

add_executable(btest_range ${test_src_file}
		btest_range.c btest_common.c)
#set_property(TARGET btest_range PROPERTY COMPILE_DEFINITIONS BTREE_MODE)

add_executable(query_perf_comp ${test_src_file}
		query_perf_comp.c)
#set_property(TARGET query_perf_comp PROPERTY COMPILE_DEFINITIONS BTREE_MODE)

target_link_libraries( btest_life_cycle btree fdfdll dl pthread aio event snappy)
target_link_libraries( btest_range btree fdfdll dl pthread aio event snappy)
target_link_libraries( query_perf_comp btree fdfdll dl pthread aio event snappy)

add_test(btree_self_test btest_life_cycle)
add_test(btree_self_test2K btest_life_cycle -k 1960)
