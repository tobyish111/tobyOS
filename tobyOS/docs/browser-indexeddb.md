# Browser — IndexedDB (v1)

Branch `browser-idb`, stacked on `browser-webp`. The structured-storage
API real SPAs persist through: `indexedDB.open()`, object stores,
transactions, requests, and cursors — with per-origin, per-database
persistence that survives reload and reboot.

## What shipped
- **Browser only** (`programs/user_gui_browser/main.c`) — no kernel
  changes:
  - Two C primitives, `idbLoad(db)` / `idbSave(db, blob)`, mirroring
    the localStorage pair: one JSON blob per (origin, database) at
    `/data/browser/<host>.i_<db>.idb`, whole-file writes (64 KiB cap).
  - The entire IndexedDB machinery lives in the JS prelude over that
    blob:
    - `indexedDB.open(name, version)` with the request lifecycle —
      `onupgradeneeded` fires exactly when the stored version is
      lower (with `oldVersion`/`newVersion`), then `onsuccess`;
      `deleteDatabase`; `cmp`.
    - `IDBDatabase`: `version`, `objectStoreNames`
      (contains/item/length), `createObjectStore` (keyPath +
      autoIncrement), `deleteObjectStore`, `transaction`, `close`.
    - `IDBObjectStore`: `put`, `add` (ConstraintError on duplicate),
      `get`, `getAll`, `getAllKeys`, `count`, `delete`, `clear`, and
      `openCursor` (key-ordered iteration with `continue()`).
      In-line keys via keyPath, out-of-line keys, and autoIncrement
      (the sequence tracks past explicit numeric keys, per spec).
    - Requests fire `onsuccess`/`onerror` asynchronously through the
      existing timer pump; transactions count their pending requests
      and fire `oncomplete` after the last one settles (requests
      chained inside handlers keep the transaction open).
    - Key order: numbers ascending, then strings — the common subset
      of the spec's collation.
  - Every mutation persists immediately (small whole-file write, the
    localStorage precedent), so data survives even if the page
    navigates before `oncomplete`.

## Verified (QEMU, `/idb` test page)
- **First load**: `UPGRADE fired: v0 -> 1`, stores created; two
  autoIncrement adds + one explicit-key put → `getAll: 3 notes`,
  `count = 3`, `get(99)` returns the right record, cursor iterates
  `1,2,99` (numeric order), `tx.oncomplete fired`, visits counter = 1
  (out-of-line key in a second store).
- **Reload (fresh JS runtime)**: no upgrade refire (version check
  against the persisted schema), visits = 2, autoIncrement continued
  past the explicit key (`100, 101`), `count = 5`, cursor order
  `1,2,99,100,101`.
- Regression: the `/store` persistent-localStorage page is unchanged.

## v1 limits
- No indexes (`createIndex` returns an inert stub so upgrade handlers
  survive; `store.index()` throws) — key-based access only.
- Values must be JSON-clonable (no Blob/File/Date/ArrayBuffer
  round-trip); keys are numbers or strings (no arrays/dates).
- No key ranges (`IDBKeyRange`) — `get`/cursor iterate all keys.
- Transactions are not atomic and don't isolate: each mutation
  persists as it executes; `oncomplete` is a notification, and
  `abort()` doesn't roll back.
- 64 KiB per database (whole-file JSON serialization).
- No cross-tab `versionchange`/`blocked` events.
