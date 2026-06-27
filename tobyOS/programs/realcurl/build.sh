#!/bin/bash
# Fetch an UNMODIFIED, off-the-shelf static curl binary for the REALCURL milestone.
#
# Source: stunnel/static-curl -- upstream curl built fully static against musl
# (x86_64), single self-contained ELF (no DSOs, no loader). We lift it
# byte-for-byte; the Makefile brands EI_OSABI=Linux (byte 7) at initrd-assembly
# time so tobyOS's Linux personality picks it up (same treatment as realbash).
#
# This is a real, third-party HTTP client (libcurl) -- a much larger real-world
# networking surface than busybox wget (B17). It does its own getaddrinfo (musl)
# over /etc/resolv.conf + a non-blocking TCP connect()/poll() HTTP/1.1 GET.
#
# Output: programs/realcurl/curl   (gitignored; opt-in)
set -e
cd "$(dirname "$0")"

VER="8.20.0"
TARBALL="curl-linux-x86_64-musl-${VER}.tar.xz"
URL="https://github.com/stunnel/static-curl/releases/download/${VER}/${TARBALL}"
SHA256="58c6fab6e3f62d39d23224d752de1302cb717d997288d0f23d6fa7e79c393c1f"

echo "[realcurl] fetching unmodified static curl ${VER} (musl, x86_64) ..."
if command -v curl >/dev/null 2>&1; then
    curl -L -o "$TARBALL" "$URL"
elif command -v wget >/dev/null 2>&1; then
    wget -O "$TARBALL" "$URL"
else
    echo "[realcurl] ERROR: need host curl or wget to download" >&2
    exit 1
fi

# The tarball contains a single top-level file: curl
rm -f curl
tar -xf "$TARBALL" curl
chmod +x curl

# Verify integrity of the extracted binary against the published checksum
# (stunnel/static-curl publishes the SHA256 of the curl binary itself).
if command -v sha256sum >/dev/null 2>&1; then
    got="$(sha256sum curl | awk '{print $1}')"
elif command -v shasum >/dev/null 2>&1; then
    got="$(shasum -a 256 curl | awk '{print $1}')"
else
    got=""
fi
if [ -n "$got" ] && [ "$got" != "$SHA256" ]; then
    echo "[realcurl] ERROR: sha256 mismatch on extracted binary" >&2
    echo "  expected $SHA256" >&2
    echo "  got      $got" >&2
    exit 1
fi
[ -n "$got" ] && echo "[realcurl] sha256 OK (binary): $got"

echo "[realcurl] staged: $(pwd)/curl"
file curl 2>/dev/null || true
echo "[realcurl] done. Build the ISO with EXTRA_CFLAGS+=-DREALCURL_BOOT, then run logs/realcurl.sh"
