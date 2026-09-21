cmake_minimum_required(VERSION 3.25)
project(lsm_lab VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)

find_package(ZLIB MODULE REQUIRED)
find_package(lz4 CONFIG REQUIRED)

include(FetchContent)
set(LSM_ROCKSDB_COMMIT abeebd9630f11bd08c28b7bd43c7bdfc62050654)
set(WITH_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_CORE_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_TRACE_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_GFLAGS OFF CACHE BOOL "" FORCE)
set(WITH_LIBURING OFF CACHE BOOL "" FORCE)
set(WITH_LZ4 ON CACHE BOOL "" FORCE)
set(ROCKSDB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(FAIL_ON_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_RTTI ON CACHE STRING "" FORCE)
set(PORTABLE ON CACHE STRING "" FORCE)
FetchContent_Declare(rocksdb
  URL "https://codeload.github.com/facebook/rocksdb/tar.gz/${LSM_ROCKSDB_COMMIT}"
  URL_HASH SHA256=25dee0decb85ea9a3c8a706b8b627fd67f218e35efae59e23997a4234db32198
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(rocksdb)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(LSM_PROTO_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${LSM_PROTO_DIR}")
set(LSM_PROTO_SOURCES)
foreach(protocol benchmark compaction)
  set(protocol_file "${CMAKE_CURRENT_SOURCE_DIR}/proto/${protocol}.proto")
  set(protocol_outputs
    "${LSM_PROTO_DIR}/${protocol}.pb.cc" "${LSM_PROTO_DIR}/${protocol}.pb.h"
    "${LSM_PROTO_DIR}/${protocol}.grpc.pb.cc" "${LSM_PROTO_DIR}/${protocol}.grpc.pb.h")
  add_custom_command(OUTPUT ${protocol_outputs}
    COMMAND protobuf::protoc
      "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto"
      "--cpp_out=${LSM_PROTO_DIR}" "--grpc_out=${LSM_PROTO_DIR}"
      "--plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
      "${protocol_file}"
    DEPENDS "${protocol_file}" protobuf::protoc gRPC::grpc_cpp_plugin
    VERBATIM)
  list(APPEND LSM_PROTO_SOURCES ${protocol_outputs})
endforeach()
add_library(lsm_protocol ${LSM_PROTO_SOURCES})
target_include_directories(lsm_protocol SYSTEM PUBLIC "${LSM_PROTO_DIR}")
target_link_libraries(lsm_protocol PUBLIC protobuf::libprotobuf gRPC::grpc++)

add_library(lsm_runtime src/runtime.cpp)
target_include_directories(lsm_runtime PUBLIC include)
target_link_libraries(lsm_runtime PUBLIC rocksdb gRPC::grpc++ nlohmann_json::nlohmann_json PRIVATE OpenSSL::Crypto)
target_compile_definitions(lsm_runtime PRIVATE LSM_ENGINE_FINGERPRINT="rocksdb-${LSM_ROCKSDB_COMMIT}-native-v1")
target_compile_options(lsm_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(lsm_config src/spec.cpp)
target_include_directories(lsm_config PUBLIC include)
target_link_libraries(lsm_config PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(lsm_config PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(lsm_engine_check tests/engine_check.cpp)
target_link_libraries(lsm_engine_check PRIVATE rocksdb)
target_compile_options(lsm_engine_check PRIVATE -Wall -Wextra -Wpedantic -Werror)
enable_testing()
add_test(NAME engine.persistence COMMAND lsm_engine_check)
set_tests_properties(engine.persistence PROPERTIES TIMEOUT 90 LABELS "engine;integration")

add_executable(lsm_config_tests tests/spec_test.cpp)
target_link_libraries(lsm_config_tests PRIVATE lsm_config GTest::gtest_main)
target_compile_options(lsm_config_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME config.validation COMMAND lsm_config_tests)
set_tests_properties(config.validation PROPERTIES TIMEOUT 30 LABELS "config;unit")

add_library(lsm_engine src/engine/dataset.cpp)
target_link_libraries(lsm_engine PUBLIC lsm_config rocksdb PRIVATE OpenSSL::Crypto)
target_compile_options(lsm_engine PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_library(lsm_remote src/engine/remote_compaction.cpp)
target_link_libraries(lsm_remote PUBLIC lsm_engine lsm_runtime lsm_protocol)
target_compile_options(lsm_remote PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(lsm_db apps/lsm_db.cpp)
target_link_libraries(lsm_db PRIVATE lsm_remote)
target_compile_options(lsm_db PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_library(lsm_worker_core src/worker/worker.cpp)
target_link_libraries(lsm_worker_core PUBLIC lsm_engine lsm_runtime lsm_protocol)
target_compile_options(lsm_worker_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(lsm_worker apps/lsm_worker.cpp)
target_link_libraries(lsm_worker PRIVATE lsm_worker_core)
target_compile_options(lsm_worker PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(lsm_worker_tests tests/worker_test.cpp)
target_link_libraries(lsm_worker_tests PRIVATE lsm_worker_core lsm_remote GTest::gtest_main)
target_compile_definitions(lsm_worker_tests PRIVATE LSM_WORKER_EXE="$<TARGET_FILE:lsm_worker>" LSM_DB_EXE="$<TARGET_FILE:lsm_db>")
add_dependencies(lsm_worker_tests lsm_worker lsm_db)
target_compile_options(lsm_worker_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME worker.lifecycle COMMAND lsm_worker_tests)
set_tests_properties(worker.lifecycle PROPERTIES TIMEOUT 90 LABELS "worker;integration")
add_executable(lsm_dataset_tests tests/dataset_test.cpp)
target_link_libraries(lsm_dataset_tests PRIVATE lsm_engine GTest::gtest_main)
target_compile_options(lsm_dataset_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME engine.datasets COMMAND lsm_dataset_tests)
set_tests_properties(engine.datasets PROPERTIES TIMEOUT 120 LABELS "engine;integration")

add_executable(lsm_admin apps/lsm_admin.cpp)
target_link_libraries(lsm_admin PRIVATE lsm_engine)
target_compile_definitions(lsm_admin PRIVATE LSM_ROCKSDB_COMMIT="${LSM_ROCKSDB_COMMIT}")
target_compile_options(lsm_admin PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME admin.config COMMAND lsm_admin validate "${CMAKE_CURRENT_SOURCE_DIR}/configs/local-smoke.json")
set_tests_properties(admin.config PROPERTIES TIMEOUT 30 LABELS "config;cli")
