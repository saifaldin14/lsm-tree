# Recovered legacy build configuration

Preserved on 2026-09-19 when editor-resident legacy content appeared after creating the fresh build files. This is historical configuration, not part of the new RocksDB build. It does not establish that the referenced source or results are present.

## CMake

```cmake
cmake_minimum_required(VERSION 3.20)
project(remote_gpu_lsm VERSION 0.3.0 LANGUAGES CXX)

include(CheckLanguage)
option(LSM_ENABLE_CUDA "Build the optional CUDA backend when a CUDA compiler is available" ON)
option(LSM_ENABLE_METAL "Build the native Metal backend on Apple platforms" ON)
option(LSM_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(LSM_WARNINGS_AS_ERRORS "Treat C++ compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

# Capture exactly the source state used by this build, including local changes.
# Results copy this small snapshot rather than relying on an absent git commit.
file(GLOB_RECURSE LSM_PROVENANCE_FILES CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_SOURCE_DIR}" "src/cpp/*" "include/remote_gpu_lsm/*" "tests/*" "configs/*.conf")
list(APPEND LSM_PROVENANCE_FILES CMakeLists.txt CMakePresets.json Makefile)
list(SORT LSM_PROVENANCE_FILES)
set(LSM_SOURCE_DIGESTS "")
foreach(LSM_FILE IN LISTS LSM_PROVENANCE_FILES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/${LSM_FILE}")
    file(SHA256 "${CMAKE_SOURCE_DIR}/${LSM_FILE}" LSM_FILE_DIGEST)
    string(APPEND LSM_SOURCE_DIGESTS "${LSM_FILE} ${LSM_FILE_DIGEST}\n")
    configure_file("${CMAKE_SOURCE_DIR}/${LSM_FILE}" "${CMAKE_BINARY_DIR}/source-snapshot/${LSM_FILE}" COPYONLY)
endforeach()
string(SHA256 LSM_SOURCE_FINGERPRINT "${LSM_SOURCE_DIGESTS}")
file(WRITE "${CMAKE_BINARY_DIR}/source-snapshot/SHA256SUMS" "${LSM_SOURCE_DIGESTS}")

set(LSM_SOURCES
    src/cpp/model.cpp
    src/cpp/storage.cpp
    src/cpp/backend_registry.cpp
    src/cpp/cpu_backend.cpp
    src/cpp/cpu_dense_backend.cpp
    src/cpp/experiment.cpp
)

if(LSM_ENABLE_CUDA)
    check_language(CUDA)
endif()

if(LSM_ENABLE_CUDA AND CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    list(APPEND LSM_SOURCES src/cpp/cuda_backend.cu)
    set(LSM_HAS_CUDA TRUE)
else()
    set(LSM_HAS_CUDA FALSE)
endif()

if(APPLE AND LSM_ENABLE_METAL)
    enable_language(OBJCXX)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    list(APPEND LSM_SOURCES src/cpp/metal_backend.mm)
    set(LSM_HAS_METAL TRUE)
else()
    set(LSM_HAS_METAL FALSE)
endif()

add_library(remote_gpu_lsm ${LSM_SOURCES})
target_include_directories(remote_gpu_lsm PUBLIC include)
target_compile_features(remote_gpu_lsm PUBLIC cxx_std_20)
target_compile_definitions(remote_gpu_lsm PRIVATE
    "LSM_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\""
    "LSM_PROJECT_VERSION=\"${PROJECT_VERSION}\""
    "LSM_SOURCE_FINGERPRINT=\"${LSM_SOURCE_FINGERPRINT}\""
    "LSM_SOURCE_SNAPSHOT=\"${CMAKE_BINARY_DIR}/source-snapshot\""
)
target_link_libraries(remote_gpu_lsm PUBLIC Threads::Threads)
if(LSM_HAS_CUDA)
    target_compile_definitions(remote_gpu_lsm PUBLIC LSM_WITH_CUDA=1)
    set_target_properties(remote_gpu_lsm PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED YES)
endif()
if(LSM_HAS_METAL)
    target_compile_definitions(remote_gpu_lsm PUBLIC LSM_WITH_METAL=1)
    target_compile_options(remote_gpu_lsm PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(remote_gpu_lsm PRIVATE ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(remote_gpu_lsm PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>)
    if(LSM_WARNINGS_AS_ERRORS)
        target_compile_options(remote_gpu_lsm PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Werror>)
    endif()
    if(LSM_ENABLE_SANITIZERS)
        target_compile_options(remote_gpu_lsm PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(remote_gpu_lsm PUBLIC -fsanitize=address,undefined)
    endif()
endif()

add_executable(lsm_storage_server src/cpp/storage_main.cpp)
target_link_libraries(lsm_storage_server PRIVATE remote_gpu_lsm)

add_executable(lsm_compact_bench src/cpp/main.cpp)
target_link_libraries(lsm_compact_bench PRIVATE remote_gpu_lsm)
add_dependencies(lsm_compact_bench lsm_storage_server)

add_executable(lsm_compact_profile src/cpp/profile_main.cpp)
target_link_libraries(lsm_compact_profile PRIVATE remote_gpu_lsm)

include(CTest)
if(BUILD_TESTING)
    add_executable(lsm_tests tests/test_main.cpp)
    target_link_libraries(lsm_tests PRIVATE remote_gpu_lsm)
    add_test(NAME lsm_tests COMMAND lsm_tests)

    add_executable(lsm_storage_tests tests/storage_test.cpp)
    target_link_libraries(lsm_storage_tests PRIVATE remote_gpu_lsm)
    add_dependencies(lsm_storage_tests lsm_storage_server)
    add_test(NAME lsm_storage_tests COMMAND lsm_storage_tests)

    add_executable(lsm_experiment_tests tests/experiment_test.cpp)
    target_link_libraries(lsm_experiment_tests PRIVATE remote_gpu_lsm)
    add_dependencies(lsm_experiment_tests lsm_storage_server)
    add_test(NAME lsm_experiment_tests COMMAND lsm_experiment_tests)
endif()

message(STATUS "CUDA backend: ${LSM_HAS_CUDA}")
message(STATUS "Metal backend: ${LSM_HAS_METAL}")
```

## Presets

```json
{
  "version": 6,
  "cmakeMinimumRequired": {"major": 3, "minor": 20, "patch": 0},
  "configurePresets": [
    {"name": "release", "displayName": "Release", "generator": "Unix Makefiles", "binaryDir": "${sourceDir}/build/release", "cacheVariables": {"CMAKE_BUILD_TYPE": "Release", "BUILD_TESTING": "ON"}},
    {"name": "sanitizers", "displayName": "Debug with sanitizers", "inherits": "release", "binaryDir": "${sourceDir}/build/sanitizers", "cacheVariables": {"CMAKE_BUILD_TYPE": "Debug", "LSM_ENABLE_SANITIZERS": "ON", "LSM_ENABLE_CUDA": "OFF"}}
  ],
  "buildPresets": [{"name": "release", "configurePreset": "release"}, {"name": "sanitizers", "configurePreset": "sanitizers"}],
  "testPresets": [{"name": "release", "configurePreset": "release", "output": {"outputOnFailure": true}}, {"name": "sanitizers", "configurePreset": "sanitizers", "output": {"outputOnFailure": true}}]
}
```