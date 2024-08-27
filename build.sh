#!/bin/bash

# HOME path
export HOME=/home/ark

# Compiler environment
export CLANG_PATH=$HOME/proton-clang/bin
export PATH="$CLANG_PATH:$PATH"
export CROSS_COMPILE=aarch64-linux-gnu-
export CROSS_COMPILE_ARM32=arm-linux-gnueabi-
export KBUILD_BUILD_USER=Ark
export KBUILD_BUILD_HOST=Ark

echo
echo "Setting defconfig"
echo

make CC=clang LD=ld.lld AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip ark_defconfig

echo
echo "Compiling kernel"
echo

make CC=clang LD=ld.lld AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip -j$(nproc --all) || exit 1
