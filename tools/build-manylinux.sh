#!/usr/bin/env bash

# u can use this script if u've built manylinux-whl once, and now u want rebuild it,
# replace FRAMEPIPE_PATH with actual one.
# NOTE: this file will copy repaired .whl in OUT_PATH

FRAMEPIPE_IO_PATH="/io/Projects/framepipe"
FRAMEPIPE_CONTAINER_PATH="/root/framepipe"
OUT_PATH="/io"

set -euo pipefail

rsync -av --delete \
    --exclude='.git/' \
    --exclude='build/' \
    --exclude='dist/' \
    --exclude='wheelhouse/' \
    --exclude='.venv/' \
    --exclude='test_data/' \
    --exclude='vcpkg/' \
    --exclude='other/' \
    --exclude='profiling/' \
    --exclude='.cache/' \
    --exclude='.pytest_cache/' \
    --exclude='tests/' \
     $FRAMEPIPE_IO_PATH \
    ~/

cd $FRAMEPIPE_CONTAINER_PATH

rm -rf dist wheelhouse build

cmake --preset linux-release
cmake --build --preset linux-release -j "$(nproc)"

uv build --wheel --no-cache

auditwheel repair dist/*.whl -w wheelhouse \
    --plat manylinux_2_34_x86_64 \
    --exclude libcuda.so.1

cp -v wheelhouse/*.whl $OUT_PATH
