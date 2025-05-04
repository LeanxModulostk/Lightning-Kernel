#!/usr/bin/env bash
#
# Copyright (C) 2023 Edwiin Kusuma Jaya (ryuzenn)
#
# Simple Local Kernel Build Script
#
# Configured for Redmi Note 8 / ginkgo custom kernel source
#
# Setup build env with akhilnarang/scripts repo
#
# Use this script on root of kernel directory

SECONDS=0 # builtin bash timer
ZIPNAME="Lightning.Plus.Kernel_$(TZ=Lima/America date +"%Y%m%d-%H%M").zip"
CLANG_DIR="$HOME/tc/clang-lean"
AK3_DIR="$HOME/android/AnyKernel3"
DEFCONFIG="vendor/ginkgo-perf_defconfig"

export PATH="$CLANG_DIR/bin:$PATH"
export KBUILD_BUILD_USER="telegram"
export KBUILD_BUILD_HOST="LeanHijosdesusMadres"
export LD_LIBRARY_PATH="$CLANG_DIR/lib:$LD_LIBRARY_PATH"
export KBUILD_BUILD_VERSION="1"
export LOCALVERSION

# Comprobar si el compilador clang está presente, si no, descargar ZyC Stable
if [ ! -d "${CLANG_DIR}" ]; then
    echo "No se encontró el compilador clang... Clonando WeebX Stable desde GitHub"

    # Establecer la URL de ZyC Stable directamente
    CLANG_URL="https://github.com/XSans0/WeebX-Clang/releases/download/WeebX-Clang-19.1.5-release/WeebX-Clang-19.1.5.tar.gz"
    ARCHIVE_NAME="clang.tar.gz"
    
    # Descargar ZyC Stable Clang
    echo "Descargando WeebX Clang... Por favor espera..."
    if ! wget -P "$HOME" "$CLANG_URL" -O "$HOME/$ARCHIVE_NAME"; then
        echo "Error al descargar Clang. Abortando..."
        exit 1
    fi

    # Crear el directorio clang y extraer el archivo
    mkdir -p "${CLANG_DIR}"
    if ! tar -xvf "$HOME/$ARCHIVE_NAME" -C "${CLANG_DIR}"; then
        echo "Error al extraer Clang. Abortando..."
        exit 1
    fi

    # Eliminar el archivo descargado
    rm -f "$HOME/$ARCHIVE_NAME"

    # Verificar si el directorio clang se creó correctamente
    if [ ! -d "${CLANG_DIR}" ]; then
        echo "Error al crear el directorio 'clang'. Abortando..."
        exit 1
    fi
fi

mkdir -p out
make O=out ARCH=arm64 $DEFCONFIG

echo -e "\nStarting compilation...\n"
make -j$(nproc --all) O=out \
                      ARCH=arm64 \
                      CC=clang \
                      LD=ld.lld \
                      AR=llvm-ar \
                      AS=llvm-as \
                      NM=llvm-nm \
                      OBJCOPY=llvm-objcopy \
                      OBJDUMP=llvm-objdump \
                      STRIP=llvm-strip \
                      CROSS_COMPILE=aarch64-linux-gnu- \
                      CROSS_COMPILE_ARM32=arm-linux-gnueabi- \
                      Image.gz-dtb \
                      dtbo.img

if [ -f "out/arch/arm64/boot/Image.gz-dtb" ] && [ -f "out/arch/arm64/boot/dtbo.img" ]; then
    echo -e "\nKernel compiled successfully! Zipping up...\n"
    git restore arch/arm64/configs/vendor/ginkgo-perf_defconfig
    if [ -d "$AK3_DIR" ]; then
        cp -r $AK3_DIR AnyKernel3
    elif ! git clone -q https://github.com/LeanxModulostk/AnyKernel3; then
        echo -e "\nAnyKernel3 repo not found locally and cloning failed! Aborting..."
        exit 1
    fi
    cp out/arch/arm64/boot/Image.gz-dtb AnyKernel3
    cp out/arch/arm64/boot/dtbo.img AnyKernel3
    rm -f *zip
    cd AnyKernel3
    git checkout ginkgo &> /dev/null
    zip -r9 "../$ZIPNAME" * -x '*.git*' README.md *placeholder
    cd ..
    rm -rf AnyKernel3
    rm -rf out/arch/arm64/boot
    echo -e "LEAN Kernel"
    echo -e "Completed in $((SECONDS / 60)) minute(s) and $((SECONDS % 60)) second(s) !"
    echo "Zip: $ZIPNAME"
else
    echo -e "\nCompilation failed!"
    exit 1
fi