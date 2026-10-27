# Imports the Chrono libraries this project uses from a prebuilt Chrono install.
#
# Why not find_package(Chrono)? The Win64 installer's ChronoTargets.cmake declares
# every Chrono module, including link interfaces on dependencies the installer does
# not ship (VSG, HDF5, CUDA, MKL, urdfdom, ...), so generation fails unless all of
# them are present. This module declares only what we link, with the compile
# settings copied from that file. They must match the DLLs' build: /arch:AVX2 in
# particular changes Eigen's alignment, and Eigen types cross the DLL boundary.
#
# Inputs (cache):  CHRONO_ROOT          Chrono install root (has bin/, lib/, include/, data/)
#                  EIGEN3_INCLUDE_DIR   Eigen headers (Chrono 10.0.0 was built with Eigen 5.0.1)
# Defines:         Chrono::core, Chrono::vehicle, Chrono::models_vehicle
#                  CHRONO_DATA_DIR, CHRONO_VEHICLE_DATA_DIR
#                  chrono_copy_dlls(<target>)

set(CHRONO_ROOT "C:/dev/chrono" CACHE PATH "Chrono install root")
set(EIGEN3_INCLUDE_DIR "C:/dev/eigen-5.0.0" CACHE PATH "Eigen3 include directory")

if(NOT EXISTS "${CHRONO_ROOT}/include/chrono/physics/ChSystem.h")
    message(FATAL_ERROR "CHRONO_ROOT=${CHRONO_ROOT} does not look like a Chrono install")
endif()
if(NOT EXISTS "${EIGEN3_INCLUDE_DIR}/Eigen/Core")
    message(FATAL_ERROR "EIGEN3_INCLUDE_DIR=${EIGEN3_INCLUDE_DIR} has no Eigen/Core")
endif()

set(CHRONO_DATA_DIR "${CHRONO_ROOT}/data/")
set(CHRONO_VEHICLE_DATA_DIR "${CHRONO_ROOT}/data/vehicle/")

function(_chrono_import target lib)
    add_library(${target} SHARED IMPORTED GLOBAL)
    set_target_properties(${target} PROPERTIES
        IMPORTED_CONFIGURATIONS "RELEASE"
        IMPORTED_IMPLIB_RELEASE "${CHRONO_ROOT}/lib/${lib}.lib"
        IMPORTED_LOCATION_RELEASE "${CHRONO_ROOT}/bin/${lib}.dll"
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO "RELEASE")
endfunction()

_chrono_import(Chrono::core Chrono_core)
set_target_properties(Chrono::core PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES
        "${CHRONO_ROOT}/include;${CHRONO_ROOT}/include/chrono;${CHRONO_ROOT}/include/chrono/collision/bullet;${CHRONO_ROOT}/include/chrono_thirdparty/HACDv2;${CHRONO_ROOT}/include/chrono_thirdparty/yaml-cpp/include;${EIGEN3_INCLUDE_DIR}"
    INTERFACE_COMPILE_DEFINITIONS "_CRT_SECURE_NO_DEPRECATE;_SCL_SECURE_NO_DEPRECATE;NOMINMAX;CH_IGNORE_DEPRECATED"
    INTERFACE_COMPILE_OPTIONS
        "/arch:AVX2;/Zc:__cplusplus;/EHsc;/bigobj;/wd4100;/wd4127;/wd4201;/wd4244;/wd4250;/wd4251;/wd4275;/wd4324;/wd4505;/wd5038"
    INTERFACE_COMPILE_FEATURES "cxx_std_17")

_chrono_import(Chrono::vehicle Chrono_vehicle)
set_target_properties(Chrono::vehicle PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CHRONO_ROOT}/include/chrono_vehicle"
    INTERFACE_LINK_LIBRARIES "Chrono::core")

_chrono_import(Chrono::models_vehicle ChronoModels_vehicle)
set_target_properties(Chrono::models_vehicle PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CHRONO_ROOT}/include/chrono_models/vehicle"
    INTERFACE_LINK_LIBRARIES "Chrono::core;Chrono::vehicle")

# Runtime DLLs: the three above plus what they load from Chrono's bin/ (checked with dumpbin).
set(CHRONO_RUNTIME_DLLS
    Chrono_core.dll Chrono_vehicle.dll ChronoModels_vehicle.dll
    Chrono_fsi.dll Chrono_fsisph.dll yaml-cpp.dll)

function(chrono_copy_dlls target)
    set(_dlls "")
    foreach(_dll ${CHRONO_RUNTIME_DLLS})
        list(APPEND _dlls "${CHRONO_ROOT}/bin/${_dll}")
    endforeach()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dlls} "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Copying Chrono DLLs")
    set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endfunction()
