#!/usr/bin/env bash
set -euo pipefail

readonly source_dir=/src
readonly build_dir=/tmp/webcpp-build
readonly asan_build_dir=/tmp/webcpp-build-asan
readonly engine_dir="${source_dir}/webcpp-engine"

generate_test_certificate() {
    mkdir -p "${engine_dir}/test/certs"
    openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
        -keyout "${engine_dir}/test/certs/server.key" \
        -out "${engine_dir}/test/certs/server.crt" \
        -subj '/CN=localhost' >/dev/null 2>&1
}

configure_and_test() {
    local build_path=$1
    local build_type=$2
    shift 2
    cmake -S "${source_dir}" -B "${build_path}" -G Ninja \
        -DCMAKE_BUILD_TYPE="${build_type}" \
        -DWEBCPP_MYSQL=ON \
        -DWEBCPP_RPC=OFF \
        "$@"
    cmake --build "${build_path}" --parallel 2
    # This module enables testing in its own CMake directory. The repository
    # root does not currently call enable_testing(), so point CTest at the
    # directory that owns the generated test manifest.
    ctest --test-dir "${build_path}/webcpp-engine" --output-on-failure --timeout 90
}

generate_test_certificate

# Compile the complete engine with portable warning checks. Warnings are shown
# but not promoted to errors until existing warnings have been triaged.
configure_and_test "${build_dir}" \
    Debug \
    '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Wpedantic'

# Run the leak and undefined-behaviour detectors against every registered test.
export ASAN_OPTIONS='detect_leaks=1:halt_on_error=1:strict_init_order=1'
export UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'
configure_and_test "${asan_build_dir}" \
    Debug \
    '-DCMAKE_CXX_FLAGS_DEBUG=-O0 -g' \
    '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
    '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined' \
    '-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined'

# Keep this signal-focused: system headers and style-only findings are excluded.
cppcheck --quiet --error-exitcode=1 --enable=warning,portability \
    --suppress=missingIncludeSystem \
    --inline-suppr \
    "${engine_dir}/cache" "${engine_dir}/config" "${engine_dir}/middleware" \
    "${engine_dir}/net" "${engine_dir}/protocol" "${engine_dir}/router" \
    "${engine_dir}/server" "${engine_dir}/src"
