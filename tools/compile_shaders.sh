#!/usr/bin/env bash
# -----------------------------------------------------------------------------
#  Diamond Engine — compile the Vulkan shaders to SPIR-V.
#
#  Usage:  GLSLC=/path/to/glslc bash tools/compile_shaders.sh
#          bash tools/compile_shaders.sh            (searches $PATH and the NDK)
#
#  Outputs
#    app/src/main/cpp/render/shader_bin_data.h   baked blobs for the backend
#    app/src/main/assets/shaders/*.spv           copies, for tooling/inspection
#
#  The Vulkan backend is optional: when no glslc is available this script exits
#  0 with a warning and the engine builds without SPIR-V. Selecting Vulkan then
#  reports "SPIR-V shaders were not baked in" instead of crashing.
# -----------------------------------------------------------------------------
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ASSET_DIR="${ROOT}/app/src/main/assets/shaders"
HEADER_DIR="${ROOT}/app/src/main/cpp/render"

find_glslc() {
    if [ -n "${GLSLC:-}" ] && [ -x "${GLSLC}" ]; then echo "${GLSLC}"; return 0; fi
    if command -v glslc >/dev/null 2>&1; then command -v glslc; return 0; fi
    # NDK shader-tools layout (r12 … r30)
    local ndk_root="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
    if [ -z "${ndk_root}" ] && [ -n "${ANDROID_HOME:-}" ] && [ -d "${ANDROID_HOME}/ndk" ]; then
        ndk_root="$(ls -1d "${ANDROID_HOME}"/ndk/* 2>/dev/null | sort -V | tail -n1)"
    fi
    if [ -n "${ndk_root}" ]; then
        for host in linux-x86_64 darwin-x86_64 windows-x86_64; do
            local candidate="${ndk_root}/shader-tools/${host}/glslc"
            [ -x "${candidate}" ] && { echo "${candidate}"; return 0; }
            candidate="${ndk_root}/shader-tools/${host}/glslc.exe"
            [ -x "${candidate}" ] && { echo "${candidate}"; return 0; }
        done
    fi
    return 1
}

if ! GLSLC_BIN="$(find_glslc)"; then
    echo "::warning::glslc not found — building without SPIR-V (the Vulkan backend will report a clean error)"
    exit 0
fi

echo "glslc: ${GLSLC_BIN}"
mkdir -p "${ASSET_DIR}"

GLSLC="${GLSLC_BIN}" python3 "${ROOT}/tools/compile_shaders.py" \
    --glslc "${GLSLC_BIN}" --out "${HEADER_DIR}" || {
        echo "::error::shader compilation failed"
        exit 1
    }

# Keep the .spv files next to the app assets so a bug report can include them
# and so the APK inspection step in CI can assert they were packaged.
for spv in "${HEADER_DIR}"/*.spv; do
    [ -e "${spv}" ] || continue
    cp "${spv}" "${ASSET_DIR}/$(basename "${spv}")"
done
rm -f "${HEADER_DIR}"/*.spv

ls -l "${ASSET_DIR}" || true
echo "shaders: $(ls -1 "${ASSET_DIR}" | wc -l) file(s) in app/src/main/assets/shaders"
