# Real fetch() / XHR + CORS

Branch `browser-fetch-cors` (Chrome-parity push item 2). Before this,
`fetch()` was GET-only with no request headers/body and no response
headers, and `XMLHttpRequest` was a thin sync shim; there was no
cross-origin enforcement. Now the browser speaks real HTTP requests and
enforces CORS.

## Kernel
- **`http_get_ext`** (`src/http.c`): `http_get_opt` + a
  `struct http_request_ext { method, headers, body, body_len }`. The
  request builder emits the method, extra CRLF-joined header lines, an
  auto `Content-Length`, and (after the header block) the request body.
  Extended requests force the proven HTTP/1.x path (h2/h3 are GET-only
  v1) and a request with a body never reuses a parked connection.
  The raw response header block (status line .. blank line, <=2 KiB) is
  captured into `http_response.raw_hdrs` for extended requests so the
  caller can read response headers (`http_free` frees it).
- **`http_request_follow`** (`src/syscall.c`): like `http_get_follow`
  but GET/HEAD follow redirects; other methods return the 3xx.
- **`HTTP_F_NO_COOKIES`**: suppress the cookie jar (cross-origin fetch
  defaults to credential-less).
- **Async layer** (`src/http_async.c`): `httpa_start2` carries a kernel
  copy of method/headers/body/flags in the slot; the worker calls
  `http_request_follow`. `httpa_hdrs` copies the raw response headers
  out. Two new syscalls: `HTTP_START2` (183) and `HTTP_HDRS` (184),
  with `struct abi_http_start2`.

## Browser
- `D.fetchStart2(url, cb, method, headers, body, noCookies)` primitive;
  the pump delivers `{status, url, body, headers}` (raw header block
  read via `HTTP_HDRS`).
- **Prelude** (`JS_PRELUDE`):
  - `fetch(url, opt)` with `method`, `headers` (object or `Headers`),
    `body`, and `credentials` (`same-origin`|`include`|`omit`). Returns
    a Response with `ok/status/url/type/redirected`, a `Headers` object
    (`get`/`has`/`forEach`), and `text()`/`json()`/`clone()`. Auto
    `Content-Type: text/plain;charset=UTF-8` for a string body.
  - **CORS**: origin computed from `location.href`. A cross-origin
    response must carry `Access-Control-Allow-Origin: *` or the exact
    page origin, else the promise rejects with a `TypeError` (the body
    is never exposed). Same-origin sends cookies; cross-origin omits
    them unless `credentials:'include'`.
  - `XMLHttpRequest` rewritten to the same async primitive:
    real method/headers/body, `getResponseHeader` /
    `getAllResponseHeaders`, `onload`/`onerror`/`onreadystatechange`,
    and the same CORS gate (cross-origin without the allow header is a
    network error: status 0, `onerror`).

## Verified
A two-origin rig (`websrv_cors.py`: page on :8099, API on :8098) run in
the browser, five chained tests all on screen:
1. same-origin `GET` + `json()` -> ok;
2. cross-origin `GET` to :8098 `/allow` (sends `ACAO:*`) -> status 200,
   custom `X-Demo` response header readable, body received;
3. cross-origin `GET` to :8098 `/deny` (no ACAO) -> **blocked**
   (promise rejects, body never seen);
4. same-origin `POST` with a body and custom `X-Tag` header ->
   server echoes `method=POST body=hello-body tag=toby`;
5. same-origin `XHR` -> status 200 with the `content-type` response
   header.

## Limits / follow-ups
No CORS *preflight* (`OPTIONS` for non-simple requests/headers) --
only the response-origin check; `Access-Control-Allow-Headers/Methods/
Credentials` and `Access-Control-Expose-Headers` are not enforced;
no `ReadableStream`/streaming bodies, `FormData`, `Blob`, or
`AbortController`; redirects on non-GET return the 3xx to JS rather
than auto-following per spec.
