# Task Brief 004 — Fix install.sh kernel config propagation

## Context

`install.sh` modifies the kernel defconfig to add G2D and rotation options, but the
current approach has two independent failure modes:

**Failure 1 — wrong guard on HW_ROTATION_SUPPORT (line 201):**
```bash
grep -q "SUNXI_DISP2_FB_HW_ROTATION_SUPPORT" "$DEF" || ...
```
This matches `# CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT is not set`, so the `=y`
line is never appended when the "is not set" comment is already in the defconfig.

**Failure 2 — DISABLE_ROTATE never removed:**
`CONFIG_SUNXI_DISP2_FB_DISABLE_ROTATE=y` is the default for a Kconfig choice block
(the three options are mutually exclusive). Even if `HW_ROTATION_SUPPORT=y` is
appended below it, Kconfig resolves the choice to `DISABLE_ROTATE` unless it is
explicitly removed first.

**Failure 3 — defconfig-only approach misses incremental builds:**
Incremental kernel builds reuse the existing `lichee/linux-4.9/.config` and never
read the defconfig. Defconfig changes only take effect on clean builds where the
kernel `.config` doesn't exist yet. The kernel `.config` must also be patched
directly when it exists.

**Missing option:**
`CONFIG_SUNXI_G2D_ROTATE=y` was enabled in `make kernel_menuconfig` but was never
added to install.sh.

## Objective

Fix the kernel config block in `install.sh` so that:
- Clean builds (no kernel `.config` present) produce the right kernel config via the defconfig
- Incremental builds (kernel `.config` exists) are patched directly via `scripts/config`
- Both paths enable the same set of options

## Scope

Two files: `~/opencode/git/Moss-zero28/install.sh` and
`~/opencode/git/Moss-zero28/build-inner.sh`

### Change 1 — Fix defconfig block in install.sh

Replace the existing defconfig block (around lines 199–202, the `DEF=` block):

```bash
# Kernel defconfig additions (idempotent guards)
DEF="/root/lichee/lichee/linux-4.9/arch/arm64/configs/sun50iw10p1smp_defconfig"
grep -q "SUNXI_DISP2_FB_HW_ROTATION_SUPPORT" "$DEF" || echo "CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT=y"  >> "$DEF"
grep -q "CONFIG_SUNXI_G2D=y"                "$DEF" || echo "CONFIG_SUNXI_G2D=y"                            >> "$DEF"
```

With:

```bash
# Kernel defconfig — G2D hardware rotation
# DISABLE_ROTATE is the choice block default; must be removed before adding HW_ROTATION_SUPPORT.
DEF="/root/lichee/lichee/linux-4.9/arch/arm64/configs/sun50iw10p1smp_defconfig"
sed -i '/^CONFIG_SUNXI_DISP2_FB_DISABLE_ROTATE=y$/d'                                    "$DEF"
grep -q "CONFIG_SUNXI_G2D=y"                            "$DEF" || echo "CONFIG_SUNXI_G2D=y"                            >> "$DEF"
grep -q "CONFIG_SUNXI_G2D_ROTATE=y"                     "$DEF" || echo "CONFIG_SUNXI_G2D_ROTATE=y"                     >> "$DEF"
grep -q "CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT=y"  "$DEF" || echo "CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT=y"  >> "$DEF"
```

Keep the remaining guards below (`NLS_ISO8859_1`, `NLS_UTF8`, `FAT_DEFAULT_IOCHARSET`,
`VIDEO_SUNXI_VIN`) unchanged.

### Change 2 — Add scripts/config block for incremental builds

Immediately after the defconfig block (before the `cp bootlogo.bmp` line), add:

```bash
# Kernel .config — patch directly for incremental builds (reuse existing .config, skip defconfig)
KCONFIG="/root/lichee/lichee/linux-4.9/.config"
SCRIPTS_CONFIG="/root/lichee/lichee/linux-4.9/scripts/config"
if [ -f "$KCONFIG" ] && [ -x "$SCRIPTS_CONFIG" ]; then
    "$SCRIPTS_CONFIG" --file "$KCONFIG" \
        --disable SUNXI_DISP2_FB_DISABLE_ROTATE \
        --enable  SUNXI_G2D \
        --enable  SUNXI_G2D_ROTATE \
        --enable  SUNXI_DISP2_FB_HW_ROTATION_SUPPORT
    echo "[install.sh] Patched kernel .config via scripts/config"
fi
```

Note: `scripts/config` takes symbol names WITHOUT the `CONFIG_` prefix.

### Change 3 — Update build-inner.sh verification

The existing check at line 69 only verifies `HW_ROTATION_SUPPORT` in the defconfig.
Extend it to also verify `CONFIG_SUNXI_G2D=y`:

```bash
# Kernel defconfig: HW rotation + G2D
grep -q 'SUNXI_DISP2_FB_HW_ROTATION_SUPPORT=y' lichee/linux-4.9/arch/arm64/configs/sun50iw10p1smp_defconfig \
    && _ok "Kernel defconfig: HW rotation enabled" \
    || _fail "Kernel defconfig: HW rotation missing"
grep -q 'CONFIG_SUNXI_G2D=y' lichee/linux-4.9/arch/arm64/configs/sun50iw10p1smp_defconfig \
    && _ok "Kernel defconfig: CONFIG_SUNXI_G2D enabled" \
    || _fail "Kernel defconfig: CONFIG_SUNXI_G2D missing"
```

## Non-goals / Later

- Do NOT investigate or fix the G2D boot hang — that is a separate issue
- Do NOT change DTS values (degree0, fb0_buffer_num)
- Do NOT modify moss-tina.config or update-moss-config.sh
- Do NOT change any other defconfig guards (NLS, FAT, VIN)

## Constraints / Caveats

- `scripts/config --disable X` writes `# CONFIG_X is not set` — correct form for a
  choice block deselection
- `scripts/config --enable X` writes `CONFIG_X=y` — correct for bool options
- The `if [ -f "$KCONFIG" ]` guard ensures the scripts/config block is a no-op on a
  truly clean build where the kernel `.config` doesn't yet exist; the defconfig handles
  that case
- The `sed -i` on the defconfig is idempotent: if `DISABLE_ROTATE=y` is absent (already
  removed by a prior run), the sed matches nothing and exits 0
- Do not use `scripts/config --set-val` — use `--enable` and `--disable` only
