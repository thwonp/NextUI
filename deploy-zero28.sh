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

echo "==> Writing version.txt..."
VERSION_HASH=$(git rev-parse --short HEAD 2>/dev/null || echo "dev")
echo "dev-$VERSION_HASH" > build/SYSTEM/version.txt

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
    adb push build/SYSTEM/version.txt /mnt/SDCARD/.system/version.txt
    # Push non-core content unconditionally
    adb push build/SYSTEM/zero28/bin/. /mnt/SDCARD/.system/zero28/bin/
    adb push build/SYSTEM/zero28/lib/. /mnt/SDCARD/.system/zero28/lib/
    adb push build/SYSTEM/zero28/paks/. /mnt/SDCARD/.system/zero28/paks/ 2>/dev/null || true
    adb push build/SYSTEM/zero28/shaders/. /mnt/SDCARD/.system/zero28/shaders/ 2>/dev/null || true
    [ -f build/SYSTEM/zero28/system.cfg ] && adb push build/SYSTEM/zero28/system.cfg /mnt/SDCARD/.system/zero28/ || true
    adb shell "mkdir -p /mnt/SDCARD/Tools/zero28"
    adb push build/EXTRAS/Tools/zero28/. /mnt/SDCARD/Tools/zero28/
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
    # Replace vendor SDL2 on rootfs with our build.
    # What:    /usr/magicx/lib/libSDL2-2.0.so.0.2600.1 is overwritten with our
    #          workspace-built binary.  The existing vendor symlink
    #          (libSDL2-2.0.so.0 -> libSDL2-2.0.so.0.2600.1) continues to resolve
    #          correctly because the patch-versioned filename is unchanged.
    # Backup:  One-shot; stored at
    #          /mnt/SDCARD/.system/zero28/lib/libSDL2-2.0.so.0.2600.1.vendor
    #          The guard checks the SD path (stable across reruns), not the rootfs
    #          path (which would be wrong after the first deploy).
    # Restore: adb shell cp /mnt/SDCARD/.system/zero28/lib/libSDL2-2.0.so.0.2600.1.vendor \
    #                       /usr/magicx/lib/libSDL2-2.0.so.0.2600.1
    # Note:    The non-adb (zip-to-SD flash) path cannot reach the device rootfs at
    #          deploy time.  If the SD card is fresh-flashed, the rootfs SDL2 remains
    #          vendor until the next run of: deploy-zero28.sh adb
    adb shell '
set -e
VENDOR_FILE=/usr/magicx/lib/libSDL2-2.0.so.0.2600.1
SD_FILE=/mnt/SDCARD/.system/zero28/lib/libSDL2-2.0.so.0.2600.1
BACKUP_FILE=/mnt/SDCARD/.system/zero28/lib/libSDL2-2.0.so.0.2600.1.vendor
if [ ! -f "$BACKUP_FILE" ]; then
    echo "==> Backing up vendor SDL2 to SD card..."
    cp "$VENDOR_FILE" "$BACKUP_FILE"
fi
echo "==> Installing workspace SDL2 to rootfs..."
df /overlay | tail -1
cp -f "$SD_FILE" "$VENDOR_FILE"
sync
'
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
