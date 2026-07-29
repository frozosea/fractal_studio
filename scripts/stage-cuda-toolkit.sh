#!/bin/sh
set -eu

toolkit_dir="${CUDA_TOOLKIT_DIR:-/usr/local/cuda-13.0}"
stage_dir="${1:-.cuda-toolkit}"
target_dir="$toolkit_dir/targets/x86_64-linux"

if [ ! -x "$toolkit_dir/bin/nvcc" ] || [ ! -d "$target_dir/include" ]; then
    echo "CUDA Toolkit not found at $toolkit_dir" >&2
    echo "Set CUDA_TOOLKIT_DIR to a CUDA 13 installation and retry." >&2
    exit 1
fi

mkdir -p "$stage_dir/targets/x86_64-linux/lib"
cp -a "$toolkit_dir/bin" "$stage_dir/"
cp -a "$toolkit_dir/nvvm" "$stage_dir/"
cp -a "$target_dir/include" "$stage_dir/targets/x86_64-linux/"
cp -a "$target_dir/lib/stubs" "$stage_dir/targets/x86_64-linux/lib/"
cp -a "$target_dir"/lib/libcudart.so* "$stage_dir/targets/x86_64-linux/lib/"
cp -a "$target_dir/lib/libcudart_static.a" \
      "$target_dir/lib/libcudadevrt.a" \
      "$target_dir/lib/libculibos.a" \
      "$stage_dir/targets/x86_64-linux/lib/"

echo "Staged CUDA Toolkit build files in $stage_dir"
