# framepipe — build guide for manylinux wheel (Python 3.14, CUDA 13)

> target: `framepipe-*-cp314-cp314-manylinux_2_34_x86_64.whl`.
> container: `quay.io/pypa/manylinux_2_34_x86_64` (AlmaLinux 9).
> **Imortant:** well, im not sure this will work for you, make sure you know what ure doing

## At host

```bash
# run it from project root
docker run -it --name framepipe-build \
  -v "$PWD:/io" \
  quay.io/pypa/manylinux_2_34_x86_64 bash

docker start -ai framepipe-build

# u can reenter containter with this
# docker start -ai framepipe-build
```

## In container's shell, setup tools and CUDA

```bash
set -e

dnf -y update
dnf -y install \
  git curl wget ca-certificates tar gzip unzip zip which \
  clang clang++ make cmake ninja-build pkgconf-pkg-config \
  autoconf automake libtool bison flex patch diffutils \
  perl nasm yasm python3-pip \
  libdrm-devel libva-devel rsync

curl -fsSL \
  https://developer.download.nvidia.com/compute/cuda/repos/rhel9/x86_64/cuda-rhel9.repo \
  -o /etc/yum.repos.d/cuda-rhel9.repo

curl -LsSf https://astral.sh/uv/install.sh | sh

dnf clean all
dnf makecache

dnf -y install cuda-toolkit-13-0

/usr/local/cuda-13.0/bin/nvcc --version
```

## Env variables

```bash
export CUDA_HOME=/usr/local/cuda-13.0
export CUDAToolkit_ROOT="$CUDA_HOME"
export PATH="$CUDA_HOME/bin:/opt/python/cp314-cp314/bin:$PATH"
export LD_LIBRARY_PATH="$CUDA_HOME/lib64:${LD_LIBRARY_PATH:-}"
export CC=clang
export CXX=clang++
export CMAKE_GENERATOR=Ninja
export VCPKG_ROOT=~/framepipe/vcpkg
export CMAKE_PREFIX_PATH="$CUDA_HOME:${CMAKE_PREFIX_PATH:-}"

python --version
nvcc --version
```
## Sync the src folder, bootstrap vcpkg and python deps

```bash
# src sync via /io:
mkdir -p ~/framepipe
rsync -av --exclude={'.venv/','test_data/','vcpkg/','other/','profiling/','build/','.cache/','.pytest_cache/'} /io/path/to/framepipe ~/
cd ~/framepipe

# vcpkg
git submodule update --init --recursive
sh vcpkg/bootstrap-vcpkg.sh -disableMetrics

# python 3.14
export PATH="/opt/python/cp314-cp314/bin:$PATH"
uv sync --group dev --python 3.14

```

## .whl build

```bash
cd ~/framepipe
export PATH="/opt/python/cp314-cp314/bin:$PATH"

cmake --preset linux-release
cmake --build --preset linux-release

uv build --python 3.14

# check that wheel actually builded
ls -lh dist/*.whl
```

## auditwheel repair -> manylinux wheel

```bash
cd ~/framepipe
mkdir -p wheelhouse

auditwheel repair \
  --plat manylinux_2_34_x86_64 \
  --exclude libcuda.so.1 \
  -w wheelhouse dist/*.whl

ls -lh wheelhouse/*.whl
cp -v wheelhouse/*.whl /io/
```

## Check result manylinux-wheel

```bash
mkdir -p /tmp/framepipe-check
cd /tmp/framepipe-check
python -m zipfile -e ~/framepipe/wheelhouse/framepipe-*.whl .

readelf -d framepipe/_framepipe.cpython-314-*.so | grep NEEDED
readelf -d framepipe/_framepipe_cuda.so | grep NEEDED
readelf -d framepipe/_framepipe_cuda.so | grep -E 'RPATH|RUNPATH'
nm -D --undefined-only framepipe/_framepipe.cpython-314-*.so \
  | grep -E 'cuCtx|cuInit|cudaMemcpy|CudaProcessor|CudaToCpuTransfer' || true
```

Success if: no `libcuda.so.1` in `_framepipe`, but in `_framepipe_cuda.so`; `libcudart` in `framepipe.libs`, and rpath of cuda-module have `$ORIGIN/../framepipe.libs`
