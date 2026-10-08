/* libtoby/src/string.c -- string + memory routines.
 *
 * Byte-at-a-time C, no SIMD. We deliberately don't reach for any
 * builtin_memcpy etc. because user programs are built with -nostdlib;
 * GCC/clang must see real definitions in the archive. */

#include <string.h>
#include <stdlib.h>

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i-- > 0; ) d[i] = s[i];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    unsigned char  v = (unsigned char)c;
    for (size_t i = 0; i < n; i++) d[i] = v;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char        v = (unsigned char)c;
    for (size_t i = 0; i < n; i++) if (p[i] == v) return (void *)(p + i);
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s; while (*p) p++; return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t cap) {
    size_t i = 0;
    while (i < cap && s[i]) i++;
    return i;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0)  return 0;
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) { /* copy incl NUL */ }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst + strlen(dst);
    while ((*d++ = *src++)) { /* */ }
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    size_t i = 0;
    for (; i < n && src[i]; i++) d[i] = src[i];
    d[i] = '\0';
    return dst;
}

char *strchr(const char *s, int c) {
    char v = (char)c;
    for (;; s++) {
        if (*s == v) return (char *)s;
        if (!*s) return 0;
    }
}

char *strrchr(const char *s, int c) {
    char v = (char)c;
    const char *last = 0;
    for (; *s; s++) if (*s == v) last = s;
    if (v == 0) return (char *)s;
    return (char *)last;
}

char *strstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    size_t nlen = strlen(needle);
    for (; *hay; hay++) {
        if (strncmp(hay, needle, nlen) == 0) return (char *)hay;
    }
    return 0;
}

char *strdup(const char *s) {
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) return 0;
    memcpy(p, s, n + 1);
    return p;
}

char *strndup(const char *s, size_t n) {
    size_t real = strnlen(s, n);
    char *p = (char *)malloc(real + 1);
    if (!p) return 0;
    memcpy(p, s, real);
    p[real] = '\0';
    return p;
}

const char *strerror(int err) {
    /* MUSL'S WORDS, ALL OF THEM: the C library every Linux program on the
     * system speaks -- bash, busybox -- so a native tool says what they
     * say. The table was half glibc's ("No child processes", "File name
     * too long", "Too many levels of symbolic links", "Success") and
     * stopped at 40 (finding 73). These are read out of the guest's static
     * bash (programs/realbash/bash: musl's message table, in its order),
     * indexed here by the Linux errno; 0 and anything unknown are musl's
     * "No error information". */
    switch (err) {
    case 1:   return "Operation not permitted";
    case 2:   return "No such file or directory";
    case 3:   return "No such process";
    case 4:   return "Interrupted system call";
    case 5:   return "I/O error";
    case 6:   return "No such device or address";
    case 7:   return "Argument list too long";
    case 8:   return "Exec format error";
    case 9:   return "Bad file descriptor";
    case 10:  return "No child process";
    case 11:  return "Resource temporarily unavailable";
    case 12:  return "Out of memory";
    case 13:  return "Permission denied";
    case 14:  return "Bad address";
    case 15:  return "Block device required";
    case 16:  return "Resource busy";
    case 17:  return "File exists";
    case 18:  return "Cross-device link";
    case 19:  return "No such device";
    case 20:  return "Not a directory";
    case 21:  return "Is a directory";
    case 22:  return "Invalid argument";
    case 23:  return "Too many open files in system";
    case 24:  return "No file descriptors available";
    case 25:  return "Not a tty";
    case 26:  return "Text file busy";
    case 27:  return "File too large";
    case 28:  return "No space left on device";
    case 29:  return "Invalid seek";
    case 30:  return "Read-only file system";
    case 31:  return "Too many links";
    case 32:  return "Broken pipe";
    case 33:  return "Domain error";
    case 34:  return "Result not representable";
    case 35:  return "Resource deadlock would occur";
    case 36:  return "Filename too long";
    case 37:  return "No locks available";
    case 38:  return "Function not implemented";
    case 39:  return "Directory not empty";
    case 40:  return "Symbolic link loop";
    case 42:  return "No message of desired type";
    case 43:  return "Identifier removed";
    case 60:  return "Device not a stream";
    case 61:  return "No data available";
    case 62:  return "Device timeout";
    case 63:  return "Out of streams resources";
    case 67:  return "Link has been severed";
    case 71:  return "Protocol error";
    case 72:  return "Multihop attempted";
    case 74:  return "Bad message";
    case 75:  return "Value too large for data type";
    case 77:  return "File descriptor in bad state";
    case 84:  return "Illegal byte sequence";
    case 88:  return "Not a socket";
    case 89:  return "Destination address required";
    case 90:  return "Message too large";
    case 91:  return "Protocol wrong type for socket";
    case 92:  return "Protocol not available";
    case 93:  return "Protocol not supported";
    case 94:  return "Socket type not supported";
    case 95:  return "Not supported";
    case 96:  return "Protocol family not supported";
    case 97:  return "Address family not supported by protocol";
    case 98:  return "Address in use";
    case 99:  return "Address not available";
    case 100: return "Network is down";
    case 101: return "Network unreachable";
    case 102: return "Connection reset by network";
    case 103: return "Connection aborted";
    case 104: return "Connection reset by peer";
    case 105: return "No buffer space available";
    case 106: return "Socket is connected";
    case 107: return "Socket not connected";
    case 108: return "Cannot send after socket shutdown";
    case 110: return "Operation timed out";
    case 111: return "Connection refused";
    case 112: return "Host is down";
    case 113: return "Host is unreachable";
    case 114: return "Operation already in progress";
    case 115: return "Operation in progress";
    case 116: return "Stale file handle";
    case 121: return "Remote I/O error";
    case 122: return "Quota exceeded";
    case 123: return "No medium found";
    case 124: return "Wrong medium type";
    case 125: return "Operation canceled";
    case 126: return "Required key not available";
    case 127: return "Key has expired";
    case 128: return "Key has been revoked";
    case 129: return "Key was rejected by service";
    case 130: return "Previous owner died";
    case 131: return "State not recoverable";
    default:  return "No error information";
    }
}

/* ---- strtok ----------------------------------------------------- */

char *strtok_r(char *s, const char *delim, char **saveptr);

static char *g_strtok_save = 0;

char *strtok(char *s, const char *delim) {
    return strtok_r(s, delim, &g_strtok_save);
}

char *strtok_r(char *s, const char *delim, char **saveptr) {
    if (!s) s = *saveptr;
    if (!s) return 0;

    /* Skip leading delimiters */
    while (*s) {
        int is_delim = 0;
        for (const char *d = delim; *d; d++) {
            if (*s == *d) { is_delim = 1; break; }
        }
        if (!is_delim) break;
        s++;
    }
    if (!*s) { *saveptr = 0; return 0; }

    char *tok = s;
    while (*s) {
        for (const char *d = delim; *d; d++) {
            if (*s == *d) {
                *s = '\0';
                *saveptr = s + 1;
                return tok;
            }
        }
        s++;
    }
    *saveptr = 0;
    return tok;
}

/* ---- strcoll (no locale, same as strcmp) ------------------------- */

int strcoll(const char *a, const char *b) {
    return strcmp(a, b);
}

/* ---- case-insensitive comparisons ------------------------------- */

int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = (unsigned char)*a;
        int cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    int ca = (unsigned char)*a;
    int cb = (unsigned char)*b;
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb += 32;
    return ca - cb;
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = (unsigned char)a[i];
        int cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
    return 0;
}

/* ---- strspn / strcspn / strpbrk --------------------------------- */

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n]) {
        int found = 0;
        for (const char *a = accept; *a; a++) {
            if (s[n] == *a) { found = 1; break; }
        }
        if (!found) break;
        n++;
    }
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n]) {
        for (const char *r = reject; *r; r++) {
            if (s[n] == *r) return n;
        }
        n++;
    }
    return n;
}

char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        for (const char *a = accept; *a; a++) {
            if (*s == *a) return (char *)s;
        }
        s++;
    }
    return 0;
}

/* ---- stpcpy ----------------------------------------------------- */

char *stpcpy(char *dst, const char *src) {
    while ((*dst = *src)) { dst++; src++; }
    return dst;
}
