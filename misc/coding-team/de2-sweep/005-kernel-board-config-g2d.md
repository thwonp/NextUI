# Task Brief 005 — Patch board kernel config for G2D rotation

## Context

Task Brief 004 patched the architecture defconfig
(`lichee/linux-4.9/arch/arm64/configs/sun50iw10p1smp_defconfig`) to add G2D and
rotation options. This had no effect: the Tina build system does **not** use the arch
defconfig to generate the kernel `.config`. Instead, Tina runs `kconfig.pl` to merge a
list of board-specific kernel config files, the most important being:

- `LINUX_TARGET_CONFIG` = `device/config/chips/a133/configs/aw3/linux/config-4.9`

This file is what actually determines what ends up in the built kernel `.config`. It
currently contains:

```
# CONFIG_SUNXI_G2D is not set           (line 1544)
CONFIG_SUNXI_DISP2_FB_DISABLE_ROTATE=y  (line 2376)
# CONFIG_SUNXI_DISP2_FB_ROTATION_SUPPORT is not set  (line 2377)
```

The scripts/config incremental patch block added in Task Brief 004 is still correct
and should be kept: when the kernel `.config` already exists (incremental build), kconfig.pl
is not re-run, so the existing `.config` must still be patched directly.

## Objective

Fix `install.sh` so that clean builds pick up G2D options via `config-4.9` (the real
source), and update `build-inner.sh` verification to check `config-4.9` instead of the
ineffective arch defconfig.

## Scope

Two files: `~/opencode/git/Moss-zero28/install.sh` and
`~/opencode/git/Moss-zero28/build-inner.sh`

### Change 1 — Replace defconfig block in install.sh

Replace lines 199–209 (the block that patches the arch defconfig, labeled
"Kernel defconfig — G2D hardware rotation") with:

```bash
# Kernel board config — source kconfig.pl reads to generate linux-4.9/.config
# The arch defconfig (sun50iw10p1smp_defconfig) is NOT used by the Tina build.
BCONFIG="/root/lichee/device/config/chips/a133/configs/aw3/linux/config-4.9"
sed -i '/^# CONFIG_SUNXI_G2D is not set$/d'                                   "$BCONFIG"
sed -i '/^CONFIG_SUNXI_DISP2_FB_DISABLE_ROTATE=y$/d'                          "$BCONFIG"
sed -i '/^# CONFIG_SUNXI_DISP2_FB_ROTATION_SUPPORT is not set$/d'             "$BCONFIG"
grep -q "CONFIG_SUNXI_G2D=y"                           "$BCONFIG" || echo "CONFIG_SUNXI_G2D=y"                           >> "$BCONFIG"
grep -q "CONFIG_SUNXI_G2D_ROTATE=y"                    "$BCONFIG" || echo "CONFIG_SUNXI_G2D_ROTATE=y"                    >> "$BCONFIG"
grep -q "CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT=y" "$BCONFIG" || echo "CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT=y" >> "$BCONFIG"
```

Keep lines 206–209 (NLS/FAT/VIN defconfig guards) and lines 211–221 (scripts/config
incremental block) unchanged.

### Change 2 — Update build-inner.sh verification

Replace lines 68–74 (the "Kernel defconfig: HW rotation + G2D" block that checks the
arch defconfig) with:

```bash
# Kernel board config: G2D + HW rotation (device/config/chips/a133/configs/aw3/linux/config-4.9)
grep -q 'CONFIG_SUNXI_G2D=y' device/config/chips/a133/configs/aw3/linux/config-4.9 \
    && _ok "Kernel board config: CONFIG_SUNXI_G2D enabled" \
    || _fail "Kernel board config: CONFIG_SUNXI_G2D missing"
grep -q 'CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT=y' device/config/chips/a133/configs/aw3/linux/config-4.9 \
    && _ok "Kernel board config: HW rotation enabled" \
    || _fail "Kernel board config: HW rotation missing"
```

Note: build-inner.sh runs from `/root/lichee` (line 22: `cd /root/lichee`), so the
relative path `device/config/chips/a133/configs/aw3/linux/config-4.9` is correct.

## Non-goals / Later

- Do NOT modify NLS/FAT/VIN lines 206–209 (separate issue, harmless for now)
- Do NOT modify the scripts/config incremental block (lines 211–221) — keep as-is
- Do NOT investigate or fix G2D boot hang
- Do NOT change DTS, moss-tina.config, or update-moss-config.sh

## Constraints / Caveats

- `sed -i` on `config-4.9` is idempotent: if the `# CONFIG_SUNXI_G2D is not set` line
  is already absent, the sed matches nothing and exits 0
- The `grep -q ... || echo ... >> "$BCONFIG"` guards ensure options are only appended
  once, even if install.sh runs multiple times
- `SUNXI_G2D_ROTATE` depends on `SUNXI_G2D`; appending both in order is sufficient
- `HW_ROTATION_SUPPORT` depends on `DISP2_SUNXI && SUNXI_G2D`; the `kconfig.pl` merge
  followed by `oldconfig` will resolve this correctly as long as `SUNXI_G2D=y` is present
  and `DISABLE_ROTATE` is removed
- The absolute path in install.sh: `/root/lichee/device/config/chips/a133/configs/aw3/linux/config-4.9`
- The relative path in build-inner.sh (from `/root/lichee`): `device/config/chips/a133/configs/aw3/linux/config-4.9`
