# MISSION: tobyOS web browser — Stage 4: real multi-tab browsing

Give `/bin/gui_browser` genuine tabs: multiple independent pages open at
once, each with its own URL, history, scroll position, title, and
rendered content. Click a tab to switch, `+` to open, the tab's `×` to
close. The tab strip is currently **decorative** — one hardcoded cell, a
close-`×` that quits the whole app, and a `+` that does nothing. Make it
real.

Build first, read the code, and confirm every claim below against the
current tree — this brief cites `programs/user_gui_browser/main.c` at a
point in time (~2651 lines, after Stage 3). Line numbers WILL drift.

## Branch workflow (the user is strict about this)
- Create a branch **`browser-tabs` off `main`** and do all work there.
- Commit to the branch; **do NOT merge to `main` without asking.** The
  user merges each feature themselves once it is "fully complete."
- Commit trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
- `make` from `/c/CustomOS/tobyOS`; `git commit` from `/c/CustomOS`.
- Never commit `*.log`, `*.img`, or `.claude/`.

## WHAT ALREADY EXISTS (Stages 1–3, all on main — build on it)
The browser is a single-window TobyTK app (`toby/tk.h`): one full-window
`tk_canvas` with `on_paint`/`on_event`, plus an `on_key` hook. It has:
- **A proportional layout engine** (Stage 1): HTML → styled spans → blocks
  → positioned runs, painted via the kernel TrueType rasterizer. Real
  headings, bold, inline blue underlined clickable links, bullets, hr,
  pre/code, pixel scrolling, find-in-page, page title → WM title bar
  (`sys_gui_set_title`).
- **Forms** (Stage 2): `<form>`/`<input>` render as focusable boxes +
  submit buttons; GET submission (`submit_form`), `e` focuses next field.
- **Images** (Stage 3): `<img>` fetched per-URL, decoded via libtoby's
  `toby_image_load()` (stb_image), scale-blitted inline. `load_images()`
  runs after render; `g_in_image_load` guards re-entrancy.
- **Omnibox / history / TLS**: Chrome-style URL resolution, redirects,
  DDG→Mojeek search fallback, back/forward (`g_history`/`g_hist_pos`),
  kernel TLS 1.3. All verified on real HW (HP EliteDesk).

## THE CORE PROBLEM: all page state is global singletons
Everything that makes up "the current page" is one set of file-scope
globals. A tab is a *bundle* of exactly this state. The heavy hitters
(grep `^static` near these):
- Document: `g_raw[96K]`, `g_render[128K]`, `g_spans[SPAN_MAX=16384]`,
  `g_blks[4096]`, `g_runs[16384]`, `g_nspans/g_nblks/g_nruns`,
  `g_render_len/g_raw_len`.
- Images: `g_images[IMG_MAX=48]` (+ malloc'd pixel buffers), `g_nimages`.
- Forms: `g_forms[16]`, `g_fields[64]`, `g_nforms/g_nfields/g_focus_field`.
- Navigation/view: `g_url`, `g_url_len`, `g_title`, `g_links`,
  `g_link_count`, `g_history[32]`, `g_hist_pos`, `g_hist_count`,
  `g_scroll_y`, `g_doc_h`, `g_layout_w`, `g_last_status`, `g_source_view`.
- Find: `g_find_mode/g_find_buf/g_find_len/g_find_run`.
- Focus: `g_focus_url`.

**This is the whole job: make N of these coexist and switch between them.**
Two viable architectures — pick one, justify it in the commit:

**A. Full per-tab document bundle (true tabs, more memory).**
Bundle the per-page state into `struct tab` and keep an array of tabs
with an `active` index. Sizing reality: the static arrays are ~1 MB per
tab (`g_render` 128K + `g_runs` ~384K + `g_spans` ~256K + `g_raw` 96K +
…). 8 static tabs ≈ 8 MB BSS — the kernel gives procs plenty (`hb`
shows ~7.6 GiB free on the EliteDesk), but confirm the process image /
BSS limit first (check `elf.c` load + the user stack/heap map). Cleaner:
**move the big per-tab buffers to the libtoby heap** (`malloc`, already
linked — see Stage 3) so only opened tabs cost memory; `struct tab`
holds pointers, allocated on tab open, freed on close (mirror
`images_free`). Switching tabs = repoint the working set + `layout()` +
repaint. This is the "real Chrome tabs" answer; it's a wide refactor
touching every `g_*` access (render/layout/paint/events).

**B. Lightweight tabs: per-tab nav state only, re-render on switch
(less memory, simpler, some cost).**
Each tab stores just URL + history + scroll + title (small). The heavy
document globals stay singletons = the *active* document. Switching tabs
re-fetches/re-renders that tab's URL (network hit; loses in-page form
input; keeps scroll offset). Simpler diff, but a tab switch that
re-hits the network is not really Chrome-like. Acceptable as a first
cut only if you also cache the last-rendered doc for the active tab.

Recommendation: **A with heap-allocated per-tab document buffers.** It's
more work but it's the honest feature and avoids re-fetch-on-switch. If
you must ship incrementally, land the `struct tab` + tab-strip UI first
with a small fixed tab count, then move buffers to the heap.

## THE TAB STRIP (UI + interaction)
Current paint (`paint_all`, ~1946): fills `TAB_BAR_H`(22px), draws ONE
active cell at x=[0,220], `@` icon, truncated title, a red `×` at x≈200,
a `+` at x≈230. Current click (`handle_mouse_down`, ~2237): `my <
TAB_BAR_H && mx in [195,215]` → `sys_exit(0)` (kills the app!).

Rebuild it:
- Render **all** tabs across the strip: each a cell (title truncated to
  fit, its own `×`), active tab highlighted (`COL_TAB_ACTIVE`) vs
  inactive (`COL_TAB_BG`), then a `+` after the last tab. Compute cell
  width from live `g_win_w` and tab count (min/max width, ellipsize).
  Handle overflow gracefully (shrink cells, or clip — don't overrun the
  window-control buttons at the right).
- Hit-testing by x-range: click a tab body → switch active; click a
  tab's `×` → close that tab (pick a sensible neighbor as new active;
  closing the LAST tab should quit the app, matching today's `×`); click
  `+` → open a new tab on the home page, made active.
- **Do not `sys_exit(0)` on a tab close unless it's the last tab.**

## KEYBOARD (raw keys arrive in `on_key`)
Ctrl+letter arrive as 0x01–0x1A (Ctrl+F=0x06 already = find; don't
clobber it). Add: **Ctrl+T** (0x14) new tab, **Ctrl+W** (0x17) close
tab, **Ctrl+Tab**/**Ctrl+PgDn** next tab, **Ctrl+Shift+Tab**/**Ctrl+PgUp**
prev tab. Verify what the kernel actually delivers for Ctrl+Tab / Ctrl+PgUp
(PS/2 + USB HID paths — see `[[taskbar-search]]` for how arrow/nav keys
were wired as 0x80..0x85; Ctrl-modified nav keys may need a look at the
keymap). If a chord isn't deliverable, fall back to a plain key (e.g.
`t`/`w` when the URL bar isn't focused, like the existing `e`/`r`/`h`).

## PER-TAB CORRECTNESS (easy to get wrong)
- Each tab keeps its **own** back/forward history + position, scroll
  offset, title, focus (URL-bar vs field vs none), find state, loading
  state, and (arch A) rendered document + decoded image pixels.
- Switching tabs must `sys_gui_set_title` to the active tab's title and
  repaint fully.
- `g_in_image_load` blocks input during a synchronous image fetch —
  make sure a tab switch or new-tab can't corrupt an in-flight load
  (either finish/abort the load, or make it per-tab and guard).
- Free a closed tab's malloc'd image pixels + document buffers (leak =
  the EliteDesk runs for hours).
- The home/new-tab page (`set_home_page`) must populate a fresh tab.

## KEY FILES & APIS
- `programs/user_gui_browser/main.c` — everything. The refactor is
  concentrated but wide: introduce `struct tab`, route every `g_*`
  page-state access through the active tab (a `#define` shim like
  `#define g_scroll_y (cur->scroll_y)` can shrink the diff, or add a
  `static struct tab *cur;` and rename — your call, keep it readable).
- `libtoby/include/toby/tk.h` / `libtoby/src/tk.c` — `tk_draw_*`,
  `sys_gui_set_title` already used. No toolkit change should be needed;
  if you add one (e.g. a real tab widget) keep it minimal and note the
  ABI in a comment. `struct tk_window` layout changes → recompile ALL
  libtoby programs (no header dep tracking) — see `[[tobyos-build-env]]`.
- No new syscalls expected. If you add one, append after the current max
  in `include/tobyos/abi/abi.h`, bump `ABI_SYS_NR_MAX`, keep structs
  reserved-padded, and **`make clean`** (struct/ABI changes need it).

## BUILD & TEST
Build (from `/c/CustomOS/tobyOS`, MSYS bash):
```
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
make "CC=TMP='C:\t' clang" "HOST_CC=TMP='C:\t' gcc" \
     EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT" iso
```
QEMU screenshot harness: `-DTKAPP_BOOT -DTKAPP_BROWSER` boots straight
into the browser; drive it with the scratchpad `browser_drive.py` /
QMP `send-key` + `screendump` pattern from prior stages. **GOTCHAS that
cost hours before:**
- **EXTRA_CFLAGS staleness trap:** switching `-DTKAPP_BOOT` on/off does
  NOT recompile `kernel.c` (no flag-hash in deps). `touch src/kernel.c`
  first, then verify: `python -c "print(b'[TKAPP] launching' in
  open('tobyos.bin','rb').read())"`. A TKAPP-less kernel boots to the
  login screen and `holding` never appears (looks like 4 failed boots).
- **Flaky early-boot stall:** the TKAPP harness intermittently faults/
  stalls right after app spawn (garbled `*** EXCEPTION` at ~750ms, or
  silence). Retry-boot 2–4× before blaming your code — the browser
  itself launches clean once past boot. See `[[gui-line-vertical-hang]]`.
- QEMU SLIRP gives real DNS + internet. Local test server on the host:
  `python -m http.server` (note **port 8000 is taken by Epic Games on
  this box — use 8077**); reach it at `http://10.0.2.2:8077/...`.
  QMP relative-mouse targeting is unreliable — prefer keyboard-driven
  test hooks (that's why Stage 2 added `e`).

## DEFINITION OF DONE
Open 3+ tabs to different sites (`+` and Ctrl+T); each loads
independently; click between them and each shows **its own** page,
scroll position, title (WM title bar updates), and history (back/forward
per tab); close a middle tab (`×` / Ctrl+W) and a neighbor becomes
active; closing the last tab quits. No regression to Stage 1–3
(layout/links/find, forms, images, omnibox/redirect/search/TLS).
Verified in QEMU with screenshots showing two different pages under two
tabs; ideally one real-HW pass on the EliteDesk (serial COM4 @ 38400).

## READ FIRST (memory notes)
- `[[browser-omnibox-tls]]` — the full browser history: omnibox, TLS
  fixes, TCP window, the Stage 1/2/3 architecture (layout engine, forms,
  images), the branch chain, and every test-recipe gotcha. **Start here.**
- `[[tobyos-build-env]]` — toolchain, the TMP prefix, no-header-dep-
  tracking clean-rebuild rule, EXTRA_CFLAGS staleness.
- `[[file-explorer-tk-menu]]` — TobyTK app patterns, SDK-header shadow,
  QMP screenshot-drive recipe.
- `[[gui-line-vertical-hang]]` — the flaky TKAPP boot; retry, don't
  single-sample-bisect.
- `[[real-hardware-elitedesk-bringup]]` — real-HW networking (AMT off),
  serial triage.
