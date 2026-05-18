# Task Brief 003 — update-moss-config.sh utility script

## Context

`~/opencode/git/Moss-zero28/configs/moss-tina.config` is a complete frozen kernel
`.config` snapshot. At the start of every build, `build-inner.sh` copies it verbatim
to `/root/lichee/.config` inside the container. All config resolution happens from that
working copy — the defconfig is not used in this flow.

When the user runs `make menuconfig` (or otherwise edits `.config` inside the container),
the changes live only in `/root/lichee/.config`. The SDK directory is bind-mounted on
the host at `~/Downloads/Zero 28 Linux_SDK/Stock SDK/lichee`, so `.config` is directly
readable on the host at `$SDK/.config` — no docker/podman plumbing needed.

The SDK path is already defined in `build-env.sh`:
```bash
SDK="/home/thwonp/Downloads/Zero 28 Linux_SDK/Stock SDK/lichee"
```

## Objective

Create `~/opencode/git/Moss-zero28/update-moss-config.sh` — a host-side script that:
1. Diffs `$SDK/.config` (the live build config) against `configs/moss-tina.config`
2. Shows the full diff to the user
3. Asks for confirmation
4. Backs up the old `moss-tina.config` with a timestamp
5. Replaces `moss-tina.config` with the current `$SDK/.config`

This is a full replacement, not a selective merge. Kernel configs are monolithic —
cherry-picking individual lines risks inconsistent Kconfig state.

## Scope

One new file: `~/opencode/git/Moss-zero28/update-moss-config.sh`

### Behavior

```
Usage: ./update-moss-config.sh
```

No arguments. The script infers all paths from its own location.

- `SCRIPT_DIR` — directory containing the script (the repo root)
- `SDK` — hardcoded to match `build-env.sh`: `"/home/thwonp/Downloads/Zero 28 Linux_SDK/Stock SDK/lichee"`
- `LOCAL_CONFIG` — `$SDK/.config`
- `REPO_CONFIG` — `$SCRIPT_DIR/configs/moss-tina.config`
- `BACKUP_DIR` — `$SCRIPT_DIR/configs/backups/`

### Execution flow

1. Verify `LOCAL_CONFIG` exists (error + exit if not: "run build-inner.sh first")
2. Verify `REPO_CONFIG` exists (error + exit if not found)
3. Run `diff "$REPO_CONFIG" "$LOCAL_CONFIG"` and print the output
4. Print the count of changed lines (`diff | grep -c '^[<>]'`)
5. If zero differences: print "No differences — moss-tina.config is already up to date" and exit 0
6. Prompt: `"Apply current .config as new moss-tina.config? [y/N] "`
7. If not y/Y: print "Aborted" and exit 0
8. `mkdir -p "$BACKUP_DIR"`
9. `cp "$REPO_CONFIG" "$BACKUP_DIR/moss-tina.config.$(date +%Y%m%d-%H%M%S)"`
10. `cp "$LOCAL_CONFIG" "$REPO_CONFIG"`
11. Print the backup path and confirmation

### Style

- `#!/bin/bash`, `set -e`
- `read -rp` for the prompt
- No dependencies beyond coreutils (`diff`, `cp`, `mkdir`, `date`, `grep`)
- `chmod +x` is not the script's job — note in task brief only

## Non-goals / Later

- Do NOT selectively merge individual config lines
- Do NOT parse or validate the config content
- Do NOT handle the case where the container is running (file is directly readable via bind-mount)
- Do NOT add git operations

## Acceptance criteria

Run from `~/opencode/git/Moss-zero28/` after `make menuconfig` has modified `$SDK/.config`.
Script diffs, prompts, backs up, and updates `configs/moss-tina.config` in one invocation.
