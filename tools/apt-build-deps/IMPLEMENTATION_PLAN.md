# Plan: build agent ELFs on the VM at first boot

Source of truth lives in this file so future-Claude (or future-you)
can pick up where we left off without re-deriving the design.

## Architecture summary (current)

```
┌─ HOST (Windows) ─────────────────────────────────┐
│                                                  │
│  AppSandbox.exe (linux_create_thread)            │
│    1. detect_iso_kernel(iso) → (codename, kver)  │
│    2. ensure_apt_build_deps_cached(codename, kver)│
│       cache hit?  → skip                          │
│       cache miss? → spawn iso-patch.exe          │
│                     --prefetch-build-deps         │
│                     (C, no PowerShell)            │
│    3. generate_vhdx_manifest_ubuntu              │
│       stages cached dev .debs into VHDX           │
│    4. iso-patch.exe --ubuntu-to-vhdx --stage     │
│                                                  │
│  C:\ProgramData\AppSandbox\apt-build-deps\       │
│    <codename>\<kver>\                             │
│      ├── Packages    (synthetic, closure only)   │
│      ├── .closure.json                            │
│      └── *.deb       (~10-15 files, ~10 MB)      │
│                                                  │
└──────────────────────────────────────────────────┘
                       │
                       ▼ (VHDX content)
┌─ GUEST (Linux) ──────────────────────────────────┐
│                                                  │
│  /opt/appsandbox/                                │
│    local-apt/        ← ISO's pool/main + dists/  │
│    local-apt-extras/ ← prefetched dev headers    │
│    (NO agent-src; NO dxgkrnl-src; NO asb_drm-src │
│     — fetched at first boot from GitHub)          │
│                                                  │
│  firstboot.sh (poc-firstboot.service):           │
│    STEP 1-7   grub-install, user, OOBE, gdm      │
│    STEP 7.4   write apt sources, apt update      │
│    STEP 7.5   curl tarball from GitHub:           │
│               github.com/jamesstringer90/         │
│               appsandbox @ wip-linux-vm-support   │
│               → /opt/appsandbox/src/tools/linux/  │
│    STEP 8     apt install dev headers + make +   │
│               make install (agent ELFs)           │
│    STEP 9-11  systemd units, modules-load.d      │
│    STEP 12    apt install dkms+headers + dkms    │
│               build/install asb_drm + dxgkrnl     │
│    STEP 13-16 wsl-mesa, wsl-deps, services,      │
│               sleep masks, modprobe              │
│    STEP 17    verification block (OK/MISSING)    │
│    STEP 99    reboot                              │
│                                                  │
└──────────────────────────────────────────────────┘
```

## What's where, source-of-truth

| Artifact | Source | Lives in VHDX as | Built/installed where |
|---|---|---|---|
| Agent C source | repo `tools/linux/agent/*.c` + `Makefile` | NOT in VHDX | fetched STEP 7.5, built STEP 8 |
| asb_drm source | repo `tools/linux/asb_drm/` | NOT in VHDX | fetched STEP 7.5, DKMS-built STEP 12 |
| dxgkrnl source | repo `tools/linux/dxgkrnl/src/` | NOT in VHDX | fetched STEP 7.5, DKMS-built STEP 12 |
| Dev .deb closure | NuGet-style prefetched from archive.ubuntu.com | `/opt/appsandbox/local-apt-extras/` | apt installed STEP 8 + 12 |
| ISO's pool/main + dists/ | Ubuntu ISO at build time | `/opt/appsandbox/local-apt/` | apt source for build tools |
| systemd units, configs | repo `tools/linux/{agent,asb_drm}/` | `/opt/appsandbox/systemd/` (still host-staged) | STEP 9-10 install |
| wsl-mesa | repo `tools/linux/wsl-mesa/prebuilt/` | `/opt/appsandbox/wsl-mesa.tar.zst` | STEP 13 extract |
| wsl-deps | Microsoft NuGet at install-time | `/opt/appsandbox/wsl-deps/` | STEP 14 ldconfig |

systemd units could ALSO move to GitHub-fetch in a future cleanup
(they're text files in the same `tools/linux/` tree). Leaving them
host-staged for now to keep the firstboot dependency on internet
optional for the install/enable steps.

---


## Goal

Stop shipping the gitignored prebuilt agent ELFs
(`release/resources/linux/appsandbox-{agent,audio,clipboard,display,input}`).
All agent code comes from committed C source at `tools/linux/agent/`,
built on the guest VM at first boot from a host-cached set of dev-package
`.deb` files. Same flow for the asb_drm / dxgkrnl kernel modules
(already DKMS-built; no prebuilt fast path either).

## Constraints

- **No backwards compatibility** with prebuilt-ELF VMs — this feature isn't
  shipped, no Linux VMs in the wild. Single code path everywhere; no
  "if prebuilts present, use them, else build from source" branching.
- **Offline at first boot** — guest must not need archive.ubuntu.com.
  All `.deb`s + sources staged into the VHDX at iso-patch time.
- **Internet on the HOST is fine** — that's where the prefetch happens.
- **One kernel per ISO** — Ubuntu desktop ISO ships exactly one kernel
  (e.g. `7.0.0-14-generic` on 26.04 daily). No multi-kernel iteration
  needed in DKMS step.

## Source-build closure

From `tools/linux/agent/Makefile` + `#include` grep of the 5 .c files:

### Pulled in by `build-essential` (already installed by DKMS step)
- `gcc`, `make`, `libc6-dev` (with `linux-libc-dev` for `linux/uinput.h`,
  `linux/vm_sockets.h`, `linux/input-event-codes.h`, `drm_fourcc.h`),
  `dpkg-dev`

### NEW dev packages we must prefetch (5 seed packages)
- `libasound2-dev` — `alsa/asoundlib.h` for `appsandbox-audio`
- `libxcb1-dev` — `xcb/xcb.h` for `appsandbox-clipboard`
- `libxcb-xfixes0-dev` — `xcb/xfixes.h` for `appsandbox-clipboard`
- `libdrm-dev` — `xf86drm.h`, `xf86drmMode.h` for `appsandbox-display`
- `pkg-config` (or `pkgconf`) — Makefile uses `pkg-config --cflags libdrm`

### Runtime libs (already in `minimal.squashfs`, no .deb needed)
- `libasound2t64`, `libxcb1`, `libxcb-xfixes0`, `libdrm2`

Walking `Depends:` + `Pre-Depends:` from the 5 seed packages typically
yields ~10-15 .debs total, ~5-15 MB. `--include-recommends` flag
on the fetch script as escape hatch if a `Recommends:` is needed.

## Host-side prefetch design

### Cache layout

Mirrors `tools/wsl-deps/` pattern. Keyed by Ubuntu release codename +
kernel version (because `linux-headers-X.Y.Z-generic` is kernel-locked).

```
C:\ProgramData\AppSandbox\apt-build-deps\
└── <codename>\                          e.g. "resolute"
    ├── <kver>\                          e.g. "7.0.0-14-generic"
    │   ├── Packages                     synthetic, generated by the fetch
    │   │                                script — only entries for the
    │   │                                closure, so apt update is fast.
    │   ├── .closure.json                manifest of what we fetched
    │   │                                (for diagnostics / verification).
    │   └── *.deb                        the closure, ~10-15 files
    └── <other-kver>\                    future kernel ABI = separate cache
```

### `tools/apt-build-deps/fetch-build-deps.ps1`

PowerShell, no external dependencies (Win10+ built-ins only):
- `Invoke-WebRequest` for HTTP downloads
- `[System.IO.Compression.GZipStream]` for gunzip
- Pure PowerShell text parsing for `Packages` (no external apt tools)

Arguments:
- `-CodeName <name>` (e.g. `resolute`)
- `-KernelVersion <X.Y.Z-generic>` (used to also fetch
  `linux-headers-<X.Y.Z-generic>` defensively in case ISO pool/main
  doesn't have it on every release)
- `-OutDir <path>`
- `-Mirror <url>` (default `http://archive.ubuntu.com/ubuntu`)
- `-IncludeRecommends` (default false; flip on if a build fails for
  a missing Recommends dep)

Algorithm:
1. `Invoke-WebRequest "$Mirror/dists/$CodeName/main/binary-amd64/Packages.gz"`
   → `$env:TEMP\Packages.gz`
2. Gunzip via `[System.IO.Compression.GZipStream]` → `$env:TEMP\Packages`
3. Parse to `@{}` hashtable: name → record with fields
   `Filename`, `Size`, `SHA256`, `Version`, `Depends`, `Pre-Depends`
   (and optionally `Recommends` if `-IncludeRecommends`)
4. BFS from seed set (5 packages + `linux-headers-<KernelVersion>`).
   Resolve disjunction (`a | b`) by taking the first available alternative.
5. For each package in closure: `Invoke-WebRequest "$Mirror/$Filename"` →
   `$OutDir.tmp/<basename>`. SHA256 verify against the record.
6. Write synthetic `Packages` file at `$OutDir.tmp/Packages` containing
   ONLY the closure entries (verbatim from the parsed metadata, with
   `Filename:` rewritten to the local basename).
7. Write `.closure.json` for diagnostics.
8. Atomic-rename `$OutDir.tmp` → `$OutDir` (no partial caches on failure).

### Failure handling

If host has no internet OR the rename fails: log to stderr, exit nonzero.
Caller (AppSandbox) logs the warning and proceeds anyway. firstboot's
STEP 8b `make` will fail → STEP 17 verification flags missing ELFs
→ user sees clear `MISSING /usr/local/bin/appsandbox-agent` in com1
output. No silent broken VMs.

## VM-create-time integration (AppSandbox host side)

### 1. ISO version detection — `detect_iso_kernel()` in asb_core.c

~50 LOC. Inputs: `iso_path`. Outputs: `release_codename`, `kver`.

- Mount ISO via VirtDisk (re-use existing patterns)
- Read the only subdir under `<iso>:\dists\` → `release_codename`
- `FindFirstFile` `<iso>:\pool\main\l\linux\linux-image-*-generic_*.deb` →
  parse `linux-image-(.*?)-generic` from filename → `kver`
- Dismount

Runs once at the start of `linux_create_thread`. ~1 second.

### 2. Cache check + fetch — `ensure_apt_build_deps_cached()`

```c
wchar_t cache_dir[MAX_PATH];
swprintf(cache_dir, MAX_PATH,
    L"%s\\AppSandbox\\apt-build-deps\\%s\\%s",
    programdata, release_codename, kver);

if (cache_is_valid(cache_dir)) {
    /* hit — skip fetch */
} else {
    /* miss — spawn PowerShell fetch, block until done */
    spawn_fetch(release_codename, kver, cache_dir);
}
```

`cache_is_valid()`:
- `cache_dir` exists
- `cache_dir\Packages` exists, > 0 bytes
- `cache_dir\.closure.json` parses, lists ≥ 6 .debs (sanity floor)
- All .debs listed in closure.json exist on disk

`spawn_fetch()`:
- `CreateProcessW` `powershell.exe -ExecutionPolicy Bypass -File
  <exe-dir>\..\tools\apt-build-deps\fetch-build-deps.ps1
  -CodeName <rel> -KernelVersion <kver> -OutDir <cache_dir>`
- Wait. Log progress lines to asb_log. Return exit code.

### 3. Stage cached .debs + agent source into the manifest

Extend `stage_linux_agent_and_extras` in `disk_util.c`:

- Resolve `<programdata>\AppSandbox\apt-build-deps\<release>\<kver>\`
- Copy `*.deb` + `Packages` → `staging\extras\local-apt-extras\`
- Copy `<repo>\tools\linux\agent\{*.c, Makefile}` → `staging\extras\agent-src\`

REMOVE from `stage_linux_agent_and_extras`:
- Section that copies prebuilt agent ELFs from `<res_dir>\linux\appsandbox-*`
- Section that copies `<res_dir>\linux\modules\` prebuilt .ko trees

Keep:
- DKMS source trees (`tools/linux/{asb_drm, dxgkrnl}/`) → `extras/{asb_drm,dxgkrnl}-src/`
- systemd units (`tools/linux/agent/systemd/*.service`)
- modules-load.d / modprobe.d configs
- wsl-mesa tarball
- wsl-deps .so libs (still prefetched from NuGet via separate path)

## Guest-side firstboot.sh changes

### STEP 8 (replace existing prebuilt install)

```bash
# --- STEP 8: build agent binaries from source ---
echo "==== STEP 8: build agent binaries ===="
BUILD_T0=$SECONDS
EXTRAS=/opt/appsandbox
if [ -d "$EXTRAS/agent-src" ]; then
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        libasound2-dev libxcb1-dev libxcb-xfixes0-dev \
        libdrm-dev pkg-config 2>&1 | tail -5 \
      && echo "OK: apt install agent build deps" \
      || echo "FAIL: apt install agent build deps"
    cd "$EXTRAS/agent-src"
    make -j$(nproc) 2>&1 | tail -10 \
      && echo "OK: make" || echo "FAIL: make"
    make install PREFIX=/usr/local 2>&1 | tail -3 \
      && echo "OK: make install" || echo "FAIL: make install"
else
    echo "FAIL: $EXTRAS/agent-src missing — agents won't be built"
fi
echo "==== STEP 8 finished in $((SECONDS - BUILD_T0)) s ===="
```

### STEP 10.5 (already exists; extends for local-apt-extras)

Two file:// apt sources:
```
/etc/apt/sources.list.d/appsandbox-local.list:
    deb [trusted=yes] file:/opt/appsandbox/local-apt resolute main

/etc/apt/sources.list.d/appsandbox-local-extras.list:
    deb [trusted=yes] file:/opt/appsandbox/local-apt-extras ./
```

One `apt-get update` after writing both. Use
`Dir::Etc::sourceparts=/etc/apt/sources.list.d/` to scan both.

### STEP 11 — DELETED entirely

No prebuilt-kernel-modules fast path. DKMS is the only path.

### STEP 12 — simplify

Drop the `need_dkms` check. Always:
```bash
echo "==== STEP 12: build kernel modules ===="
DKMS_T0=$SECONDS
DEBIAN_FRONTEND=noninteractive apt-get install -y \
    dkms build-essential "linux-headers-$TGT_KVER" 2>&1 | tail -10
for mod in asb_drm dxgkrnl; do
    SRC="$EXTRAS/$mod-src"
    [ -d "$SRC" ] || { echo "SKIP $mod: no $SRC"; continue; }
    ver=$(awk -F= '/^PACKAGE_VERSION=/{gsub(/"/,"",$2); print $2}' "$SRC/dkms.conf")
    [ -z "$ver" ] && ver=1.0.0
    rm -rf "/usr/src/$mod-$ver"
    cp -r "$SRC" "/usr/src/$mod-$ver"
    dkms add -m $mod -v $ver 2>&1 | tail -2 || true
    if dkms build -m $mod -v $ver -k "$TGT_KVER" 2>&1 | tail -5; then
        echo "OK: dkms build $mod"
    else
        echo "FAIL: dkms build $mod — dumping make.log:"
        for log in $(find /var/lib/dkms -name make.log 2>/dev/null); do
            echo "---- $log (tail 40) ----"; tail -40 "$log"
        done
    fi
    dkms install -m $mod -v $ver -k "$TGT_KVER" 2>&1 | tail -3
done
depmod -a "$TGT_KVER"
echo "==== STEP 12 finished in $((SECONDS - DKMS_T0)) s ===="
```

### STEP 17 (verification) — keep

Already checks `/usr/local/bin/appsandbox-*` + .ko in all possible
locations. No change needed.

## Implementation order

1. **`detect_iso_kernel()` in asb_core.c** — pure detection, no state
   change. Log what it finds. Smoke-test by creating a VM and grepping
   the AppSandbox log.
2. **`fetch-build-deps.ps1`** — write + test standalone first. Run
   manually:
   ```powershell
   .\tools\apt-build-deps\fetch-build-deps.ps1 `
       -CodeName resolute -KernelVersion 7.0.0-14-generic `
       -OutDir $env:TEMP\fbd-test
   ```
   Verify cache dir is populated with Packages + .debs.
3. **`ensure_apt_build_deps_cached()` + spawn wiring** in asb_core.c.
4. **Extend `stage_linux_agent_and_extras`** — add agent-src + cached
   .debs sections; remove prebuilt-ELF + prebuilt-.ko sections.
5. **Update firstboot script in ubuntu_vhdx.c**:
   - Replace STEP 8 with the source-build STEP 8b
   - Delete STEP 11 + `SKIP_PREBUILT_KO` toggle
   - Simplify STEP 12 (drop `need_dkms` check)
   - Add `/etc/apt/sources.list.d/appsandbox-local-extras.list` in STEP 10.5
6. **Build, smoke test** — create VM, watch com1 for STEP 8 +
   STEP 12 timings, verify STEP 17 shows all 5 binaries + both .ko.
7. **Housekeeping** — `AppSandbox.vcxproj` PostBuildEvent that copies
   gitignored Linux ELFs can be deleted; `release/resources/linux/`
   entries in `.gitignore` can be removed.

## Sizing

| Component | LOC | Disk delta | First-boot time delta |
|---|---|---|---|
| `detect_iso_kernel()` | ~50 | — | +1s per VM-create |
| `ensure_apt_build_deps_cached()` | ~80 | — | +30s on cache miss, 0 on hit |
| `fetch-build-deps.ps1` | ~150 | host cache ~10 MB / kernel | — |
| `stage_linux_agent_and_extras` edits | ~40 | +10 MB VHDX | — |
| firstboot STEP 8b | ~30 | — | +30-60s per VM |
| firstboot STEP 10.5 extension + STEP 11 deletion + STEP 12 simplification | ~-20 net | — | — |
| **Totals** | ~330 LOC | +10 MB VHDX | +30-60s firstboot, +30s on first-ever VM build |

## Risks / open questions

- **Ubuntu archive URL stability** — `http://archive.ubuntu.com/ubuntu/` is
  the canonical mirror, stable for >20 years. HTTPS available via
  `https://us.archive.ubuntu.com/ubuntu/` if we prefer.
- **Closure resolution misses an indirect Recommends** — mitigation:
  re-run prefetch with `-IncludeRecommends`. Doc this in the README.
- **SHA256 mismatch on download** — Packages.gz changed between fetch
  and verify (Ubuntu mirror sync race). Retry once; if still mismatch,
  log + exit nonzero. apt-secure trusts our locally-shipped Packages
  file because we mark the source `[trusted=yes]`.
- **First VM after Ubuntu point release** — old cache dir matches old
  codename. New codename = new cache. Both can coexist. No cleanup
  needed unless host disk pressure (caches are ~10 MB each).
- **Headers package not in ISO pool** — defensive: fetch script also
  pulls `linux-headers-<KernelVersion>` from archive. We don't rely
  on the ISO having it (it does today; might not on every release).

## References

- `tools/wsl-deps/README.md` — the model we're copying (host-side
  prefetch from a Microsoft NuGet feed; same pattern, different
  upstream).
- `f361def src/backend_win/disk_util.c:2326-2479` — the OLD subiquity
  flow's autoinstall.late-commands that handled this during install
  phase instead of first-boot. Reference for what setup was needed.
- `tools/linux/agent/Makefile` — single source of truth for the build
  recipe; mirror what `make install` does in STEP 8b.
- `tools/linux/agent/*.c` — `#include`s analyzed above; that's the
  source closure.
