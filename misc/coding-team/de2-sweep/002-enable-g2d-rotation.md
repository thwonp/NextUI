# Task Brief 002 — Enable CONFIG_SUNXI_G2D and fb0_buffer_num for G2D rotation

## Context

The DE2 sweep determined that the framebuffer rotation system on this build is entirely
G2D-based. All required patches are already applied in install.sh:

- `0001-feat-support-disp2-fb-hw-rotate.patch` — rewrites fb_g2d_rot.c to read degree0
  from DTS via `of_property_read_u32`, moves G2D init to `subsys_initcall`
- `0002-zero28-g2d-90-270-rot-fix.patch` (applied via sed) — fixes clip_rect axis
  swap and `&&` → `||` validation bug for 90/270° cases
- `CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT=y` — already added to defconfig
- `disp_rotation_used = <1>`, `degree0 = <1>` — already in DTS

Two pieces are missing:
1. `CONFIG_SUNXI_G2D=y` — G2D driver is not compiled in (`# CONFIG_SUNXI_G2D is not set`
   in running kernel). Without it, `g2d_open()` fails and no rotation occurs.
2. `fb0_buffer_num = <2>` — required by the rotation system for double buffering.
   The BSP readme explicitly states rotation forces double-buffer mode.

The G2D Kconfig has no dependencies beyond `ARCH_SUNXI` (satisfied on A133).
`moss-tina.config` does not mention `SUNXI_G2D`, so the defconfig setting will survive
the `set-config.sh` overlay pass.

## Objective

Add both missing pieces to `~/opencode/git/Moss-zero28/install.sh` so the next
incremental build produces a kernel with G2D enabled and a DTS with double buffering.

## Scope

One file: `~/opencode/git/Moss-zero28/install.sh`

### Change 1 — CONFIG_SUNXI_G2D in defconfig

The existing defconfig additions block (around line 193–197) currently reads:

```bash
DEF="/root/lichee/lichee/linux-4.9/arch/arm64/configs/sun50iw10p1smp_defconfig"
grep -q "SUNXI_DISP2_FB_HW_ROTATION_SUPPORT" "$DEF" || echo "CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT=y"  >> "$DEF"
grep -q "CONFIG_NLS_ISO8859_1"               "$DEF" || echo "CONFIG_NLS_ISO8859_1=y"                        >> "$DEF"
...
```

Add one line in the same style immediately after the `HW_ROTATION_SUPPORT` line:

```bash
grep -q "SUNXI_G2D"                          "$DEF" || echo "CONFIG_SUNXI_G2D=y"                            >> "$DEF"
```

### Change 2 — fb0_buffer_num in DTS

The existing DTS insertion block (around line 188–193) currently inserts:

```
disp_rotation_used       = <1>;
degree0                  = <1>;
```

Extend the inserted block to also include `fb0_buffer_num`. The sed that inserts these
on a fresh DTS (the `grep -q "degree0" || sed -i ...` line) needs updating, AND a
separate unconditional sed must handle the case where the DTS already has the block
from a prior build (same pattern as the existing degree0 unconditional sed).

Target state in the DTS:
```
disp_rotation_used       = <1>;
degree0                  = <1>;
fb0_buffer_num           = <2>;
```

For the conditional insert (fresh DTS), extend the appended block. For the unconditional
update, add a sed that inserts `fb0_buffer_num = <2>;` after `degree0` if not already
present:

```bash
grep -q "fb0_buffer_num" "$DTS" || \
    sed -i '/degree0\s*=\s*<[0-9]*>;/a \\t\t\tfb0_buffer_num           = <2>;' "$DTS"
```

Add a post-condition check in the same style as the existing degree0 check:

```bash
grep -Eq 'fb0_buffer_num[[:space:]]*=[[:space:]]*<2>;' "$DTS" || \
    { echo "ERROR: fb0_buffer_num=<2> not set in $DTS after sed"; exit 1; }
```

## Non-goals / Later

- Do NOT change degree0 (stays at <1>).
- Do NOT change the DTS dimension swap — no --clean is needed for this task.
- Do NOT rebuild. The user will run the incremental build manually.

## Constraints / Caveats

- A kernel config change (new CONFIG_SUNXI_G2D=y) will cause the build to compile
  the G2D driver objects. An incremental build handles this — no --clean required.
- The `grep -q "SUNXI_G2D"` guard will match both `CONFIG_SUNXI_G2D=y` and
  `# CONFIG_SUNXI_G2D is not set`. If the running defconfig already has the `is not set`
  line, the guard fires and the `=y` line is NOT appended. The `make oldconfig` +
  `set-config.sh` steps resolve the final config from the full Kconfig tree, so this
  is safe — the `=y` appended to the defconfig overrides the `is not set` comment
  from prior runs only on a clean defconfig. To be safe, change the guard to check
  specifically for the enabled form:
  ```bash
  grep -q "CONFIG_SUNXI_G2D=y" "$DEF" || echo "CONFIG_SUNXI_G2D=y" >> "$DEF"
  ```
- The `fb0_buffer_num` insert target (`/degree0\s*=\s*<[0-9]*>;/a`) appends after the
  degree0 line. Verify the DTS after build that `fb0_buffer_num` appears inside the
  `disp` node, not in an unrelated location.
