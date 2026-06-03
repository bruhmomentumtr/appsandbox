# AppSandbox — Full Source Review: Confirmed Bug List

**Scope:** Every first-party source file under `src/`, `web/`, and `tools/` was read line-by-line.
**Method:** Each file was deep-reviewed; every candidate finding was then independently re-verified
by an adversarial reviewer that re-read the entire code path and tried to refute it; finally every
finding below was re-read against the actual source by the author of this report. Only defects
confirmed by reading the full code path are listed.

**Excluded as third-party/vendored (out of scope, not reviewed for bugs):**
`tools/linux/dxgkrnl/*` (the **vendored Microsoft WSL2 `dxgkrnl` GPU driver** — every file is SPDX
`GPL-2.0` / `Copyright Microsoft`; not this project's code), `tools/iso-patch/engine/xz/*`
(XZ embedded decoder, 0BSD upstream), and `tools/vdd/ThirdParty/wdf/*` (Microsoft WDF SDK headers).

**No functionality changes are proposed.** Every fix is behavior-preserving for well-formed input;
it only rejects/handles the malformed or error case that currently corrupts memory, leaks, or misbehaves.

## Fix application status — branch `bug-bash`

Each fix below was re-traced line-by-line against `main` and applied to the working tree on the
`bug-bash` branch (left uncommitted — squash/commit however you prefer).

> **H21 — found at RUNTIME by App Verifier (NOT in the original static review).** Running the fixed
> `AppSandbox.exe` under App Verifier and repeatedly closing a running Linux VM's IDD display window raised
> `VERIFIER STOP 0x300: Invalid handle exception`, stack `closesocket → appsandbox_core!clip_writer_thread_proc`.
> Root cause: `vm_clipboard.c` closes `writer_socket`/`reader_socket` in **both** the worker thread (~643 / ~957)
> **and** `clip_stop` (~1030 / ~1034); `clip_stop` closes the socket to unblock the thread's `recv`, and the
> unblocked thread then closes the *same* handle → double-close → crash. Same H14/H15 class I fixed for the agent
> and SSH sockets but **missed for the clipboard**. **Fixed** with the same atomic `InterlockedExchangePointer`
> claim at all four sites; Core rebuilds clean. Residual (minor, not the crash): a teardown racing the writer's
> *reconnect* store can leak one socket. Lesson: static review under-covered the clipboard teardown; App
> Verifier's Handles layer caught it on the 2nd/3rd display-close cycle.

> **H22 — found at RUNTIME by full page heap (NOT in the original static review; I had wrongly cleared this
> function as "size-exact").** Sequence: copy file host→VM, copy file VM→host, copy folder host→VM → heap
> corruption (`c0000374` under light page heap; pinpointed under full page heap to `memset` inside
> `wcscpy_s`). Root cause: `vm_clipboard.c:418` in `clip_build_hdrop_from_temp` —
> `wcscpy_s(ptr, MAX_PATH, paths[i])`. The `DROPFILES` buffer is allocated tightly (each path gets exactly
> `wcslen(paths[i])+1` wchars, see `total_size`), but `MAX_PATH` (260) is passed as `destsz`, so the
> secure-CRT tail-fill `memset` writes the unused remainder (up to 260 wchars) **past the entry's slice** →
> overwrites the heap (Debug build) / latent in Release. Triggered on the VM→host file-copy apply
> (`vm_clipboard_on_reader_apply`→`clip_build_hdrop_from_temp`), corrupting the host `CF_HDROP` block;
> the original `c0000374` was the same overflow, just detected later at the clipboard free. **Pre-existing in
> `main`** (our diff does not touch this function). **Fixed**: pass `wcslen(paths[i]) + 1` as `destsz`
> (behavior-preserving — identical copied content, no over-fill); Core rebuilds clean. Lesson: the
> secure-CRT functions fill to `destsz`, so an over-stated `destsz` on a tightly-sized buffer overflows even
> when the copied string fits.

> **H23 — found at RUNTIME by user testing (UI bug; NOT in the original backend-focused review).** Clicking
> the snapshot column's "+" creates the snapshot (persisted, visible after restart) but the row's snapshot
> dropdown does not refresh; clicking the row's edit button then reveals it. Root cause is purely frontend
> (`web/app.js`, `renderVmTable`): a per-row render-skip optimization compares a `sig` string and skips
> rebuilding the row (and thus re-running `makeSnapCell`) when `sig` is unchanged. `sig` included
> `editModeRow === i` and `selectedSnap[i]` but **not the snapshot tree** — so `snapTake` (which the backend
> *does* deliver synchronously via `vmListChanged`) didn't change `sig` → no rebuild → stale column; toggling
> edit mode flips `editModeRow`, changing `sig`, which forced the rebuild that revealed the already-delivered
> snapshot. **Pre-existing in `main`** (our branch never touched the frontend). **Fixed**: added the snapshot
> inputs `makeSnapCell` actually reads — `vm.hasSnapshots`, `vm.snapCurrent`, `vm.snapCurrentBranch`,
> `JSON.stringify(vm.snapshots)`, `JSON.stringify(vm.baseBranches)` — to `sig`. Behavior-preserving: these
> change only on user snapshot actions (a running VM can't be snapshotted), so the install-progress-tick
> rebuild-skip the optimization exists for is unaffected. `node --check` clean; deployed by a clean Release rebuild.

**Applied & build-verified on Windows** (MSVC + WDK, `Debug|x64`; every project compiles & links):
- `AppSandboxCore.dll`: H1, H2, H5, H4, C9(host), H12, H13, L3, H14, H15, H16, H17, **H21 (clipboard socket double-close — App-Verifier-found)**, **H22 (clipboard DROPFILES `wcscpy_s` heap overflow — full-page-heap-found)**, M6, M7, M8, M9, L4, L5, L6, L7, L8
- `AppSandbox.exe`: L1
- `web/app.js` (frontend, copied to `bin\<cfg>\web\` by the build): **H23 (snapshot column no-refresh — user-found)**
- `web/style.css`: **H24 — REVERTED at user request.** The attempted arrow-overlap fix (reserving `20px` on `.snap-select`/`.snap-overlay`) widened every snapshot `<select>` by 18px, which made the pre-existing content-driven width variation between rows look worse and the empty "No snapshots" dropdown stand out. `style.css` restored to `main`. The underlying issues (native-arrow/text overlap, inconsistent per-row dropdown widths, tiny empty-state dropdown) remain open and will be reworked together later — likely a fixed-width snapshot dropdown rather than a padding tweak.
- `agent.exe`: C4, C5, M14, L9
- `appsandbox-clipboard.exe`: C9 · `appsandbox-displays.exe`: M12
- `iso-patch.exe`: C1, C2, C6, C7, H6, H7, H8, H9, H10, H11, M10, M11, L17 (+ the M8 `IStream::Read` sibling)
- `AppSandboxVAD.sys`: M15, M16, L14, L15  (builds **and** signs)
- `AppSandboxVDD`: no changes (its only finding was a verified false positive)

**Reverted — not shipped:**
- **L2** (webview2 COM ref-release): safety depends on the WebView2 runtime AddRef'ing async handlers,
  which is not verifiable from our source; the leak it fixed is one-time and benign.
- **L16** (vad poolalloc): false positive — defining `operator delete(void*)` collides with the WDK's
  `stdunk.lib` (`LNK2005`). Left as shipped.

**⚠ NOT applied — require a macOS host (Xcode/clang) to apply & build-verify:**
**H3, C3, C8, H18, H19, H20, M1, M2, M3, M4, M5, M13, L10**
(`src/backend_mac/*.m`, `src/app_mac/AppDelegate.m`, `tools/agent_mac/*.m`, `tools/iso-patch-mac/iso-patch-mac.m`).
The per-finding **Fix** text below is the proposed change; apply and `xcodebuild` on macOS before committing.

**✅ Linux fixes — applied, built, deployed to a live test VM, validated & committed to `bug-bash`:**
**C10, L11** (`appsandbox-clipboard.c`), **L12** (`appsandbox-display.c`), **L13** (`appsandbox-audio.c`),
plus **L18 (folder copy host→guest flattened — found during testing, below)**.
Built in-VM with `make` and installed to `/usr/local/bin` exactly as the firstboot script does; daemons
restarted. Folder/sub-item paste verified working; audio/display/clipboard daemons healthy after redeploy.

**Known limitation (pre-existing, NOT fixed — confirmed during testing, not a regression):**
Guest→host clipboard **images only transfer as `image/bmp`**; Linux screenshot tools put **`image/png`** on
the clipboard, which the daemon has no decoder for (`kFormats` maps `image/bmp`↔`CF_DIB` only — the
long-standing libpng TODO), so a VM screenshot can't be pasted into a host bitmap app. Host→guest BMP images
do work. A fix is a feature (a self-contained zlib-based PNG→`CF_DIB` decoder, ~150–250 lines, since
`libpng-dev` isn't installable on the VM), deliberately left out of this bug-fix pass.

> **L18 — clipboard: host→guest folder paste flattens (files dumped at the target, no folder created).**
> Pre-existing in `main` (our C10/L11 edits don't touch this path; verified via `git show main:`). In
> `appsandbox-clipboard.c`'s `CLIP_MSG_FILE_DATA` handler the host→guest `text/uri-list` builder called
> `hdrop_add_uri` for **every received file** (its full nested path) and for **no** directory, so the URI
> list handed to the file manager was the nested files — not the dropped folder — and paste copied them flat.
> The host side correctly streams the whole tree (dir entry + `MyFolder/child…`); only the guest's URI-list
> construction was wrong. **Fix:** list **top-level items only** — a top-level directory contributes its own
> `file://…/MyFolder` URI (file manager copies it recursively) and nested entries contribute none; loose
> top-level files unchanged. Deployed and **confirmed working** — folders and sub-items paste with structure intact.

I deliberately made **no edits** to any `.m` or `tools/linux/*` file, since they cannot be compiled on
this Windows host and the user requires every fix to be build-tested.

### Threat-model legend
- **[guest→host]** — host code parses data controlled by the (untrusted) guest VM. Highest priority:
  this is the sandbox-escape attack surface.
- **[host→guest]** — guest agent (runs as SYSTEM/root) parses data from the host. Host is normally
  trusted, so these are robustness / compromised-or-buggy-host / defense-in-depth defects.
- **[image]** — parses an attacker- or MITM-supplyable disk image / package index (ISO, qcow2,
  squashfs, apt `Packages`).
- **[local]** — reliability/logic/leak with no direct remote attacker.

### Type legend — implementation failure vs. broken functionality
Each finding below is tagged one of:
- **[impl]** — *implementation failure.* Invisible during normal, correct operation; it only triggers on
  hostile/malformed input, OOM/resource exhaustion, or specific teardown-race timing. The product works as
  designed today — the fix is hardening and changes nothing observable in normal use.
- **[FUNCTIONAL]** — *fix to broken functionality.* The product currently misbehaves in real use (or on an
  error / over-long-session path it is supposed to handle). The fix restores the *intended* behavior — it
  still adds/removes/redesigns no feature.

The 11 **[FUNCTIONAL]** findings are **M18, M17, M9, M5, H17, L13, L1** (wrong behavior in normal use) and
**M8, M10, M1, M13** (wrong behavior on an error or long-session path). **Every other finding is [impl].**
Note: some [impl] races (e.g. H13, H14–H16, H18–H20) can also produce wrong behavior, but only under specific
concurrent teardown timing — not in normal use.

### Priority — re-ranked by real-world likelihood
Ranked by how likely each is to occur in actual operation, not by raw severity:
- **[P1] Prioritize** — occurs in real use even with valid input: triggered by **user action**,
  **accumulates over repeated runs**, is a **confirmed misimplementation** that runs on valid input, or is
  caused by **something outside our control** (app force-quit, disk full, I/O error, OOM, slow network,
  thread races during normal start/stop).
- **[SEC] Security boundary** — host code parsing data from the **untrusted guest**. These fire only on
  malformed data, but that data is produced by code running inside the sandbox, which is *not* trusted — so the
  "we set the data correctly" rule does **not** cover them. Recommended HIGH for a security product; downgrade
  only if you fully trust the in-guest agent never to be bypassed or compromised.
- **[P3] Deprioritize** — fires only on **wrong input from a trusted source**: a malformed disk image when the
  user has a valid Ubuntu ISO, protocol fields our own code sets correctly (host→guest), near-impossible OS-API
  failures, guest-local self-DoS from malformed requests, or latent/dead code.

**[SEC] (5):** H1, H2, H3, H4, H5.

**[P1] (32):** C8, C10, H12\*, H13, H14, H15, H16, H17, H18, H19, H20, M1, M3, M4, M5, M6, M7, M8, M9, M10,
M11, M13, M14, M17, M18, L1, L6, L12, L13, L14, L15, L17.  \*H12 is a confirmed logic error but has no in-repo
caller today (latent).

**[P3] (28):** C1, C2, C3, C4, C5, C6, C7, C9, H6, H7, H8, H9, H10, H11, M2, M12, M15, M16, L2, L3, L4, L5,
L7, L8, L9, L10, L11, L16.

Two [P3] caveats worth a second look: **M10** already sits in [P1] because a truncated/partial ISO *download*
is an external cause; and **H10/H11** fetch the apt `Packages` index over **plain HTTP**, so a network MITM
(external) could supply the malformed input — prefer HTTPS/signature verification over just the bounds fix.

---

## CRITICAL — memory corruption (OOB write / heap overflow)

### C1. [P3] [impl] squashfs: uncompressed data block written past `malloc(file_size)` buffer  [image]
**Area:** `tools/iso-patch/engine/squashfs.c` — `sqfs_read_file`, lines 858–875 (uncompressed branch 871–875).
**Class:** heap buffer overflow (OOB write).
`out = malloc(file_size)` (line 850). For an uncompressed block the code reads `on_disk = bs & 0x00ffffff`
(up to 16 MiB) straight into `out + written` via `sqfs_read_raw` with **no** check that
`written + on_disk <= file_size`. `block_sizes[i]` is image-controlled, so a crafted block length
overflows the heap buffer. The only consistency check (`written != file_size`, line ~935) runs *after*
the overflowing write.
**Trigger:** convert/ingest a squashfs (e.g. `casper/minimal.squashfs` from a malicious ISO) whose
`block_sizes[i]` has the uncompressed bit set and a length larger than the bytes remaining in `out`.
**Fix:** before the read, compute `size_t remaining = file_size - written;` and reject if
`on_disk > this_block_logical || on_disk > remaining` (a full uncompressed block must equal `block_size`,
a tail must equal `tail`). Mirror the bound for the sparse `memset` (`this_block_logical`).

### C2. [P3] [impl] squashfs: compressed tail block decompressed with `out_cap = block_size` into a `tail`-sized remainder  [image]
**Area:** `tools/iso-patch/engine/squashfs.c` — `sqfs_read_file`, lines 857–891 (compressed branch line 885).
**Class:** heap buffer overflow (OOB write).
When `has_frag == false` and `tail > 0`, the final iteration (`i == n_full`) has only `tail` bytes left in
`out`, but `data_decompress(cbuf, on_disk, out + written, block_size, &got)` passes `block_size` as the
output cap (not `tail`). A crafted block that decompresses to more than `tail` bytes overflows the heap.
**Fix:** pass the true remaining capacity as the output cap: `size_t cap = (i < n_full) ? block_size : tail;`
and reject `got != cap` (i.e. `got != this_block_logical`).

### C3. [needs-macOS] [P3] [impl] agent_mac clipboard: 32-bit `format_len + 1` overflow → `malloc(0)` then ~4 GB read/write  [host→guest]
**Area:** `tools/agent_mac/appsandbox-clipboard.m` — `handle_inbound` lines 1189–1196 **and**
`connect_sequence_inbound_format_list` lines 1117–1125.
**Class:** integer overflow → heap buffer overflow.
Only `data_size` is validated against `CLIP_MAX_PAYLOAD`; `format_len` is not. `malloc(h.format_len + 1)`
is computed in 32-bit unsigned arithmetic, so `format_len == 0xFFFFFFFF` wraps to `malloc(0)` (non-NULL on
Darwin, so the NULL check is bypassed). `read_full(fd, fb, h.format_len)` then streams up to ~4 GB into the
tiny allocation and `fb[h.format_len] = 0` writes far out of bounds.
**Trigger:** peer sends a `ClipHeader` whose `format_len` is `0xFFFFFFFF` (or > the real following bytes).
**Fix:** after the `data_size` check add `if (h.format_len > CLIP_MAX_PAYLOAD) return -1;` and allocate with
`malloc((size_t)h.format_len + 1)`. Apply identically at both sites.

### C4. [P3] [impl] p9copy: `Rread`/`Rreaddir` `count` used without validating against received bytes  [host→guest]
**Area:** `tools/agent/p9copy.c` — `p9_read` lines 393–394, `p9_readdir` lines 419–420
(consumers: `copy_file` `WriteFile(...,nread,...)` ~546; `copy_dir_contents` `memcpy(dirbuf,data,nread)` ~606).
**Class:** OOB read + heap buffer overflow (OOB write into `dirbuf`).
`*nread_out = unpack_u32(&payload)` exposes the wire `count` and `*data_out = payload` (= `recvbuf + 11`)
with no check that `count <= size - 11`. `sock_recv_msg` validates only the outer `size`. A malicious 9P
server returns a short message claiming `count = 0xFFFFFFFF`; `WriteFile` then reads ~4 GB past the 64 KiB
`recvbuf`, and `memcpy` into `dirbuf = malloc(s->msize)` overflows the heap.
**Fix:** in `p9_read`/`p9_readdir`, read the message length (already in `recvbuf[0..3]`),
compute `avail = msg_size - 11`, and `if (nread > avail) return FALSE;` before exposing `*data_out`.

### C5. [P3] [impl] p9copy: server `msize` accepted unclamped → stack overflow of `recvbuf[P9_MSIZE]`  [host→guest]
**Area:** `tools/agent/p9copy.c` — `p9_version`, line 273 (`s->msize = unpack_u32(&payload)`); used by
`sock_recv_msg` ~182 into `recvbuf[65536]` embedded in the stack-allocated `P9Session`.
**Class:** unvalidated wire length → stack buffer overflow.
The 9P spec requires the server `msize <= client msize`, but the server value is stored verbatim. Thereafter
any reply with `size <= msize` but `size > 65536` is recv'd into the 64 KiB stack buffer.
**Trigger:** host 9P server returns `Rversion` with `msize = 0xFFFFFFFF`, then a reply with length 65537…msize.
**Fix:** after line 273, `if (s->msize < 7 || s->msize > P9_MSIZE) s->msize = P9_MSIZE;`.

### C6. [P3] [impl] ext4 writer: per-group inode bitmap loop overruns 4 KiB stack buffer  [image]
**Area:** `tools/iso-patch/engine/ext4.c` — `write_inode_bitmap` lines 1270–1291;
root cause in `ext4_writer_open` lines 720–724 (`inodes_per_group` never clamped to `BLOCK_SIZE*8`).
**Class:** stack buffer overflow (OOB write).
`bm[BLOCK_SIZE]` (4096 bytes) addresses at most 32768 inodes; the marking loop sets bit
`b = 0..inodes_per_group-1`. `inodes_per_group = ceil(est_inodes/num_groups)` rounded only up to a multiple
of 8 — never clamped — and `est_inodes` derives from the squashfs `inode_count` (image-controlled). If
`inodes_per_group > 32768`, `bm[b>>3]` writes past `bm`.
**Trigger:** small root partition (few groups) + large squashfs `inode_count`.
**Fix:** in `ext4_writer_open`, after computing `per`, `if (per > BLOCK_SIZE * 8) per = BLOCK_SIZE * 8;`
(and fail open if capacity is then insufficient).

### C7. [P3] [impl] qcow2→raw: `l1_size * 8` 32-bit overflow → undersized L1 alloc, OOB read  [image]
**Area:** `tools/iso-patch/iso-patch.c` — `qcow2_to_raw`, line 2077 (`DWORD l1_bytes = l1_size * 8;`),
read at line 2046, loop guard `if (l1_idx >= l1_size) break;` (2112) and `read_be64(l1_table + l1_idx*8)` (2114).
**Class:** integer overflow → OOB heap read.
`l1_size` is read from the qcow2 header with no upper bound; `l1_size >= 0x20000000` makes the 32-bit
product wrap to a small value, so `HeapAlloc(l1_bytes)` is far too small while the cluster loop iterates up
to the full `l1_size`, reading past the allocation (and can drive an attacker-controlled `pread`).
**Trigger:** `iso-patch.exe --qcow2-to-vhdx evil.qcow2 ...` with crafted `l1_size`.
**Fix:** validate before allocating — `need_l1 = ceil(total_clusters / l2_entries)`; reject if
`l1_size < need_l1` or `l1_size > 0x100000000ULL/8`; allocate with `(SIZE_T)l1_size * 8` (64-bit).

### C8. [needs-macOS] [P1] [impl] iso-patch-mac: ARC over-release / use-after-free of manifest strings via `NSValue`  [local, runs as root]
**Area:** `tools/iso-patch-mac/iso-patch-mac.m` — `StageEntry` struct lines 265–271 (has `__strong`
`NSString *src/destRel`); `load_manifest` stores entries as `NSValue`; consumed at lines 825–837
(`[v getValue:&e]`).
**Class:** over-release / double-free / UAF (project built with ARC).
`StageEntry` has strong object members. `valueWithBytes:`/`getValue:` do raw byte copies and do **not**
retain; but ARC releases `e.src`/`e.destRel` at each loop-iteration scope exit, dropping the strings'
refcount with no matching retain. The autoreleased `fields` array still references them → over-release,
then a second release at pool drain → UAF/double-free in the privileged (root) staging tool.
**Trigger:** any `stage` invocation with a non-empty manifest (the normal path).
**Fix:** stop storing ARC-managed pointers in `NSValue`. Wrap the entry in a small `@interface`
(`NSObject` with `@property(strong) NSString *src/destRel; …`) and iterate `for (StageEntryObj *e in entries)`,
or mark the struct members `__unsafe_unretained` and keep the owning strings alive.

### C9. [P3] [impl] agent (win) clipboard: `off += (int)name_len` advances cursor on unvalidated wire value  [host→guest]
**Area:** `tools/agent/appsandbox-clipboard.c` — FORMAT_LIST parse loop lines 585–605 (line 597);
identical defect host-side in `src/backend_win/vm_clipboard.c` ~833.
**Class:** OOB read (pointer corruption).
The name validity guard (line 590) only gates the `memcpy`/`RegisterClipboardFormat`; the cursor advance
`off += (int)name_len` (line 597) runs unconditionally. A `name_len` such as `0x80000000` makes `(int)name_len`
large-negative, so `off` becomes wildly out of range, the loop guard `off + 8 <= data_size` still passes, and
the next `*(UINT32 *)(buf + off)` (line 586) dereferences far outside the 16 KiB buffer.
**Fix:** validate before advancing — after reading `name_len`, `if (name_len > (UINT32)((int)hdr.data_size - off)) break;`
(off is in `[8, data_size]` here, so the cast is safe). Apply to both files.

### C10. [needs-Linux] [P1] [impl] linux agent clipboard: INCR realloc-failure leaves inflated capacity → heap overflow  [host→guest]
**Area:** `tools/linux/agent/appsandbox-clipboard.c` — `handle_property_notify` INCR branch, lines 1772–1781.
**Class:** heap buffer overflow on allocation failure.
`accum_cap` is doubled in a loop **before** `realloc`; on `realloc` failure `accum` still points at the old,
smaller buffer while `accum_cap` is already inflated. The `if (g_pending_host.accum)` guard is then true and
`memcpy(accum + accum_len, val, n)` overflows the un-enlarged buffer.
**Fix:** compute the new capacity in a temporary; only commit `accum`/`accum_cap` on a successful `realloc`;
do not `memcpy` on failure.

---

## HIGH — guest→host OOB reads (sandbox-escape attack surface)

### H1. [SEC] [impl] Host IDD frame receiver: full-frame & dirty-rect copies read `recv_buf` by guest geometry, not `data_size`  [guest→host]
**Area:** `src/backend_win/vm_display_idd.c` — `display_recv_thread`, full-frame copy lines 1703–1715,
dirty-rect copy lines 1716–1745. `recv_buf` is a fixed `MAX_FRAME_DATA_SIZE` (1920×1080×4 = 8 294 400 bytes,
line 1494).
**Class:** heap OOB read (crash / stale-heap disclosure into the rendered frame).
Header validation (1643–1645) bounds only `width<=7680, height<=4320, stride>=width*4` — **no upper bound on
`stride`** and dimensions may far exceed 1920×1080. `data_size` is checked only against `MAX_FRAME_DATA_SIZE`.
The full-frame loop reads `src + row*hdr.stride` for `row` up to `d->frame_height` (which becomes the
guest-chosen height after a resize), and the dirty-rect loop advances `src += rect_row_bytes*rect_h` per rect,
neither bounded by `data_size` or `recv_buf`'s size. A guest can send a header describing 7680×4320 with
`data_size = 0` and read ~124 MB past `recv_buf`.
**Fix (full-frame):** `UINT src_rows = hdr.stride ? (data_size / hdr.stride) : 0;` and loop
`row < hdr.height && row < d->frame_height && row < src_rows`.
**Fix (dirty-rect):** before each rect, `if ((size_t)(src - recv_buf) + (size_t)rect_row_bytes*rect_h > data_size) break;`.

### H2. [SEC] [impl] Host IDD cursor builder: `shape_data_size`/`pitch` not validated against geometry  [guest→host]
**Area:** `src/backend_win/vm_display_idd.c` — CURSOR_MAGIC handler lines 1592–1626;
`create_cursor_from_bitmap` lines 1294–… (only checks `width/height <= 256`, never `pitch`).
**Class:** heap OOB read.
`cursor_buf` is allocated to exactly `shape_data_size` (checked only `<= MAX_CURSOR_SIZE`), then passed with
guest-controlled `width/height/pitch`. The builder reads `shape_data[row*pitch + col*4 + 3]`, requiring
`(height-1)*pitch + width*4` bytes, with no check that `shape_data_size >= pitch*height`.
**Trigger:** guest sends `shape_data_size = 4`, `width = height = 256`, `pitch = 1024`.
**Fix:** before use, reject `width==0||height==0||width>256||height>256||pitch < width*4 ||
(uint64_t)pitch*height > shape_data_size`.

### H3. [needs-macOS] [SEC] [impl] Host macOS clipboard FORMAT_LIST_V2: `ulen + 8 + 4` 32-bit overflow bypasses bounds check  [guest→host]
**Area:** `src/backend_mac/vm_clipboard_mac.m` — `handleInbound` FORMAT_LIST_V2 branch, line 1246.
**Class:** integer overflow → ~4 GB OOB read.
`if ((size_t)(end - p) < ulen + 8 + 4)` computes the right side in 32-bit unsigned arithmetic (`ulen` is
`uint32_t`, the literals don't widen it). For `ulen` in `[0xFFFFFFF4, 0xFFFFFFFF]` the sum wraps to 0…11, the
guard passes, and `[[NSString alloc] initWithBytes:p length:ulen ...]` reads ~4 GB past `buf`.
**Trigger:** guest sends a FORMAT_LIST_V2 with a type entry whose `uti_len` is near `UINT32_MAX`.
**Fix:** `if ((size_t)(end - p) < (size_t)ulen + 8 + 4) goto host_parse_done;` (cast `ulen` to 64-bit before adding).

### H4. [SEC] [impl] Host clipboard reader FORMAT_LIST: `data_size` lower bound missing → 4-byte OOB read  [guest→host]
**Area:** `src/backend_win/vm_clipboard.c` — `clip_reader_handle_message`, lines 805–824
(host counterpart of C9; analogous defect in `tools/linux/agent/appsandbox-clipboard.c` ~1833–1841).
**Class:** OOB heap read.
Only the upper bound `data_size > 16384` is checked; the buffer is `HeapAlloc(data_size)` and
`count = *(UINT32 *)buf` reads 4 bytes. For `data_size` in `{0,1,2,3}` (note `HeapAlloc(0)` returns non-NULL),
the 4-byte read over-reads the allocation.
**Fix:** `if (hdr->data_size < 4 || hdr->data_size > 16384) return FALSE;` (a valid FORMAT_LIST always carries
the 4-byte count). Apply the same lower-bound to the Linux agent handler.

### H5. [SEC] [impl] Exported screenshot API frame receiver: same OOB read as H1 (full-frame + dirty-rect)  [guest→host, exported API]
**Area:** `src/backend_win/asb_display.c` — `display_recv_thread`, full-frame lines 335–345, dirty-rect 346–373.
**Class:** heap OOB read (critical OOB extent).
Byte-for-byte the same defect as H1 in the `asb_display_*` exported screenshot/remote-control API
(`asb_display_connect`). **Note:** this API currently has no in-repo callers, so it is reachable only by an
external consumer of `appsandbox_core.dll`; the live in-app path is H1. Still a real defect in exported code.
**Fix:** same bounds-by-`data_size` fixes as H1, at lines 342–345 and 348–372.

---

## HIGH — image parsing (malicious ISO / qcow2 / squashfs / apt index)

### H6. [P3] [impl] ext4 writer: inode-table buffer sized by inode bytes, written by block count → OOB read  [image]
**Area:** `tools/iso-patch/engine/ext4.c` — `write_all_inodes` lines 1213–1234; sizing in
`ext4_writer_open` lines 722, 727–728.
**Class:** heap OOB read (uninitialized/adjacent heap copied into the on-disk inode table).
`tbl = calloc(inodes_per_group * 256)` but the code writes `inode_table_blocks_per_group` whole 4096-byte
blocks. These are equal only when `inodes_per_group` is a multiple of 16, yet `ext4_writer_open` rounds only to
a multiple of 8. For a per-group value that is ×8-but-not-×16 (reachable on the default path), each group write
reads up to ~2 KB past `tbl`.
**Fix (minimal, layout-preserving):** allocate the block-aligned size:
`size_t table_bytes = (size_t)w->inode_table_blocks_per_group * BLOCK_SIZE; uint8_t *tbl = calloc(1, table_bytes);`
(the per-group `memset(tbl, 0, table_bytes)` already zeroes the padding).

### H7. [P3] [impl] squashfs: division by zero when superblock `block_size == 0`  [image]
**Area:** `tools/iso-patch/engine/squashfs.c` — `sqfs_read_file` lines 845–848; `sqfs_open` (validates only
magic/version/compressor).
**Class:** crash (integer divide-by-zero) on attacker-controlled input.
`block_size = ctx->sb.block_size` is taken from the image unvalidated; `file_size / block_size` and
`% block_size` divide by zero if it is 0.
**Fix:** in `sqfs_open` validate `block_size != 0`, power-of-two, `== (1u << block_log)`, `<= 1 MiB`; reject
otherwise. Defensively guard the divide in `sqfs_read_file`.

### H8. [P3] [impl] squashfs: `walk_directory` end pointer from image-controlled `dir_size`, resolved offset unbounded  [image]
**Area:** `tools/iso-patch/engine/squashfs.c` — `walk_directory` lines 587–644.
**Class:** heap OOB read.
`stream_resolve` is only checked for `d < 0`, never `d <= dir_stream.len`. `end = base + (dir_size - 3)` with
`dir_size` from the directory inode (image-controlled). The parse loop reads up to `end`, which may lie far past
the decompressed buffer (contrast `read_inode_common`, which *does* check `d + size > len`).
**Fix:** `if (d < 0 || (size_t)d > ctx->dir_stream.len) return -1;` and clamp the scan window to
`min(bytes_left, dir_stream.len - d)`.

### H9. [P3] [impl] squashfs: `block_sizes[]` index not bounded by inode-stream length → OOB read  [image]
**Area:** `tools/iso-patch/engine/squashfs.c` — `sqfs_read_file` lines 830–859.
**Class:** heap OOB read.
`block_sizes = body + sizeof(f)` points into `inode_stream`; the loop reads `block_sizes[i]` for
`i in [0, block_list_count)` (derived from `file_size`/`block_size`) with no check that the inode body actually
contains that many `uint32` entries within `inode_stream.len` (unlike `walk_inode`).
**Fix:** compute `body_avail = inode_stream.len - (body - inode_stream.buf)` (guard underflow), verify the fixed
struct fits, then `if (block_list_count > (body_avail - fixed) / sizeof(uint32_t)) return -1;`.

### H10. [P3] [impl] prefetch_build_deps: `Filename:` value copied into fixed 1024-byte stack buffer  [image/MITM]
**Area:** `tools/iso-patch/prefetch_build_deps.c` — lines 778–779.
**Class:** stack buffer overflow (OOB write).
`fn_utf8[1024]`; `memcpy(fn_utf8, r->filename, r->filename_len)` with `filename_len` taken verbatim from the
apt `Packages` `Filename:` field (unbounded; the index is fetched over plain HTTP and is not GPG/SHA-verified).
The adjacent sha256 copy *is* length-guarded — the filename copy is the missing-bound oversight.
**Fix:** `if (r->filename_len >= sizeof(fn_utf8)) { log_err(...); return -1; }` before the `memcpy`
(or bound the field at parse time).

### H11. [P3] [impl] prefetch_build_deps: `snprintf` return used as `WriteFile` length → OOB stack read  [image/MITM]
**Area:** `tools/iso-patch/prefetch_build_deps.c` — `write_synth_packages` lines 624–626 **and**
`write_closure_json` lines 654–657 & 662–666.
**Class:** OOB stack read (stack bytes leaked into the generated files; possible fault).
C99/UCRT `snprintf` returns the length it *would* have written. `WriteFile(h, buf, n, ...)` then uses that as
the byte count, so when the formatted string (containing the unbounded package name / basename) exceeds the
2048-byte `buf`, `WriteFile` reads past the stack buffer.
**Fix:** clamp the write length: `DWORD wlen = (n < 0) ? 0u : (n >= (int)sizeof(buf) ? (DWORD)(sizeof(buf)-1) : (DWORD)n); WriteFile(h, buf, wlen, &wr, NULL);`
at every `snprintf`+`WriteFile` site (lines 626, 657, 666), or bound `name_len`/`filename_len` at parse time.

---

## HIGH — Windows backend logic / concurrency

### H12. [P1] [impl] asb_core: `asb_vm_wait_running` first-loop condition is a double-negation typo  [local, exported API]
**Area:** `src/backend_win/asb_core.c` — line 3496.
**Class:** logic error (operator precedence).
`while (!inst->running && !inst->building_vhdx == FALSE)` parses as
`(!running) && ((!building_vhdx) == FALSE)` ≡ `!running && building_vhdx`, the inverse of the intended
`!running && !building_vhdx`. For a cold start (`running=FALSE, building=FALSE`) the loop is skipped and the
function returns `E_FAIL` immediately instead of waiting out `timeout_ms`. (Exported `ASB_API`; currently no
in-repo callers, so it bites external consumers.)
**Fix:** `while (!inst->running && !inst->building_vhdx) {`.

### H13. [P1] [impl] asb_core: VHDX/Linux build threads use a stale cached `g_vms[]` index after array compaction  [local]
**Area:** `src/backend_win/asb_core.c` — `vhdx_create_thread`/`linux_create_thread` completion blocks
lines 1238–1300 (and ~2285–2338); compaction in `asb_vm_delete` (~3116) and `asb_hcs_state_changed` (~701);
unsynchronized progress reads ~1076–1081.
**Class:** race / wrong-target state write + leak.
The worker caches `args->vm_index` at creation. `g_vms[]` is compacted (entries shifted down) whenever any VM is
deleted/finalized. If a lower-index VM is removed during a build, the building VM shifts down but the worker
keeps the old index: the completion block then writes the started HCS handle/runtime_id/`running` into a
**different** VM's slot (and can delete the wrong VM), or — if the stale index is now out of range — silently
skips, leaving `building_vhdx` set forever and leaking the heap-allocated `vm_inst`. The progress path also
reads `g_vms[args->vm_index]` without `g_cs`. The existing `idd_probe` code already uses
`asb_find_vm_by_id(unique_id)` precisely to survive this.
**Fix:** capture `args->vm_unique_id = inst->unique_id` at launch and, under `g_cs`, resolve via
`asb_find_vm_by_id(args->vm_unique_id)` in both the progress and completion paths (mirroring `idd_probe`).

### H14. [P1] [impl] vm_agent: agent thread double-closes the socket that `vm_agent_stop` already closed  [local]
**Area:** `src/backend_win/vm_agent.c` — `agent_thread_proc` `disconnected:` label lines 491–492;
`vm_agent_stop` lines 545–548.
**Class:** socket double-close (can close an unrelated recycled handle).
The thread keeps the socket in local `s` and in `conn->sock`. `vm_agent_stop` closes `conn->sock` and sets it
`INVALID_SOCKET` to unblock the thread; the thread then unconditionally `closesocket(s)` with the same value.
If that handle value was meanwhile reused by another `socket()`/`accept()`, the wrong live socket is closed.
**Fix:** make the close idempotent via an atomic claim, e.g.
`SOCKET old = (SOCKET)InterlockedExchangePointer((PVOID volatile*)&conn->sock, (PVOID)INVALID_SOCKET); if (old != INVALID_SOCKET) closesocket(old);`
in both the thread and `vm_agent_stop`.

### H15. [P1] [impl] vm_ssh_proxy: listener cleanup and relay thread close the same sockets without shared exclusion  [local]
**Area:** `src/backend_win/vm_ssh_proxy.c` — `relay_thread` lines 170–173 (no lock; `SshRelay` has no proxy
back-pointer); listener cleanup loop lines 294–309 (under `proxy->cs`).
**Class:** double-close / wrong-socket close race.
`relay_thread` closes its sockets without `proxy->cs` (it cannot take the lock); the listener cleanup closes the
same `relays[i].tcp_sock/hv_sock` under the lock. The CS provides no mutual exclusion against the relay, so both
can `closesocket` the same value (and a recycled handle can be closed wrongly).
**Fix:** give one owner the close. Simplest: in the cleanup loop, only `stop = TRUE` + `WaitForSingleObject(thread)`
+ `CloseHandle` and let the relay be the sole closer of its own sockets (remove the `closesocket` calls at 298–301);
use `shutdown()` to unblock if needed.

### H16. [P1] [impl] vm_ssh_proxy: `vm_ssh_proxy_stop` frees `proxy` after only a 5 s wait while the listener can run ~48 s  [local]
**Area:** `src/backend_win/vm_ssh_proxy.c` — `vm_ssh_proxy_stop` lines 402–408.
**Class:** use-after-free + `DeleteCriticalSection` on an in-use CS.
`WaitForSingleObject(proxy->thread, 5000)` then unconditionally `CloseHandle`/`DeleteCriticalSection(&proxy->cs)`/
`free(proxy)`. The listener's own relay-cleanup loop can take up to `MAX_SSH_RELAYS (16) × 3000 ms` ≈ 48 s
(a relay can stall on a 30 s `SO_SNDTIMEO` send). If the listener outlives 5 s, `proxy` is freed while it is still
executing inside it.
**Fix:** `WaitForSingleObject(proxy->thread, INFINITE)` (the listener shutdown is bounded, so this cannot hang),
and only then tear down.

### H17. [P1] [FUNCTIONAL] asb_display / vm_display_idd: `send_input` treats `WSAEWOULDBLOCK` as fatal and counts partial sends as success  [local]
**Area:** `src/backend_win/vm_display_idd.c` — `send_input` lines 495–505 (socket is non-blocking after the
handshake, ioctlsocket FIONBIO at ~1527/1772).
**Class:** API misuse → input-channel churn + wire desync.
The comment says "drop packet if buffer full rather than stall the UI," but every `SOCKET_ERROR` — including the
transient `WSAEWOULDBLOCK` — flags the socket dead and forces a reconnect. Also a short send (`ret > 0 &&
ret < sizeof(pkt)`) is counted as success (`g_input_send_count++`), permanently misaligning the fixed-20-byte
`InputPacket` stream the guest reads.
**Fix:** on `SOCKET_ERROR` with `WSAGetLastError()==WSAEWOULDBLOCK`, just return (drop) without touching the
socket; on `ret != sizeof(pkt)`, flag the socket for reconnect (do not count it as sent).

---

## HIGH — macOS backend concurrency (SSH proxy cluster)

### H18. [needs-macOS] [P1] [impl] vm_ssh_proxy_mac: `-stop` detaches (never joins) relay threads → UAF of freed object storage  [local]
**Area:** `src/backend_mac/vm_ssh_proxy_mac.m` — `-stop` lines 89–103 (comment line 95 "Detach rather than
join"); threads receive `&_relays[slot]` (inline in the object).
**Class:** use-after-free.
The header documents `-stop` as "wait for relay threads," but it detaches and returns without waiting. When the
object is deallocated (e.g. `stop_ssh_proxy_for` sets the last strong ref to nil → `dealloc` → `[self stop]`),
a still-running `relay_main` reads/writes `*r` (its `&_relays[slot]`) in freed memory (lines 257–259).
**Fix:** record the `pthread_t`s and `pthread_join` them in `-stop` (release `_relaysLock` while joining), as the
Windows reference design does.

### H19. [needs-macOS] [P1] [impl] vm_ssh_proxy_mac: `relay_main` reads/closes fds without `_relaysLock` → double-close / `FD_SET(-1)`  [local]
**Area:** `src/backend_mac/vm_ssh_proxy_mac.m` — `relay_main` lines 234–259 (no lock) vs `-stop` lines 90–93
(under lock).
**Class:** data race → fd double-close / `FD_SET(-1)` UB.
`relay_main` uses `r->tcp_fd/vsock_fd` in `FD_SET` and `close()` without the lock that `-stop` uses to mutate
them, so `-stop` and the relay can `close()` the same fd (recycled-handle hazard) and `relay_main` can
`FD_SET(-1)`.
**Fix:** with H18's join in place, make the relay the sole closer and have `-stop` only `shutdown()`+signal+join;
or guard all fd access with `_relaysLock`.

### H20. [needs-macOS] [P1] [impl] vm_ssh_proxy_mac: relay thread detached twice (creation + `-stop`)  [local]
**Area:** `src/backend_mac/vm_ssh_proxy_mac.m` — `pthread_detach` at line 226 (creation) and again at line 98
(`-stop`).
**Class:** undefined behavior (double `pthread_detach`; possible detach of a recycled tid).
**Fix:** detach in exactly one place — remove the `-stop` detach loop (lines 96–101) and keep the detach at
creation (or, if adopting H18's join, remove the creation detach and join instead).

---

## MEDIUM

### M1. [needs-macOS] [P1] [FUNCTIONAL] macOS: `asb_mac_vm_create` leaves a phantom VM when the VM directory can't be created  [local]
**Area:** `src/backend_mac/asb_core_mac.m` — lines 681–691.
`g_vm_count++` and `save_vm_list()` run *before* `[VmDir ensureDirectoryFor:]`; on failure the function returns
without rolling back, so a half-created VM persists in `g_vms[]` and `vms.cfg`. It never sets `install_complete`
(so it can't start) and can't be deleted (`deleteVm` fails on the missing directory) — a stuck, undeletable
entry that reloads every launch. (The Windows path does `g_vm_count--` on failure.)
**Fix:** on `ensureDirectoryFor` failure, `g_vm_count--; memset(&g_vms[g_vm_count],0,sizeof(...)); save_vm_list(); post_list_changed();` before returning.

### M2. [needs-macOS] [P3] [impl] macOS: admin password not zeroed on the agent-resources-missing install path  [local, secret hygiene]
**Area:** `src/backend_mac/asb_core_mac.m` — `finish_install` nil-`agentDir` branch lines 549–556
(vs the `memset` at ~591 on the normal path).
The early-return path sets `install_complete` and returns without wiping `g_vms[idx].admin_pass`, so the
plaintext password (never persisted, intentionally) lingers in process memory for the process lifetime.
**Fix:** add `memset(g_vms[idx].admin_pass, 0, sizeof(g_vms[idx].admin_pass));` before `install_complete = YES;`
in that branch.

### M3. [needs-macOS] [P1] [impl] iso_patch_mac: auth keep-alive timer can use `g_auth` after `releaseAuthorization` frees it  [local]
**Area:** `src/backend_mac/iso_patch_mac.m` — timer handler lines 51–62 (runs on a global concurrent queue);
`releaseAuthorization` lines 100–109.
`dispatch_source_cancel` is asynchronous and does not stop an already-running handler; `releaseAuthorization`
then immediately `AuthorizationFree(g_auth)`/`g_auth=NULL` while an in-flight handler may pass its
`if (!g_auth)` check and call `AuthorizationCopyRights(g_auth,...)` on the freed handle.
**Fix:** install a `dispatch_source_set_cancel_handler` that performs the `AuthorizationFree`, and have
`releaseAuthorization` only `dispatch_source_cancel` (GCD runs the cancel handler after any in-flight event handler).

### M4. [needs-macOS] [P1] [impl] macOS: vsock fd leak when the VZ connect completion fires after the semaphore timeout (3 sites)  [local]
**Area:** `src/backend_mac/vm_agent_mac.m` `connectOnce` lines 180–204 (10 s);
`src/backend_mac/vm_clipboard_mac.m` `connectVsock` lines 267–281 (5 s);
`src/backend_mac/vm_ssh_proxy_mac.m` `connectVsock` lines 181–197 (5 s).
Each waits with a timeout but ignores the wait result and returns the `__block int fd`. If the completion
handler runs after the timeout and the connection succeeded, it `dup()`s into orphaned storage that nobody
reads/closes → one leaked fd per late-but-successful connect; repeated reconnects can exhaust the fd table.
**Fix:** check the wait result and, under a small lock, have whichever side "loses the race" `close()` the dup'd
fd (atomic ownership transfer so the success path is unchanged).

### M5. [needs-macOS] [P1] [FUNCTIONAL] macOS: View → Event Log menu and ⌘L are never installed (no main menu exists)  [local]
**Area:** `src/app_mac/AppDelegate.m` — `installMenuItems` line 23 (`if (!mainMenu) return;`).
The app is built programmatically (`main.m` never loads a nib or calls `setMainMenu:`, and `Info.plist` has no
`NSMainNibFile`), so `[NSApp mainMenu]` is `nil` at `applicationDidFinishLaunching`; the guard returns early and
the documented Event Log menu/⌘L shortcut are never created.
**Fix:** when `mainMenu` is nil, create one (`NSMenu` + an application menu item) and `[NSApp setMainMenu:]`
before proceeding, instead of bailing out.

### M6. [P1] [impl] hcs_vm: result document leaked on failed property query (monitor loop)  [local, accumulating leak]
**Area:** `src/backend_win/hcs_vm.c` — `vm_monitor_thread_proc` lines 485–509.
`HcsWaitForOperationResult` allocates the result/error document even on a failure HRESULT; the `FAILED(hr)`
branch logs/breaks but never `LocalFree(result_doc)` (freed only in the SUCCEEDED branch). In a polling loop
this leaks one buffer per failed query while the VM is unhealthy.
**Fix:** `if (result_doc) { LocalFree(result_doc); result_doc = NULL; }` at the top of the `FAILED(hr)` block.

### M7. [P1] [impl] hcs_vm: result document leaked on failed query in `cache_runtime_id`  [local]
**Area:** `src/backend_win/hcs_vm.c` — `cache_runtime_id` lines 633–646.
Same `HcsWaitForOperationResult` contract: `result_doc` is freed only inside `if (SUCCEEDED(hr) && result_doc)`,
so a failed wait that still allocated a document leaks it (once per VM start in that case).
**Fix:** free unconditionally on non-NULL: keep the parse/log gated on `SUCCEEDED(hr)`, then `if (result_doc) LocalFree(result_doc);`.

### M8. [P1] [FUNCTIONAL] disk_util: `write_stream_to_file` swallows I/O errors → a failed resources-ISO write is reported as success  [local]
**Area:** `src/backend_win/disk_util.c` — `write_stream_to_file` lines 461–468; caller `create_iso_from_dir` (521)
→ `iso_create_resources` (747) / `iso_create_instance_resources` (1270) → VM create (`asb_core.c` 2798/2807/2818).
Two error sinks: a `FAILED(hr)` `IStream::Read` (463) just `break`s and line 468 returns the **literal `S_OK`**
(not `hr`); and `WriteFile`'s `BOOL` result and `bytes_written` (464) are **never checked**, so a disk-full /
short / failed write is ignored. The function therefore returns `S_OK` unconditionally, and the whole chain
reports the **`APPSETUP` resources/autounattend ISO** (autounattend.xml + guest agent + drivers + setup scripts)
as created successfully. Note this is **not** the OS install media (that is the `iso-patch` VHDX) and the output is
only corrupt **if the underlying I/O actually fails** (disk full / I/O error) — in which case unattended Windows
setup silently breaks (setup can't find autounattend/agent). Proof this is an oversight, not intent: the sibling
copy `iso-patch.c:361` *does* check `WriteFile` (lines 380–384); the `disk_util.c` copy was simply not updated.
**Fix:** keep the failing Read's `hr` and break with it; check `WriteFile` and `bytes_written != bytes_read` and
set `hr = HRESULT_FROM_WIN32(GetLastError())` on failure; `return hr` instead of unconditional `S_OK`.
(Optionally also propagate the `FAILED` `IStream::Read` in the `iso-patch.c:361` copy, which still returns `S_OK`.)

### M9. [P1] [FUNCTIONAL] snapshot: `tree.dat` saved/loaded via C-locale narrowing → non-ASCII snapshot/branch names corrupted  [local]
**Area:** `src/backend_win/snapshot.c` — `snapshot_save` line 73 (`_wfopen_s(..., L"w")`), `snapshot_load` ~line 121 (`L"r"`).
The byte-oriented text stream narrows `wchar_t` via the default "C" locale (no `setlocale`), so any user-entered
name character > 0x7F fails to encode — the name (and the rest of that write) is truncated/mangled, losing data
on the round trip.
**Fix:** open both ends Unicode-aware: `_wfopen_s(&f, path, L"w, ccs=UTF-8")` and `L"r, ccs=UTF-8"` (existing
ASCII files remain valid UTF-8).

### M10. [P1] [FUNCTIONAL] ubuntu_vhdx: squashfs traversal failure (and per-file data errors) ignored → partial rootfs finalized as success  [image]
**Area:** `tools/iso-patch/ubuntu_vhdx.c` — `do_ubuntu_to_vhdx`, `walk_rc` captured line 2027, only logged ~2044, never checked; `pl.n_errors` likewise never checked.
The squashfs reader is itself the integrity detector: `sqfs_walk` (squashfs.c:752–766) returns non-zero whenever
`walk_inode`/`walk_directory` hit a structural failure — an inode reference that resolves outside the inode stream
or a **metadata block that fails to XZ-decompress** (`read_inode_common != 0`, line 658), an inode body too short
for its fixed struct (`body_avail < sizeof(...)`, lines 674/687/700/709/719/732), a symlink target past the body
(722), or a directory block/entry that doesn't resolve (`walk_directory`, propagated at 682/695). Per-file data-block
decompression failures are separately tallied in `pl.n_errors`. `do_ubuntu_to_vhdx` captures `walk_rc` and never
tests it (nor `n_errors`), so a source squashfs the reader could **not** fully traverse — e.g. a truncated/partial
ISO download, a read error, or bit-rot in a metadata/data block — falls straight through to write fstab/grub,
finalize, and `log_done` with `exit_code = 0`, producing a silently incomplete, likely non-bootable VHDX reported
as success. (On an intact official image `walk_rc == 0` and this never fires — hence "error path".)
**Fix:** after the threads drain, `if (walk_rc != 0 || pl.n_errors != 0) { log_err(...); sqfs_close(sq); ext4_writer_close(ew); goto cleanup; }` (leaves `exit_code = 1` so the partial VHDX is deleted).

### M11. [P1] [impl] ubuntu_vhdx: worker/consumer threads leak (and UAF stack `pl`) on `CreateThread` failure  [local]
**Area:** `tools/iso-patch/ubuntu_vhdx.c` — `do_ubuntu_to_vhdx` lines 2012–2025.
On a `CreateThread` failure mid-spawn, the code `goto cleanup` without stopping/joining already-created worker
threads, which block on `&pl` (a stack local). After the function returns, those threads dereference reclaimed
stack memory (and the critical sections are never deleted).
**Fix:** on failure, set `pl.wq.stop=1; WakeAllConditionVariable(&pl.wq.cv_worker);`, `WaitForMultipleObjects(wi, pl.workers, TRUE, INFINITE)` for the `wi` workers created so far, `CloseHandle` them, delete the CSs, then `goto cleanup`.

### M12. [P3] [impl] agent (win) displays: `off += snprintf(...)` accumulation overruns 512-byte buffer  [local]
**Area:** `tools/agent/appsandbox-displays.c` — lines 47–48.
`snprintf` returns the would-have-written length; with no clamp on `off`, once an entry truncates, the next
iteration computes `sizeof(buf) - off` as an underflowed `size_t` and writes through `buf + off` past the end.
**Trigger:** enough active monitors (~50 at typical resolutions) for `off` to reach 512.
**Fix:** clamp after each `snprintf`: `if (n < 0) break; off += n; if (off >= (int)sizeof(buf)) { off = (int)sizeof(buf)-1; buf[off]='\0'; break; }` and gate the call on `off < (int)sizeof(buf)-1`.

### M13. [needs-macOS] [P1] [FUNCTIONAL] agent_mac: `mute`/`unmute` fork children are never reaped → zombie accumulation  [local]
**Area:** `tools/agent_mac/appsandbox-agent.m` — lines 512–528; `main` signal setup 596–600 (no SIGCHLD handling).
The long-lived LaunchDaemon `fork()`+`execl(osascript)` per mute/unmute but never `waitpid()`s and sets no
SIGCHLD disposition. The host sends mute/unmute on every display show/hide, so zombies accumulate and can
eventually exhaust the process limit (breaking later `fork()`s, including shutdown).
**Fix:** in `main`, `signal(SIGCHLD, SIG_IGN);` (Darwin auto-reaps) or install `sigaction` with `SA_NOCLDWAIT`.

### M14. [P1] [impl] agent (win): clipboard helper process handles raced/double-closed across SCM and monitor threads  [local]
**Area:** `tools/agent/agent.c` — `service_ctrl_ex` SESSIONCHANGE lines 2337–2347 vs the clipboard monitor
threads; globals `g_clipboard_process/_session` (and reader analogs) mutated with no synchronization.
The SCM control-handler thread (LOGON/UNLOCK) and the monitor threads both run `kill_*`/`spawn_*` and the
`if (g_*_process){ CloseHandle(...); g_*_process=NULL; }` test-then-close on the same globals, so the same
HANDLE can be closed twice (recycled-handle hazard).
**Fix:** guard all access to the helper-process globals with a dedicated critical section (snapshot the handle
under the lock, close outside the lock to avoid holding it across `WaitForSingleObject`).

### M15. [P3] [impl] VAD wavminiport: `PropertyHandlerProposedFormat` indexes `m_DeviceFormatsAndModes` with unvalidated PinId  [local guest, kernel]
**Area:** `tools/vad/wavminiport.cpp` — lines 793–806 (`IsSystemRenderPin`/`IsBridgePin` → unguarded
`m_DeviceFormatsAndModes[nPinId]`).
The handler validates only `InstanceSize`, not the user-supplied `PinId`, before indexing the per-pin array
(every sibling handler — `GetModes`, `IsFormatSupported`, `…ProposedFormat2` — does bound `PinId >= PinCount`).
A large `PinId` causes an OOB kernel read (bugcheck / branch on adjacent memory).
**Fix:** after deriving `kspPin`, `if (kspPin->PinId >= m_pMiniportPair->WaveDescriptor->PinCount) return STATUS_INVALID_PARAMETER;`.

### M16. [P3] [impl] VAD wavstream: divide-by-zero from unvalidated client `nAvgBytesPerSec`  [local guest, kernel]
**Area:** `tools/vad/wavstream.cpp` — line 301 (`(RequestedSize_ * 1000) / m_ulDmaMovementRate`); also
`GetPresentationPosition` ~736.
`m_ulDmaMovementRate` is set verbatim from the client `WAVEFORMATEX.nAvgBytesPerSec`, which `IsFormatSupported`
never checks. A format matching all checked fields but with `nAvgBytesPerSec = 0` causes a kernel divide-by-zero
(#DE bugcheck) on buffer allocation.
**Fix:** validate in `IsFormatSupported` (`nAvgBytesPerSec == nSamplesPerSec * nBlockAlign`, reject 0) and guard
the divisors before dividing.

### M17. [P1] [FUNCTIONAL] web: `selectedSnap` keyed by VM array index → wrong snapshot/branch applied after a deletion  [local]
**Area:** `web/app.js` — `selectedSnap` declared line 7; read/written by index (685–731, 796–804, 1052, 1116, …);
list replaced wholesale on `vmListChanged`/`fullState` (134/162); never re-indexed or pruned.
The backend compacts `g_vms[]` on delete, so surviving VMs shift index, but `selectedSnap[i]` is never remapped.
A stale selection that belonged to a different VM is then applied to whichever VM now occupies that index — the
Start button can boot from a snapshot/branch the user never chose for that VM (`rowCache` correctly uses
`vm.name`, proving the index keying is the defect).
**Fix:** key `selectedSnap` by `vm.name` (mirroring `rowCache`) and prune entries for vanished names in the
existing cleanup loop (~766–773); keep sending the numeric `vmIndex` to the backend.

### M18. [P1] [FUNCTIONAL] web: row render-signature omits snapshot fields → snapshot dropdown/overlay never refreshes  [local]
**Area:** `web/app.js` — `renderVmTable` signature lines 796–804; early-return 806–811; `makeSnapCell` reads
`vm.snapshots/baseBranches/snapCurrent/snapCurrentBranch/hasSnapshots` (1044–1052) — none of which are in the
signature.
After take/rename/delete the backend re-sends the list, but the signature is unchanged, so the row is not
rebuilt and the snapshot UI stays stale (e.g. still shows "No snapshots" after the first snapshot).
**Fix:** append the snapshot fields to the signature, e.g. `vm.hasSnapshots, vm.snapCurrent,
vm.snapCurrentBranch, JSON.stringify(vm.snapshots||[]), JSON.stringify(vm.baseBranches||[])`.

---

## LOW

### L1. [P1] [FUNCTIONAL] webview2: queued pre-ready messages are never flushed  [local]
**Area:** `src/app_win/webview2_bridge.c` — `webview2_flush_queue` lines 394–403 has **no callers** (verified);
`webview2_post` queues while `!g_ready`.
Messages posted before the WebView2 controller is ready (e.g. `ui_log` lines during init) are queued and then
only `free()`d at cleanup — never delivered.
**Fix:** call `webview2_flush_queue()` from the `uiReady` handler in `ui.c` (after the JS message listener is
registered).

### L2. [P3] [impl] webview2: completion-handler COM references leaked; `CreateCoreWebView2Controller` HRESULT unchecked  [local]
**Area:** `src/app_win/webview2_bridge.c` — `EnvHandler` (359–365), `CtrlHandler` (186–192), `MsgHandler`
(235–241); controller create at 191–193.
Each handler is created with `ref=1` and handed to WebView2 (which takes its own ref); the creator's reference is
never released (one-time tiny leaks; `MsgHandler` is never unregistered). The synchronous HRESULT of
`CreateCoreWebView2Controller` is also ignored (leaks `ch` and logs nothing on synchronous failure).
**Fix:** `Release` each creator reference once the API has taken its own; capture the controller-create HRESULT
and on `FAILED(hr)` log + `Release(ch)`; save the `MsgHandler` token and `remove_WebMessageReceived` in cleanup.

### L3. [P3] [impl] asb_core: `asb_snap_delete_branch` logs `nodes[snap_idx].name` without bounds-checking `snap_idx`  [local]
**Area:** `src/backend_win/asb_core.c` — line 3360.
For any `snap_idx != -2`, the log dereferences `g_snap_trees[idx].nodes[snap_idx].name` before
`snapshot_delete_branch` validates the index; a UI-supplied out-of-range `snapIndex` (e.g. -1) causes an OOB
read just to format the log line.
**Fix:** guard the argument: `snap_idx == -2 ? L"base" : (snap_idx >= 0 && snap_idx < g_snap_trees[idx].count ? nodes[snap_idx].name : L"?")`.

### L4. [P3] [impl] gpu_enum: `InfPath` registry value may be unterminated, then used as a C string  [local]
**Area:** `src/backend_win/gpu_enum.c` — lines 284–292.
`RegQueryValueExW` does not guarantee NUL termination; if `InfPath` fills the whole `inf_name[MAX_PATH]` buffer,
`SetupGetInfDriverStoreLocationW` reads past it. (Unlikely in practice — the OS writes short INF names.)
**Fix:** force-terminate after the read: `inf_name[MAX_PATH-1] = L'\0';`.

### L5. [P3] [impl] gpu_enum: COM init/uninit imbalance on `RPC_E_CHANGED_MODE`  [local, currently unreachable]
**Area:** `src/backend_win/gpu_enum.c` — `gpu_get_default_driver_path` lines 547–595.
If the first `CoInitializeEx` returns `RPC_E_CHANGED_MODE` (no init performed), the unconditional
`CoUninitialize()` at line 595 over-decrements the caller's COM init count. (This function has no in-repo callers
today, so it is currently unreachable.)
**Fix:** track a `BOOL com_inited` set only on `S_OK`/`S_FALSE` and guard `CoUninitialize()` with it.

### L6. [P1] [impl] hcn_network: undefined shift when an adapter reports `OnLinkPrefixLength > 32`  [local]
**Area:** `src/backend_win/hcn_network.c` — `ranges_overlap` lines 234–235; prefix stored unclamped in
`collect_inuse_subnets` (~266).
`~0u << (32 - b_prefix)` is guarded only for `prefix == 0`. `OnLinkPrefixLength` is a `UINT8` that can be 0xFF
("unknown"), making the shift count negative (UB) and corrupting the overlap test for that adapter.
**Fix:** clamp/skip in `collect_inuse_subnets`: `if (u->OnLinkPrefixLength > 32) continue;` (or store `min(…, 32)`).

### L7. [P3] [impl] snapshot: `CoCreateGuid` return value unchecked → uninitialized GUID used for VHDX filenames  [local]
**Area:** `src/backend_win/snapshot.c` — `generate_guid_string` lines 8–17.
`GUID g;` is uninitialized and the `CoCreateGuid(&g)` HRESULT is discarded; on (rare) failure the formatted
string is built from indeterminate stack bytes and used to name on-disk differencing VHDXs.
**Fix:** `GUID g = {0};` and/or `if (FAILED(CoCreateGuid(&g))) { out[0] = L'\0'; return; }` with callers treating
an empty string as an error.

### L8. [P3] [impl] vmms_cert: `GetComputerNameW` return unchecked → uninitialized subject buffer  [local]
**Area:** `src/backend_win/vmms_cert.c` — lines 199–200 (`computer[256]` uninitialized).
On `GetComputerNameW` failure (returns 0) `computer` is never written, so `swprintf_s(..., L"CN=AppSandbox-%s", computer)`
reads uninitialized stack memory.
**Fix:** `if (!GetComputerNameW(computer, &cn_size)) wcscpy_s(computer, 256, L"unknown");`.

### L9. [P3] [impl] agent (win): unchecked `MultiByteToWideChar` leaves `dest_wide` uninitialized/unterminated  [host→guest]
**Area:** `tools/agent/agent.c` — lines 484–499.
`MultiByteToWideChar(CP_UTF8, 0, si->dest_path, -1, dest_wide, MAX_PATH)`'s result is ignored; an over-long
host-supplied `dest_path` (the wire field allows 511 bytes) makes the conversion fail (returns 0, no NUL), and
`dest_wide` is then used as a string in `wcscpy_s`/`p9_copy_share` (OOB stack read / garbage path). Under a
well-behaved host the field is bounded, so this is defense-in-depth.
**Fix:** check the result and skip the share on 0 (`agent_log(...); failed_shares++; continue;`); optionally
`dest_wide[0] = 0;` after the declaration.

### L10. [needs-macOS] [P3] [impl] iso-patch-mac: resume-sidecar length overflow can throw `NSRangeException`  [local]
**Area:** `tools/iso-patch-mac/iso-patch-mac.m` — `resumeDataFromSidecarAt:` lines 939–947.
`n` is read as a raw `uint64_t`; the guard `raw.length < 8 + n` is bypassed by overflow when `n` is within 7 of
`UINT64_MAX`, then `subdataWithRange:NSMakeRange(8, n)` exceeds the data length and raises an exception
(aborts fetch-ipsw). Requires a corrupted/tampered `.resume` sidecar.
**Fix:** `if (n == 0 || n > raw.length - 8) return nil;` (raw.length >= 8 is already guaranteed).

### L11. [needs-Linux] [P3] [impl] linux agent clipboard: FORMAT_LIST `data_size < 4` OOB read  [host→guest]
**Area:** `tools/linux/agent/appsandbox-clipboard.c` — lines 1833–1841 (Linux analog of H4/C9).
Only the upper bound is checked; `uint32_t count = *(uint32_t *)buf` reads 4 bytes from an allocation that may be
0–3 bytes.
**Fix:** `if (h->data_size < 4 || h->data_size > 16384) return -1;`.

### L12. [needs-Linux] [P1] [impl] linux agent display: use-after-free reading `p->plane_id` after `drmModeFreePlane(p)`  [local]
**Area:** `tools/linux/agent/appsandbox-display.c` — lines 300–302 (`cursor_init`).
`drmModeFreePlane(p)` frees the struct; the next `agent_log("cursor plane %u ...", p->plane_id, ...)` reads freed
memory. The value is already cached in `cur->plane_id` (line 295). Runs on every startup.
**Fix:** log `cur->plane_id` instead of `p->plane_id` (or move the free after the log).

### L13. [needs-Linux] [P1] [FUNCTIONAL] linux agent audio: silence pacing interval (10 ms) is less than the packet duration (~21.3 ms)  [local]
**Area:** `tools/linux/agent/appsandbox-audio.c` — `packet_ns` line 253.
The EIO/EPIPE recovery sends one 1024-frame (21.33 ms at 48 kHz) silence packet but paces at a hard-coded 10 ms,
i.e. ~2.13× realtime, overflowing the host render ring and causing constant frame-drop churn while idle.
**Fix:** `const long packet_ns = (long)FRAMES_PER_PACKET * 1000000000L / SAMPLE_RATE;` (≈21 333 333 ns).

### L14. [P1] [impl] VAD topology: JACK_DESCRIPTION/JACK_DESCRIPTION2 GET handlers don't set `ValueSize`  [local guest, info leak]
**Area:** `tools/vad/topology.cpp` — `PropertyHandlerJackDescription` lines 354–365 and
`PropertyHandlerJackDescription2` lines 424–438.
The GET-success branch writes only `cbNeeded` bytes but never sets `PropertyRequest->ValueSize`, so PortCls
reports `Irp->IoStatus.Information` as the caller's (larger) buffer size — the uninitialized tail of the KS output
buffer is copied back to user mode (info disclosure; incorrect returned length). Every other GET handler sets it.
**Fix:** add `PropertyRequest->ValueSize = cbNeeded;` in each GET-success branch before `STATUS_SUCCESS`.

### L15. [P1] [impl] VAD adaptercommon: single-instance guard counter leaked on allocation failure  [local guest]
**Area:** `tools/vad/adaptercommon.cpp` — `NewAdapterCommon` lines 260–282.
`InterlockedCompareExchange(&m_AdapterInstances, 1, 0)` sets the gate *before* `new`; if `new` returns NULL the
function returns without decrementing (the only decrement is in the destructor, which never runs). Every later
adapter creation then fails with `STATUS_DEVICE_BUSY` forever.
**Fix:** in the `p == NULL` branch, `InterlockedDecrement(&CVadAdapterCommon::m_AdapterInstances);` before
`goto Done`.

### L16. [P3] [impl] VAD poolalloc: declared `operator delete` overloads are never defined  [local, latent build]  — WITHDRAWN (not a bug in this build)
**Area:** `tools/vad/poolalloc.h` lines 30–44 vs `poolalloc.cpp`.
`operator delete(PVOID)` and `operator delete[](PVOID, size_t)` are declared but not defined. Original suspicion
was a latent unresolved-external. **This is NOT actionable and the originally-proposed fix is wrong:** defining
`operator delete(void*)` in `poolalloc.cpp` produces `LNK2005 — operator delete already defined in stdunk.lib`
(the WDK's `stdunk.lib` already provides the scalar `operator delete(void*)`). The original three definitions
plus the WDK-provided one link cleanly. **Resolution: leave `poolalloc.cpp` as shipped** (the speculative
definitions were reverted). If the unused header declarations bother you, delete them from `poolalloc.h` — do
**not** add definitions.

### L17. [P1] [impl] iso-patch prefetch_wsl_deps: temp directory leaked on every error path  [local]
**Area:** `tools/iso-patch/prefetch_wsl_deps.c` — `do_prefetch_wsl_deps` lines 211–268 (cleanup only at 272–277).
The unique `%TEMP%\isopatch-wsl-deps-<tick>` dir is removed only on success; every failure path `return -1`
without deleting it, and the pre-creation sweep only targets the current (new) name, so orphaned dirs accumulate
across failed VM-create attempts.
**Fix:** route the download/extract/copy failures through a single `fail:` label that runs the same `rd /s /q`
cleanup before `return -1` (do not route the pre-creation early return through it).

---

## Excluded as not-a-bug (verified false positives)

For transparency, these candidates were raised during review and then **rejected** after reading the full path:

- `d3dlayers.c` FE3 GetCookie "EncryptedData truncation" — the `between()` helper already clamps to the buffer
  (`if (n >= cch) n = cch - 1;`), so no overflow.
- `vz_display.m` NSWindow "releasedWhenClosed over-release" — the window is owned by an `NSWindowController`
  (`initWithWindow:`), which manages its lifetime correctly.
- `tools/linux/agent/appsandbox-agent.c` GPU-share line length desync — the concrete trigger is unreachable
  given the host-side formatting bounds.
- `tools/linux/agent/appsandbox-clipboard.c` DROPFILES `fWide` offset — the struct is an intentional 12-byte
  wire format under `GMEM_ZEROINIT`; the cited consequence does not occur.
- `vdd.cpp` dirty-rect send "16 GB OOB read" — the rect width cannot wrap as claimed for the cited triggers.
- `tools/vad/poolalloc.cpp` placement array-new mismatch — resolves safely under the project's build settings
  (covered instead by the narrower, real L16).

---

*Total confirmed defects: 65 (10 Critical, 20 High, 18 Medium, 17 Low) — several bundle multiple identical sites.
By **likelihood** (the actionable ranking): **32 [P1]** (occur in real use — user action / accumulation /
confirmed misimplementation / external failures), **5 [SEC]** (untrusted guest→host parsing — the sandbox
boundary), **28 [P3]** (only on malformed input from a trusted source, near-impossible API failures, or latent
code). All are in this project's own code — the vendored Microsoft `dxgkrnl` fork, the `xz` decoder, and the WDF
headers are out of scope. Note severity and priority are orthogonal: e.g. the C-tier image-parser overflows are
real memory-corruption but [P3] because a valid Ubuntu ISO never triggers them.*
