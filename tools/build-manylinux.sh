#!/usr/bin/env bash

set -euo pipefail
shopt -s nullglob

FRAMEPIPE_CONTAINER_PATH="${FRAMEPIPE_CONTAINER_PATH:-/root/framepipe}"
OUT_PATH="${OUT_PATH:-/io}"
NANOBIND_SPEC="${NANOBIND_SPEC:-nanobind==3.1.0}"
JOBS="${JOBS:-$(nproc)}"

cd "$FRAMEPIPE_CONTAINER_PATH"
[[ -f CMakeLists.txt && -f pyproject.toml && -d framepipe ]]

export CUDA_HOME="${CUDA_HOME:-/usr/local/cuda-13.0}"
export CUDAToolkit_ROOT="$CUDA_HOME"
export PATH="$CUDA_HOME/bin:$HOME/.local/bin:$PATH"
export LD_LIBRARY_PATH="$CUDA_HOME/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export CMAKE_PREFIX_PATH="$CUDA_HOME${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export CC=clang
export CXX=clang++
export CMAKE_GENERATOR=Ninja
export VCPKG_ROOT="$PWD/vcpkg"

declare -A pythons
for minor in {10..15}; do
    abi="cp3${minor}"
    py="/opt/python/$abi-$abi/bin/python"
    if [[ ! -x "$py" ]]; then
        py=$(uv python find --no-project --no-python-downloads "3.$minor")
    fi
    "$py" -I -c 'import sys, sysconfig; assert sys.implementation.name == "cpython" and sys.version_info[:2] == (3, int(sys.argv[1])) and not sysconfig.get_config_var("Py_GIL_DISABLED"), sys.version' "$minor"
    pythons[$abi]="$py"
done

rm -rf build dist wheelhouse framepipe.egg-info
rm -f framepipe/*.so framepipe/*.so.* framepipe/*.pyd framepipe/*.dll
mkdir -p dist wheelhouse

for minor in {10..15}; do
    abi="cp3${minor}"
    py="${pythons[$abi]}"
    build_dir="$PWD/build/linux-$abi"
    echo "=== Building $abi ($py) ==="

    uv pip install --python "$py" "$NANOBIND_SPEC"

    rm -f framepipe/*.so framepipe/*.so.* framepipe/*.pyd framepipe/*.dll
    rm -rf build/lib.* build/bdist.* framepipe.egg-info

    cmake --preset linux-release \
        -B "$build_dir" \
        -DPython_EXECUTABLE="$py" \
        -DVCPKG_INSTALLED_DIR="$PWD/build/vcpkg_installed" \
        -DFRAMEPIPE_GENERATE_STUBS=OFF

    cmake --build "$build_dir" -j "$JOBS"

    ext_suffix=$("$py" -I -c 'import sysconfig; print(sysconfig.get_config_var("EXT_SUFFIX"))')
    [[ -f "framepipe/_framepipe$ext_suffix" && -f framepipe/_framepipe_cuda.so ]]

    uv build --wheel --no-cache --python "$py"

    wheels=(dist/framepipe-*-$abi-$abi-*.whl)
    [[ ${#wheels[@]} -eq 1 ]] || { echo "Expected exactly one wheel for $abi" >&2; exit 1; }

    auditwheel repair "${wheels[0]}" -w wheelhouse \
        --plat manylinux_2_34_x86_64 \
        --exclude libcuda.so.1

    repaired=(wheelhouse/framepipe-*-$abi-$abi-*manylinux_2_34_x86_64*.whl)
    [[ ${#repaired[@]} -eq 1 ]] || { echo "Expected exactly one repaired wheel for $abi" >&2; exit 1; }

    "$py" -I - "${repaired[0]}" <<'PY'
import sys
import sysconfig
import tempfile
from pathlib import Path
from zipfile import ZipFile

expected = "framepipe/_framepipe" + sysconfig.get_config_var("EXT_SUFFIX")
with tempfile.TemporaryDirectory() as tmp, ZipFile(sys.argv[1]) as wheel:
    native = {n for n in wheel.namelist()
              if n.startswith("framepipe/_framepipe") and n.endswith(".so")}
    if native != {expected, "framepipe/_framepipe_cuda.so"}:
        raise RuntimeError(f"Unexpected native modules: {native}; expected {expected}")
    wheel.extractall(tmp)
    sys.path.insert(0, str(Path(tmp) / "framepipe"))
    import _framepipe
    if not hasattr(_framepipe, "Reader"):
        raise RuntimeError("Native Reader not found")
    print(f"ABI and native import OK: {sys.argv[1]}")
PY
done

mkdir -p "$OUT_PATH"
cp -v wheelhouse/*.whl "$OUT_PATH/"
