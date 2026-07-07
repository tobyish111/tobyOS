# Browser stage 12D: async HTTP — the UI never freezes

Branch `kernel-async-http` (stage-12 scope item D, stacked on
`browser-flexbox`). The long-documented blocker: `SYS_HTTP_FETCH` is
synchronous, so every page/sheet/script/image load froze the whole
browser and JS `fetch()` was a Promise over a blocking call. This
stage adds the async kernel ABI and converts the browser to it.

## Kernel side

### The ABI (abi.h, syscalls 172–175)
- `SYS_HTTP_START(struct abi_http_start*)` → handle. Queues a transfer
  (url + max_body cap) and returns immediately.
- `SYS_HTTP_POLL(handle, struct abi_http_poll*)` → state
  (QUEUED/RUNNING/DONE/ERROR) + status/body_len/body_total/final_url/
  content_type when DONE, HTTP_ERR when ERROR.
- `SYS_HTTP_READ(handle, buf, off<<32|len)` → copies body bytes out
  (bounced through a kernel buffer, ≤ 1 MiB).
- `SYS_HTTP_FINISH(handle)` → frees the slot; on a queued/running
  handle it CANCELS (the worker discards the result on return).
- `SYS_HTTP_FETCH` stays untouched for wget/pkg (and the browser's
  remaining synchronous asset fetches, see limits).

### The worker (src/http_async.c + proc.c + tcp.c)
- New `proc_create_kernel(entry, name)`: a ring-0 worker process —
  same PCB/fake-initial-frame machinery as any proc, but
  `proc_first_user_entry` sees `is_kernel` and CALLs the kernel
  function instead of iret-ing to ring 3. Kernel PML4, ADMIN caps
  (same blanket as pid 0 / ap_idle), own kmalloc'd kstack. Spawned
  lazily on the first SYS_HTTP_START.
- The worker scans an 8-slot table for QUEUED work under the BKL and
  runs the EXISTING synchronous engine (`http_get_follow`: DNS +
  TCP/TLS + redirects + dechunk + gunzip + keep-alive — everything
  stages 1–12A built) — full reuse, no state-machine rewrite.
- New per-proc `tcp_yield_wait` opt-in: `tcp_poll_until`'s wait does
  `sched_yield()` instead of `hlt` for the worker, so a long transfer
  SHARES its CPU instead of parking it (and the BKL is already
  released there, so no lock-holding yields). Everyone else keeps the
  documented hlt wait.
- `http_get_follow` gained a per-recv timeout parameter: the worker
  passes 30 s (an async caller tolerates slow servers by design — the
  first run of the acceptance test found the 5 s default killing a
  9-second-quiet page with ERROR -7).
- Slots owned by exited processes are reaped lazily when the table
  fills; FINISH-on-running marks `cancel` and the worker frees.

## Browser side (programs/user_gui_browser/main.c)

- **Async navigation**: `fetch_page` (blocking) is gone. `nav_begin`
  starts a kernel transfer for the current tab and returns; the main
  loop's `nav_pump()` polls every tab's handle and, on completion,
  pulls the body into the tab's raw buffer and runs the continuation.
  The old synchronous chains became continuation kinds: NAVK_NORMAL
  (render + history), NAVK_PLAIN (back/forward/refresh), NAVK_DDG
  (DuckDuckGo search → Mojeek fallback on 202/error), NAVK_HOST0..3
  (the bare-host https→www→http ladder). Each tab loads
  independently; navigating again or closing a tab cancels its
  in-flight transfer.
- **JS fetch() is truly async**: the prelude's fetch() now calls
  `__dom.fetchStart(url, cb)`; the pump polls the per-tab pending
  table and settles the Promise with `{status, url, body}` when the
  transfer lands. Timers tick, events dispatch and paints happen
  during the transfer. XHR keeps the sync path (v1).
- **Images**: the cooperative image loop no longer blocks per image —
  one transfer in flight at a time, started on one idle pass, polled
  on later ones, decoded on arrival. Tab close / navigation cancels
  and revalidates.

## Verified in QEMU (screenshots in the session scratchpad)
- `/asy` page: a 250 ms interval keeps counting while a 6 s fetch()
  is in flight — "FETCH RESOLVED after 14 ticks (ok=true)" with the
  ticker at 31 and still counting. (Ladder target 5, JS half.)
- Slow page (`/slow?ms=9000`) loading in tab 2 while tab 1 stays
  live: status bar shows Loading…, Ctrl+P switches back mid-load, the
  ticker has advanced, scrolling works; Ctrl+N later shows the slow
  page arrived and rendered. (Ladder target 5, UI half.)
- Regressions through the async path: CSS torture page, tables page.
- Real internet through the async path: google.com (TLS + redirect +
  chunked+gzip + assets), Wikipedia article.

## v1 limits
Stylesheet/`<script src>` fetches inside the render pipeline still use
the synchronous SYS_HTTP_FETCH (they run after the page body arrives;
short thanks to 12A keep-alive — the render itself is synchronous
anyway). One image transfer in flight at a time. One kernel worker =
one transfer at a time overall (page > image ordering follows from
the browser's own start order); parallel workers are a follow-up.
