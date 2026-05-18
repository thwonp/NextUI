# Task Brief 001 — Set degree0=1 in install.sh for de2-sweep test

## Context

We are sweeping the DE2 rotation matrix to find a (degree0, DTS-swap) combination that
produces correct landscape orientation on the Zero28 with no LD_PRELOAD shim. The shim
has already been disabled in `skeleton/` and `build/` launch.sh (committed separately).

Cell (degree0=3, no DTS swap, shim OFF) was just tested: both SDL paths show 90° CCW
from correct. Allwinner encodes degree0 in CW degrees, so degree0=1 (90° CW) is the
logical candidate to produce correct landscape.

## Objective

Change `degree0 = <3>` to `degree0 = <1>` in `Moss-zero28/install.sh` so the next
incremental build produces a test image with degree0=1.

## Scope

One file: `~/opencode/git/Moss-zero28/install.sh`

The relevant block is around line 190–191:

```bash
# board.dts: enable 90° HW rotation → portrait 480×640, DE2 rotates 90° CCW to landscape
DTS="/root/lichee/device/config/chips/a133/configs/aw3/board.dts"
grep -q "degree0" "$DTS" || \
    sed -i '/fb0_height\s*=\s*<640>/a \\t\t\tdisp_rotation_used       = <1>;\n\t\t\tdegree0                  = <3>;' "$DTS"
```

The `grep -q "degree0" || sed -i` guard only inserts degree0 if it is absent. For
subsequent sweep iterations the DTS already contains degree0=3 from the previous build,
so the guard fires and skips the insert — the value is never updated.

**Fix both problems in one edit:**

1. Change the inserted value from `<3>` to `<1>` in the sed string.
2. Add a second `sed -i` immediately after that unconditionally updates an existing
   degree0 value, so re-running install.sh in later sweep iterations is idempotent:

```bash
# board.dts: enable HW rotation — degree0=1 → 90° CW (DE2 rotates to correct landscape)
DTS="/root/lichee/device/config/chips/a133/configs/aw3/board.dts"
grep -q "degree0" "$DTS" || \
    sed -i '/fb0_height\s*=\s*<640>/a \\t\t\tdisp_rotation_used       = <1>;\n\t\t\tdegree0                  = <1>;' "$DTS"
sed -i 's/degree0\s*=\s*<[0-9]*>;/degree0                  = <1>;/' "$DTS"
```

## Non-goals / Later

- Do NOT change the DTS dimension swap (fb0_width/fb0_height). This is a degree0-only
  change; the no-swap dimension stays as-is (480×640).
- Do NOT rebuild. The user will run the incremental build manually.
- Do NOT update devlog-de2-sweep.md — the architect will record results after
  hardware observation.

## Constraints / Caveats

- The file lives in `~/opencode/git/Moss-zero28/install.sh`, not in the NextUI repo.
- The sed replacement pattern `degree0\s*=\s*<[0-9]*>;` must match the semicolon —
  DTS property syntax requires it. Verify the pattern matches the existing line before
  committing.
- This is a sweep test change, not a final value. The comment should reflect that this
  is a test, not assert a conclusion about correct rotation direction.
