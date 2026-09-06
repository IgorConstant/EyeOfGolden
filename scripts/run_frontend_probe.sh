#!/usr/bin/env bash

# Runs a scripted frontend (menu) traversal in an isolated Azahar instance and
# collects the per-frame timing result plus framebuffer captures.
#
#   scripts/run_frontend_probe.sh [probe.cfg]
#
# Environment:
#   GE_FRONTEND_PROBE_SKIP_BUILD=1   reuse the staged executable
#   GE_FRONTEND_PROBE_TIMEOUT_SECONDS  default 240
#   GE_FRONTEND_PROBE_LABEL          result/capture directory label

set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
azahar_bin=${AZAHAR_BIN:-"${HOME}/Applications/Azahar.app/Contents/MacOS/azahar"}
if [[ ! -x "${azahar_bin}" && -x /Applications/Azahar.app/Contents/MacOS/azahar ]]; then
    azahar_bin=/Applications/Azahar.app/Contents/MacOS/azahar
fi
azahar_app=$(dirname "$(dirname "$(dirname "${azahar_bin}")")")
azahar_sd=${AZAHAR_SD:-"${HOME}/Library/Application Support/Azahar/sdmc"}
probe_source_config=${1:-"${repo_dir}/scripts/frontend-probes/solo-dam-start.cfg"}
probe_timeout_seconds=${GE_FRONTEND_PROBE_TIMEOUT_SECONDS:-240}
label=${GE_FRONTEND_PROBE_LABEL:-$(basename "${probe_source_config}" .cfg)}
stage_dir="${repo_dir}/build/3ds-sd/3ds/goldeneye-3ds"
install_dir="${azahar_sd}/3ds/goldeneye-3ds"
probe_config="${install_dir}/frontend-probe.cfg"
probe_result="${install_dir}/frontend-probe.result"
gameplay_result="${install_dir}/dam-input-probe.result"
capture_dir="${install_dir}/captures"
out_dir="${repo_dir}/build/frontend-probe/${label}"

if [[ ! -x "${azahar_bin}" ]]; then
    echo "Azahar executable not found: ${azahar_bin}" >&2
    exit 2
fi
if pgrep -f '/Azahar.app/Contents/MacOS/azahar' >/dev/null; then
    echo "Azahar is already running; close it before an isolated probe." >&2
    exit 2
fi
if [[ ! -f "${probe_source_config}" ]]; then
    echo "Frontend probe script not found: ${probe_source_config}" >&2
    exit 2
fi

cd "${repo_dir}"
if [[ "${GE_FRONTEND_PROBE_SKIP_BUILD:-0}" != 1 ]]; then
    ./scripts/build_3ds.sh
    cp platform/3ds/goldeneye-3ds.3dsx "${stage_dir}/goldeneye-3ds.3dsx"
fi
if [[ ! -f "${stage_dir}/goldeneye-3ds.3dsx" \
      || ! -f "${stage_dir}/goldeneye.u.gepack" ]]; then
    echo "staged 3DS executable/assets are missing" >&2
    exit 2
fi
mkdir -p "${install_dir}" "${out_dir}"
cp "${stage_dir}/goldeneye-3ds.3dsx" "${install_dir}/"
if ! cmp -s "${stage_dir}/goldeneye.u.gepack" "${install_dir}/goldeneye.u.gepack"; then
    cp "${stage_dir}/goldeneye.u.gepack" "${install_dir}/"
fi
# Other diagnostics would bypass the frontend or select a stage.
for stale in dam-input-probe.cfg stage.cfg frontend-visual-probe.cfg \
        probe-detail.cfg probe-retraces.cfg world-profile.cfg; do
    rm -f "${install_dir}/${stale}"
done
rm -f "${install_dir}"/*-visual-tour.geview
gameplay_frames=$(awk '$1=="gameplay_frames"{print $2}' "${probe_source_config}" || true)
cp "${probe_source_config}" "${probe_config}"
rm -rf "${capture_dir}"
rm -f "${probe_result}" "${gameplay_result}"

cleanup() {
    if [[ -s "${probe_result}" ]]; then
        cp "${probe_result}" "${out_dir}/frontend-probe.result"
    fi
    if [[ -s "${gameplay_result}" ]]; then
        cp "${gameplay_result}" "${out_dir}/gameplay.result"
    fi
    if [[ -d "${capture_dir}" ]]; then
        mkdir -p "${out_dir}/captures"
        cp "${capture_dir}"/*.ppm "${out_dir}/captures/" 2>/dev/null || true
    fi
    rm -f "${probe_config}"
    if [[ -n "${azahar_pid:-}" ]] && kill -0 "${azahar_pid}" 2>/dev/null; then
        kill -TERM "${azahar_pid}" 2>/dev/null || true
        for _quit_attempt in {1..20}; do
            kill -0 "${azahar_pid}" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "${azahar_pid}" 2>/dev/null; then
            kill -KILL "${azahar_pid}" 2>/dev/null || true
        fi
        wait "${azahar_pid}" 2>/dev/null || true
    fi
    if [[ -n "${open_pid:-}" ]] && kill -0 "${open_pid}" 2>/dev/null; then
        kill -TERM "${open_pid}" 2>/dev/null || true
        wait "${open_pid}" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

open -W -n "${azahar_app}" --args -w "${install_dir}/goldeneye-3ds.3dsx" &
open_pid=$!
azahar_pid=""
for _attempt in {1..50}; do
    azahar_pid=$(pgrep -f '/Azahar.app/Contents/MacOS/azahar' | head -n 1 || true)
    [[ -n "${azahar_pid}" ]] && break
    sleep 0.1
done
if [[ -z "${azahar_pid}" ]]; then
    echo "could not identify the isolated Azahar frontend-probe process" >&2
    exit 1
fi
echo "Frontend probe ${label} is running from ${probe_source_config}."
deadline=$((SECONDS + probe_timeout_seconds))
# Wait for the frontend result, then (if requested) the gameplay result.
while [[ ! -s "${probe_result}" ]] && kill -0 "${open_pid}" 2>/dev/null \
      && (( SECONDS < deadline )); do
    sleep 0.25
done
if [[ -n "${gameplay_frames}" && "${gameplay_frames}" != 0 ]]; then
    while [[ ! -s "${gameplay_result}" ]] && kill -0 "${open_pid}" 2>/dev/null \
          && (( SECONDS < deadline )); do
        sleep 0.25
    done
fi
# Captures scheduled on the last frames may still be pending; give the
# running frontend/gameplay a moment before the emulator is stopped.
sleep 1
if [[ ! -s "${probe_result}" ]]; then
    echo "Frontend probe did not produce a result." >&2
    exit 1
fi
cp "${probe_result}" "${out_dir}/frontend-probe.result"
echo "Frontend probe result: ${out_dir}/frontend-probe.result"
python3 "${repo_dir}/scripts/analyze_3ds_frontend_probe.py" \
    "${out_dir}/frontend-probe.result"
grep -qx 'status=complete' "${out_dir}/frontend-probe.result"
