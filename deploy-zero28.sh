#!/bin/bash
set -euo pipefail

REPO="$(cd "$(dirname "$0")" && pwd)"
TARGET="${1:-}"

if [ -z "$TARGET" ]; then
    echo "Usage: $0 /path/to/sdcard"
    echo "       $0 adb"
    exit 1
fi

if [ "$TARGET" != "adb" ] && [ ! -d "$TARGET" ]; then
    echo "Error: SD card not found at $TARGET"
    exit 1
fi

export PATH="$HOME/.local/bin:$PATH"
cd "$REPO"

echo "==> Building (Docker)..."
make build -f makefile.toolchain PLATFORM=zero28 TOOLCHAIN_PLATFORM=tg5040

echo "==> Assembling..."
make setup
make system PLATFORM=zero28

echo "==> Copying built cores..."
mkdir -p build/SYSTEM/zero28/cores
find workspace/zero28/cores/output -name "*.so" -exec cp {} build/SYSTEM/zero28/cores/ \; 2>/dev/null || true

echo "==> Stripping NEEDED entries (prevent double EGL init)..."
patchelf --remove-needed libGLESv2.so.2 build/SYSTEM/zero28/bin/nextui.elf
patchelf --remove-needed libUMP.so.3    build/SYSTEM/zero28/bin/nextui.elf

if [ "$TARGET" = "adb" ]; then
    echo "==> Deploying via ADB..."
    adb wait-for-device
    adb push build/SYSTEM/res/. /mnt/SDCARD/.system/res/
    # Push non-core content unconditionally
    adb push build/SYSTEM/zero28/bin/. /mnt/SDCARD/.system/zero28/bin/
    adb push build/SYSTEM/zero28/lib/. /mnt/SDCARD/.system/zero28/lib/
    adb push build/SYSTEM/zero28/paks/. /mnt/SDCARD/.system/zero28/paks/ 2>/dev/null || true
    adb push build/SYSTEM/zero28/shaders/. /mnt/SDCARD/.system/zero28/shaders/ 2>/dev/null || true
    [ -f build/SYSTEM/zero28/system.cfg ] && adb push build/SYSTEM/zero28/system.cfg /mnt/SDCARD/.system/zero28/ || true
    # Push cores only if not already present on device
    if compgen -G "build/SYSTEM/zero28/cores/*.so" > /dev/null 2>&1; then
        echo "==> Checking cores (skip existing)..."
        for so in build/SYSTEM/zero28/cores/*.so; do
            name=$(basename "$so")
            exists=$(adb shell "[ -f /mnt/SDCARD/.system/zero28/cores/$name ] && echo yes || echo no" 2>/dev/null | tr -d '\r\n')
            if [ "$exists" != "yes" ]; then
                echo "    deploying $name"
                adb push "$so" /mnt/SDCARD/.system/zero28/cores/
            else
                echo "    skipping $name (already on device)"
            fi
        done
    fi
    adb shell "killall nextui.elf 2>/dev/null; sleep 0.5; sync"
    echo ""
    echo "Done. NextUI will restart automatically."
else
    echo "==> Packaging..."
    rm -rf build/zero28-deploy
    mkdir -p build/zero28-deploy/.system
    cp -R build/SYSTEM/zero28 build/zero28-deploy/.system/
    cd build/zero28-deploy && zip -r "$REPO/MinUI.zip" .system
    cd "$REPO"

    echo "==> Writing SD card ($TARGET)..."
    mkdir -p "$TARGET/magicx/zero28"
    cp build/BOOT/common/zero28.sh "$TARGET/magicx/init.sh"
    chmod +x "$TARGET/magicx/init.sh"
    cp workspace/all/show2/build/zero28/show2.elf "$TARGET/magicx/zero28/"
    cp workspace/zero28/other/unzip60/unzip "$TARGET/magicx/zero28/"
    cp MinUI.zip "$TARGET/"

    echo ""
    echo "Done. Eject the SD card and boot."
fi
