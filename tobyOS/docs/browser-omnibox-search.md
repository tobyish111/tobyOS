# Slice 121 — the omnibox searches, and the profile says whether it persisted

Two small changes to `programs/chromewin/main.c` that together close the gap
between "Chromium renders pages" and "Chromium behaves like a browser you'd
use".

## 1. The omnibox decides URL vs. search

Slice 118 built the omnibox but treated **every** entry as a hostname:

```c
else snprintf(url, sizeof url, "https://%s", g_omni);   /* "cats" -> https://cats */
```

So the single most common browser action — type words, press Enter — produced
a DNS failure. Searching meant loading an engine's homepage first and typing
into the page.

`omni_navigate()` now picks between navigating and searching:

| input | result |
| --- | --- |
| contains `://` | URL verbatim |
| starts with `/` | `file://…` |
| `localhost`, `localhost:8080` | URL |
| `example.com`, `a.co`, `en.wikipedia.org/wiki/Rust` | URL |
| `10.0.0.1`, `192.168.1.1:80` | URL (complete dotted quad) |
| `cats`, `how do i cook rice`, `main.c`, `3.5`, `note: buy milk` | **search** |

The rule is deliberately biased toward search. A query misread as a host shows
an error page; a host misread as a query shows results that link to the site.
Wrong-toward-search is the recoverable direction.

Two cases are worth stating because both were wrong in the first draft and the
gate caught them:

- **A single numeric label after a dot is not an IP address.** The first
  version accepted "last label is all digits" as the last octet of a dotted
  quad, which turned `3.5`, `1.5` and `2026.08` into DNS lookups. Only a
  complete four-label 0–255 quad counts (`is_dotted_quad`).
- **No TLD is one letter.** `tld >= 2` is load-bearing: at `>= 1`, `main.c`
  and `foo.h` become hostnames. The gate did not cover this until a
  deliberately broken build was run against it and passed.

The query is percent-encoded (`url_encode_query`), and the final URL is
JSON-escaped before it reaches CDP — the `://` branch passes user text through
verbatim, and an unescaped quote there produced malformed CDP that chrome
answers with a parse error and no navigation.

### Choosing the engine

**Default is Bing, not Google**, and the reason is measured rather than
aesthetic. Google serves this deployment its "unusual traffic" interstitial for
real browser searches — photographed on the EliteDesk 2026-08-15 and reproduced
in QEMU (`logs/captcha.log`: `/sorry/index?continue=…search%3Fq%3Dcats`) — and
the reCAPTCHA on that page cannot currently be completed here, so a Google
default dead-ends the single most common browser action. Bing answers normally
from the same address. Google is one setting, or one `google.com`, away.

Note what this is *not*: a fix for the bot gate. It routes around one engine's
challenge. A cookie-less scripted GET from the same address gets served Google
results perfectly, while Chromium is challenged — so the gate is judging the
browser once JavaScript runs, not merely the address.

Override, in priority order:

1. `TOBY_SEARCH` in the environment (a shell launch, for one run)
2. `browser.search` in `/data/settings.conf` (persistent, survives reboot)

Either accepts an engine key — `google`, `ddg`, `bing`, `brave`, `wikipedia` —
or a full URL template containing exactly one `%s`. A value that is neither
logs a warning and falls back to Google; a template without `%s` is rejected
rather than silently sending every search to the same static page.

chromewin reads `/data/settings.conf` directly. `settings_get_str()` lives in
the kernel with no syscall behind it, but the backing file is plain
`key=value` text, so parsing it uses the **same** store rather than inventing
a second config file the Control Panel would not know about.

> **The engine choice is not a way around an "unusual traffic" block.** When an
> address is flagged, Google and DuckDuckGo gate it independently. Switching
> engines does not dodge it and nothing on this side can — see the bot-gate
> history. This is a convenience, not a remedy.

## 2. The profile reports whether it survived

`report_profile_state()` prints one line at startup:

```
[chromewin] profile /data/cr2: FRESH (empty) -- no cookies carried over; if /data is RAM-backed this repeats every boot
[chromewin] profile /data/cr2: REUSED (23 entries, cookie db present)
```

This exists because the failure mode is invisible from inside the window. When
the boot sweep finds no tobyfs volume, `/data` is RAM-backed
(`kernel.c`, priority 4), so chrome gets an empty `--user-data-dir` every
boot: consent screens return, nothing stays signed in, and every site sees a
first-time visitor. From the user's chair that just looks like the web being
tedious. The count alone is not the verdict — `Default/Cookies` is checked
separately, because a directory that merely exists is not a reused profile.

## Gates

- **`python logs/omnisearch.py`** — 41 host-compiled assertions over the
  URL-vs-search predicate and the query encoder. The functions are *extracted
  from `main.c` verbatim*, not copied: a copied test drifts from the shipped
  code silently, which is the exact failure this file guards against. Exit
  0/1.
- **`bash logs/cwprofile.sh`** — two QEMU boots on one blank scratch volume:
  boot 1 must report `FRESH`, boot 2 must report `REUSED`. The two-sided shape
  is the point — "REUSED on boot 2" alone would also be printed by a harness
  that never wiped anything. It does not touch the repo's `disk.img`, whose
  accumulated profile is the ambiguity the test exists to remove.
  `--verdict-only` re-scores existing logs without spending two boots.

### Result, 2026-08-15

```
boot1: profile: FRESH                       /data inodes at mount = 1
boot2: profile: REUSED, cookie db present   /data inodes at mount = 141
PASS
```

The inode count is a second, **independent** witness: it comes from the tobyfs
mount line, not from chromewin's own reporting, so a bug in the reporter
cannot fake it.

### Three traps this harness paid for

1. **The serial log splits every `printf` at its format conversions.** The
   profile line lands as `[chromewin] profile ` / `/data/cr2` / `: REUSED (` /
   `1` / …, interleaved with `[fd1] len=N:` trace lines, so a whole-line grep
   matches a fragment carrying no verdict word. The first run reported
   INCONCLUSIVE on a run that had actually passed. Match verdict *tokens* —
   they sit inside literal runs and are never split. Do not try to reassemble
   the sentence either: every fragment appears twice, so `grep -o` output
   concatenates into `absentpresent: FRESH (: FRESH (`.
2. `grep -o "[0-9]*/"` also matches the bare slashes in `'/data'` (zero digits
   then `/`). One `sed` capture instead.
3. **`--verdict-only` was originally placed after the build**, so "just
   re-score it" rebuilt the kernel and deleted the scratch disk the logs came
   from. A read-only flag that writes is a trap for the next reader.

Note that QEMU under Chromium load runs ~2.4× slower than wall clock (guest
70 s at 169 s real). A slow capture is not a hang.

`usbprov.sh` covers the storage half (stick → provision → `/data` returns);
`cwprofile.sh` covers the browser half on top of it.
