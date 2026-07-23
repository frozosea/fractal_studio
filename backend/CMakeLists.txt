cmake_minimum_required(VERSION 3.18)
project(fractal_studio_backend LANGUAGES CXX)
enable_testing()

# ---- Optional CUDA ----
# Enable CUDA if nvcc is available. The CUDA sources are only compiled when
# this succeeds, and the rest of the build is unaffected if CUDA is absent.
include(CheckLanguage)
check_language(CUDA)
if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
    set(HAS_CUDA ON)
    message(STATUS "CUDA enabled: ${CMAKE_CUDA_COMPILER}")
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES OR CMAKE_CUDA_ARCHITECTURES STREQUAL "")
        if(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 13.0)
            set(CMAKE_CUDA_ARCHITECTURES "75;86;89" CACHE STRING "CUDA architectures" FORCE)
            message(WARNING "CUDA >= 13 detected; Pascal sm_61 offline compilation is no longer supported. Use CUDA 12.x to build GTX1050/Pascal binaries.")
        else()
            set(CMAKE_CUDA_ARCHITECTURES "61;75;86;89" CACHE STRING "CUDA architectures" FORCE)
        endif()
    elseif(CMAKE_CUDA_COMPILER_VERSION VERSION_GREATER_EQUAL 13.0 AND
           CMAKE_CUDA_ARCHITECTURES MATCHES "(^|;)61(;|$)")
        message(WARNING "CMAKE_CUDA_ARCHITECTURES includes 61, but CUDA >= 13 removed Pascal sm_61 offline compilation/library support. Configure with CUDA 12.x for GTX1050 support.")
    endif()
else()
    set(HAS_CUDA OFF)
    message(STATUS "CUDA not found — GPU kernels disabled")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(OpenMP QUIET)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(OpenCV REQUIRED COMPONENTS core imgcodecs imgproc videoio)

add_executable(fractal_studio_backend
    src/main.cpp

    # Core
    src/core/path_guard.cpp
    src/core/job_runner.cpp
    src/core/resource_manager.cpp
    src/core/http_server.cpp
    src/core/hardware_probe.cpp
    src/core/db.cpp

    # API routes
    src/api/routes_runs.cpp
    src/api/routes_modules.cpp
    src/api/routes_map.cpp
    src/api/routes_ln.cpp
    src/api/routes_mesh.cpp
    src/api/routes_video.cpp
    src/api/routes_points.cpp
    src/api/routes_artifacts.cpp
    src/api/routes_benchmark.cpp
    src/api/routes_variants.cpp
    src/api/routes_compute_v1.cpp
    src/api/routes_platform_compute.cpp

    # Native compute
    src/compute/colorize.cpp
    src/compute/cpu_features.cpp
    src/compute/ln_map.cpp
    src/compute/ln_map_avx2.cpp
    src/compute/ln_map_avx512.cpp
    src/compute/map_kernel.cpp
    src/compute/orbit_program.cpp
    src/compute/orbit_program_json.cpp
    src/compute/engine_select.cpp
    src/compute/map_kernel_avx2.cpp
    src/compute/map_kernel_avx512.cpp
    src/compute/transition_kernel.cpp
    src/compute/transition_kernel_avx2.cpp
    src/compute/image_io.cpp
    src/compute/mesh_io.cpp
    src/compute/marching_cubes.cpp
    src/compute/special_points.cpp
    src/compute/special_points_hp.cpp
    src/compute/transition_volume.cpp
    src/compute/transition_volume_avx2.cpp
    src/compute/hs/heightfield_mesh.cpp
    src/compute/newton/mandelbrot_sp.cpp
    src/compute/tile_scheduler.cpp
    src/compute/perturbation.cpp
    src/compute/perturbation_avx2.cpp
    src/compute/perturbation_avx512.cpp

    # Hardware probes
    src/adapters/cuda_probe.cpp
    src/adapters/openmp_probe.cpp
)

# Conditionally compile the CUDA kernel and link it in.
if(HAS_CUDA)
    add_library(fractal_cuda STATIC
        src/compute/cuda/map_kernel.cu
        src/compute/cuda/ln_map.cu
        src/compute/cuda/video_warp.cu
        src/compute/cuda/transition_volume.cu
        src/compute/cuda/transition_kernel.cu
        src/compute/cuda/special_points.cu
        src/compute/cuda/perturb_kernel.cu
    )
    target_include_directories(fractal_cuda PRIVATE
        src
        src/include
        ${OpenCV_INCLUDE_DIRS}
    )
    set_target_properties(fractal_cuda PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
    )
    target_compile_options(fractal_cuda PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:--expt-relaxed-constexpr -O3>
    )
    target_link_libraries(fractal_studio_backend PRIVATE fractal_cuda cuda)
    target_compile_definitions(fractal_studio_backend PRIVATE HAS_CUDA_KERNEL=1)
endif()

target_include_directories(fractal_studio_backend PRIVATE
    src/include
    src
    ${OpenCV_INCLUDE_DIRS}
)

if(OpenMP_CXX_FOUND)
    target_link_libraries(fractal_studio_backend PRIVATE OpenMP::OpenMP_CXX)
    message(STATUS "OpenMP enabled")
else()
    message(STATUS "OpenMP not found — parallel rendering will be single-threaded")
endif()

target_link_libraries(fractal_studio_backend PRIVATE
    sqlite3
    ${OpenCV_LIBS}
    dl          # dlopen/dlsym for custom variant shared libraries
    OpenSSL::Crypto
)

target_compile_options(fractal_studio_backend PRIVATE
    -Wall -Wextra -Wno-unused-parameter -O3
)

add_executable(special_points_smoke
    src/tests/special_points_smoke.cpp
    src/compute/special_points.cpp
    src/compute/special_points_hp.cpp
)
target_include_directories(special_points_smoke PRIVATE
    src
    src/include
)
target_compile_options(special_points_smoke PRIVATE
    -Wall -Wextra -Wno-unused-parameter -O3
)
if(OpenMP_CXX_FOUND)
    target_link_libraries(special_points_smoke PRIVATE OpenMP::OpenMP_CXX)
endif()
add_test(NAME special_points_smoke COMMAND special_points_smoke)

add_executable(artifact_routes_smoke
    src/tests/artifact_routes_smoke.cpp
    src/api/routes_artifacts.cpp
)
target_include_directories(artifact_routes_smoke PRIVATE
    src
    src/include
)
target_compile_options(artifact_routes_smoke PRIVATE
    -Wall -Wextra -Wno-unused-parameter -O3
)
add_test(NAME artifact_routes_smoke COMMAND artifact_routes_smoke)

add_executable(ln_map_reuse_smoke
    src/tests/ln_map_reuse_smoke.cpp
)
target_include_directories(ln_map_reuse_smoke PRIVATE
    src
    src/include
)
target_compile_options(ln_map_reuse_smoke PRIVATE
    -Wall -Wextra -Wno-unused-parameter -O3
)
add_test(NAME ln_map_reuse_smoke COMMAND ln_map_reuse_smoke)

add_executable(job_runner_reconcile_smoke
    src/tests/job_runner_reconcile_smoke.cpp
    src/core/db.cpp
    src/core/job_runner.cpp
)
target_include_directories(job_runner_reconcile_smoke PRIVATE
    src
    src/include
)
target_compile_options(job_runner_reconcile_smoke PRIVATE
    -Wall -Wextra -Wno-unused-parameter -O3
)
target_link_libraries(job_runner_reconcile_smoke PRIVATE sqlite3)
add_test(NAME job_runner_reconcile_smoke COMMAND job_runner_reconcile_smoke)

add_executable(http_range_smoke
    src/tests/http_range_smoke.cpp
)
target_include_directories(http_range_smoke PRIVATE
    src
    src/include
)
target_compile_options(http_range_smoke PRIVATE
    -Wall -Wextra -Wno-unused-parameter -O3
)
add_test(NAME http_range_smoke COMMAND http_range_smoke)

add_executable(compute_path_diff
    src/tests/compute_path_diff.cpp
    src/compute/colorize.cpp
    src/compute/cpu_features.cpp
    src/compute/ln_map.cpp
    src/compute/ln_map_avx2.cpp
    src/compute/ln_map_avx512.cpp
    src/compute/map_kernel.cpp
    src/compute/orbit_program.cpp
    src/compute/engine_select.cpp
    src/compute/map_kernel_avx2.cpp
    src/compute/map_kernel_avx512.cpp
    src/compute/transition_kernel.cpp
    src/compute/transition_kernel_avx2.cpp
    src/compute/transition_volume.cpp
    src/compute/transition_volume_avx2.cpp
    src/compute/tile_scheduler.cpp
    src/compute/perturbation.cpp
    src/compute/perturbation_avx2.cpp
    src/compute/perturbation_avx512.cpp
)
target_include_directories(compute_path_diff PRIVATE
    src
    src/include
    ${OpenCV_INCLUDE_DIRS}
)
target_compile_options(compute_path_diff PRIVATE
    -Wall -Wextra -Wno-unused-parameter -O3
)
target_link_libraries(compute_path_diff PRIVATE
    ${OpenCV_LIBS}
    OpenSSL::Crypto
)

add_executable(orbit_program_smoke
    src/tests/orbit_program_smoke.cpp
    src/compute/orbit_program.cpp
    src/compute/orbit_program_json.cpp
)
target_include_directories(orbit_program_smoke PRIVATE src src/include)
target_compile_options(orbit_program_smoke PRIVATE -Wall -Wextra -Wno-unused-parameter -O3)
target_link_libraries(orbit_program_smoke PRIVATE OpenSSL::Crypto)
add_test(NAME orbit_program_smoke COMMAND orbit_program_smoke)

add_executable(hs_orbit_smoke
    src/tests/hs_orbit_smoke.cpp
    src/compute/hs/heightfield_mesh.cpp
    src/compute/orbit_program.cpp
)
target_include_directories(hs_orbit_smoke PRIVATE src src/include)
target_compile_options(hs_orbit_smoke PRIVATE -Wall -Wextra -Wno-unused-parameter -O3)
target_link_libraries(hs_orbit_smoke PRIVATE OpenSSL::Crypto)
if(OpenMP_CXX_FOUND)
    target_link_libraries(hs_orbit_smoke PRIVATE OpenMP::OpenMP_CXX)
endif()
add_test(NAME hs_orbit_smoke COMMAND hs_orbit_smoke)
if(OpenMP_CXX_FOUND)
    target_link_libraries(compute_path_diff PRIVATE OpenMP::OpenMP_CXX)
endif()
if(HAS_CUDA)
    target_link_libraries(compute_path_diff PRIVATE fractal_cuda cuda)
    target_compile_definitions(compute_path_diff PRIVATE HAS_CUDA_KERNEL=1)
endif()
add_test(NAME compute_path_diff COMMAND compute_path_diff)

set(FSD_PYTEST_PYTHON "" CACHE FILEPATH "Python interpreter with pytest installed")
if(FSD_PYTEST_PYTHON)
    set(_FSD_PYTEST_PYTHON "${FSD_PYTEST_PYTHON}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/platform-venv/bin/python")
    set(_FSD_PYTEST_PYTHON
        "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/platform-venv/bin/python")
else()
    set(_FSD_PYTEST_PYTHON "${Python3_EXECUTABLE}")
endif()
execute_process(
    COMMAND "${_FSD_PYTEST_PYTHON}" -c "import pytest"
    RESULT_VARIABLE _FSD_PYTEST_IMPORT_RESULT
    OUTPUT_QUIET ERROR_QUIET
)
if(_FSD_PYTEST_IMPORT_RESULT EQUAL 0)
    add_test(
        NAME compute_v1_http_contract
        COMMAND "${_FSD_PYTEST_PYTHON}" -m pytest -q
                ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/compute_v1
                --backend-binary=$<TARGET_FILE:fractal_studio_backend>
                --studio-root=${CMAKE_CURRENT_SOURCE_DIR}/..
    )
    set_tests_properties(compute_v1_http_contract PROPERTIES TIMEOUT 60)
else()
    message(WARNING
        "pytest not found; Compute v1 HTTP contract tests are disabled. "
        "Install backend/requirements-test.txt or set FSD_PYTEST_PYTHON.")
endif()

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES quadmath)
check_cxx_source_compiles("
#include <quadmath.h>
int main() {
    __float128 x = static_cast<__float128>(1.0);
    return sqrtq(x) > 0 ? 0 : 1;
}
" COMPILER_HAS_FLOAT128)
unset(CMAKE_REQUIRED_LIBRARIES)
if(COMPILER_HAS_FLOAT128)
    target_compile_definitions(fractal_studio_backend PRIVATE FSD_HAS_FLOAT128=1)
    target_compile_definitions(compute_path_diff PRIVATE FSD_HAS_FLOAT128=1)
    target_link_libraries(fractal_studio_backend PRIVATE quadmath)
    target_link_libraries(compute_path_diff PRIVATE quadmath)
    message(STATUS "__float128/libquadmath enabled")
else()
    message(STATUS "__float128/libquadmath unavailable — fp128 scalar path disabled")
endif()

# ---- Optional MPFR (arbitrary precision for deep-zoom perturbation) ----
find_library(MPFR_LIB mpfr)
find_path(MPFR_INCLUDE_DIR mpfr.h)
if(MPFR_LIB AND MPFR_INCLUDE_DIR)
    target_compile_definitions(fractal_studio_backend PRIVATE FSD_HAS_MPFR=1)
    target_compile_definitions(compute_path_diff PRIVATE FSD_HAS_MPFR=1)
    target_compile_definitions(special_points_smoke PRIVATE FSD_HAS_MPFR=1)
    target_include_directories(fractal_studio_backend PRIVATE ${MPFR_INCLUDE_DIR})
    target_include_directories(compute_path_diff PRIVATE ${MPFR_INCLUDE_DIR})
    target_include_directories(special_points_smoke PRIVATE ${MPFR_INCLUDE_DIR})
    target_link_libraries(fractal_studio_backend PRIVATE ${MPFR_LIB})
    target_link_libraries(compute_path_diff PRIVATE ${MPFR_LIB})
    target_link_libraries(special_points_smoke PRIVATE ${MPFR_LIB})
    message(STATUS "MPFR enabled (${MPFR_LIB})")
else()
    message(STATUS "MPFR not found — arbitrary-precision perturbation disabled")
endif()

# AVX-512 kernel: compile with AVX-512F + IFMA52 intrinsics enabled when the
# compiler supports them. avx512_available() does a runtime CPUID check so the
# binary falls back to OpenMP on CPUs without AVX-512. On compilers/arches that
# don't support the flags (ARM, old GCC), the file still compiles — all
# intrinsic code is inside #if defined(__AVX512F__) guards and avx512_available()
# returns false unconditionally.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-mavx2" COMPILER_HAS_AVX2)
check_cxx_compiler_flag("-mfma" COMPILER_HAS_FMA)
if(COMPILER_HAS_AVX2 AND COMPILER_HAS_FMA)
    set_source_files_properties(
        src/compute/map_kernel_avx2.cpp
        src/compute/ln_map_avx2.cpp
        src/compute/transition_volume_avx2.cpp
        src/compute/transition_kernel_avx2.cpp
        src/compute/perturbation_avx2.cpp
        PROPERTIES COMPILE_FLAGS "-mavx2 -mfma")
    target_compile_definitions(fractal_studio_backend PRIVATE HAS_AVX2_KERNEL=1)
    target_compile_definitions(compute_path_diff PRIVATE HAS_AVX2_KERNEL=1)
    message(STATUS "AVX2/FMA compile flags enabled")
else()
    message(STATUS "Compiler lacks AVX2/FMA support — AVX2 paths disabled at compile time")
endif()

check_cxx_compiler_flag("-mavx512f" COMPILER_HAS_AVX512F)
if(COMPILER_HAS_AVX512F)
    set_source_files_properties(
        src/compute/map_kernel_avx512.cpp
        src/compute/ln_map_avx512.cpp
        src/compute/perturbation_avx512.cpp
        PROPERTIES
        COMPILE_FLAGS "-mavx512f -mavx512dq -mavx512ifma")
    target_compile_definitions(fractal_studio_backend PRIVATE HAS_AVX512_KERNEL=1)
    target_compile_definitions(compute_path_diff PRIVATE HAS_AVX512_KERNEL=1)
    message(STATUS "AVX-512 compile flags enabled")
else()
    message(STATUS "Compiler lacks AVX-512 support — AVX-512 path disabled at compile time")
endif()
