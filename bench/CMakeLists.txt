add_executable(chrono_rt_bench src/chrono_rt_bench.cpp)

target_compile_definitions(chrono_rt_bench PRIVATE
    "CHRONO_DATA_DIR=\"${CHRONO_DATA_DIR}\""
    "CHRONO_VEHICLE_DATA_DIR=\"${CHRONO_VEHICLE_DATA_DIR}\"")
target_link_libraries(chrono_rt_bench PRIVATE Chrono::models_vehicle)

chrono_copy_dlls(chrono_rt_bench)
