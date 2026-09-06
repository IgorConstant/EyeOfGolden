#!/usr/bin/env bash
# Host tests for the 3DS stereo clip-shift math and the scripted frontend
# probe. Both modules are platform-neutral C with no libctru dependency.
set -euo pipefail
repo_dir=${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
test_dir="${repo_dir}/build/host-tests/3ds-frontend-diagnostics"
mkdir -p "${test_dir}"
cc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I"${repo_dir}/platform/3ds/include" \
    -I"${repo_dir}/port/include" \
    "${repo_dir}/platform/3ds/source/ge_3ds_stereo.c" \
    "${repo_dir}/platform/3ds/tests/test_ge_3ds_stereo.c" \
    -lm -o "${test_dir}/test_ge_3ds_stereo"
cc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I"${repo_dir}/platform/3ds/include" \
    -I"${repo_dir}/port/include" \
    "${repo_dir}/platform/3ds/source/ge_3ds_frontend_probe.c" \
    "${repo_dir}/platform/3ds/tests/test_ge_3ds_frontend_probe.c" \
    -lm -o "${test_dir}/test_ge_3ds_frontend_probe"
cd "${test_dir}"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 ./test_ge_3ds_stereo
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 ./test_ge_3ds_frontend_probe
