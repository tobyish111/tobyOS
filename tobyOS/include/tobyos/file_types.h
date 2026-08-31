/* include/tobyos/file_types.h -- THE file-type registry.
 *
 * ONE table, shared by the kernel and userland. Before this file there were
 * three: src/file_assoc.c (kernel, 184 lines), libtoby/src/fileassoc.c
 * (userland, 115 lines) -- both DEAD, nothing outside either file ever called
 * them, not even their own _init() -- and a third set of hardcoded
 * streq(ext,".txt") chains inside programs/user_gui_files/main.c, which was
 * the only one that actually ran. Three answers to "what is this file", two
 * of which no one could see, and they disagreed: the dead tables sent .txt to
 * the read-only viewer while the live code sent it to the editor.
 *
 * So the data lives here exactly once, as an X-macro, and each world expands
 * it into its own array (see file_assoc.c and libtoby/src/fileassoc.c). An
 * X-macro rather than a `static const` table in the header because a header
 * table is copied into every translation unit that includes it; this way the
 * DATA has one home and each world links one copy.
 *
 * No #includes on purpose: this is compiled into freestanding userland
 * programs and the kernel alike, so it must not drag either world's headers
 * into the other.
 *
 * Adding a type is one line. Keep the label short -- File Explorer shows it
 * in a column -- and keep it TRUE: a label is a claim about what the file is,
 * and the handler is a claim that the handler can cope with it. Where nothing
 * can, say so with FTA_NONE instead of pointing at an app that will open a
 * blank window.
 */

#ifndef TOBYOS_FILE_TYPES_H
#define TOBYOS_FILE_TYPES_H

/* Broad category, so callers can ask "is this an image?" without knowing
 * every image extension. Used by gui_viewer to decide whether to attempt a
 * decode at all, and by the kernel's file_assoc_get_icon(). NOT used for
 * colour in File Explorer: tk_table cells are plain strings with no per-cell
 * colour, so the group would have nowhere to show. */
enum file_group {
    FTG_OTHER = 0,
    FTG_TEXT,       /* prose, logs, notes */
    FTG_CODE,       /* source and scripts */
    FTG_DATA,       /* config, structured data */
    FTG_WEB,        /* markup and stylesheets */
    FTG_IMAGE,
    FTG_AUDIO,
    FTG_VIDEO,
    FTG_ARCHIVE,
    FTG_DISK,       /* disc and disk images */
    FTG_EXEC,       /* programs, libraries, objects */
    FTG_FONT,
    FTG_DOC,        /* office/e-book documents */
    FTG_GROUP_COUNT
};

/* What double-click should DO. */
enum file_action {
    FTA_NONE = 0,   /* nothing here can open it -- say so, do not pretend */
    FTA_OPEN,       /* spawn `handler` with the path as its argument */
    FTA_RUN         /* the file IS the program: execute it */
};

struct file_type {
    const char   *ext;      /* lowercase, no leading dot */
    const char   *label;    /* what Explorer's Type column shows */
    const char   *handler;  /* app to spawn, or 0 */
    unsigned char group;    /* enum file_group */
    unsigned char editable; /* safe to load into the text editor */
    unsigned char action;   /* enum file_action */
};

/* Handlers. Every one of these must exist in the initrd -- a handler that
 * is not installed is a double-click that does nothing, with no error. */
#define H_EDIT   "/bin/gui_edit"
#define H_VIEW   "/bin/gui_viewer"
#define H_BROWSE "/bin/gui_browser"
#define H_PLAY   "/bin/mediaplayer"
#define H_NONE   0

/* gui_viewer has Text, Hex and Image modes, so it is the honest destination
 * for two very different things: a binary we can only show as hex (still
 * useful -- you can look at any file on the system) and an image it can
 * genuinely decode. Both are H_VIEW; the viewer picks the mode from the
 * content, not from our say-so.
 *
 * IMAGES: libtoby's decoder is stb_image, which handles PNG, JPEG, GIF, BMP,
 * TGA, PSD, PIC and the Netpbm family. The other image formats below are
 * labelled honestly and still routed to the viewer, which reports that it
 * cannot decode them and falls back to hex rather than showing a blank frame.
 *
 * AUDIO: mediaplayer builds its playlist from .mp3/.wav/.ogg/.m4a and ignores
 * everything else, so only those four are routed to it. Sending it a .flac
 * would open a player that silently refuses to list the file.
 *
 * VIDEO: nothing here decodes video. Every video type is FTA_NONE. */

#define FILE_TYPE_TABLE(X)                                                     \
    /* ---- plain text ---- */                                                 \
    X("txt",      "Text document",        H_EDIT,   FTG_TEXT,  1, FTA_OPEN)    \
    X("text",     "Text document",        H_EDIT,   FTG_TEXT,  1, FTA_OPEN)    \
    X("md",       "Markdown document",    H_EDIT,   FTG_TEXT,  1, FTA_OPEN)    \
    X("markdown", "Markdown document",    H_EDIT,   FTG_TEXT,  1, FTA_OPEN)    \
    X("rst",      "reStructuredText",     H_EDIT,   FTG_TEXT,  1, FTA_OPEN)    \
    X("log",      "Log file",             H_EDIT,   FTG_TEXT,  1, FTA_OPEN)    \
    X("nfo",      "Info document",        H_EDIT,   FTG_TEXT,  1, FTA_OPEN)    \
    X("me",       "Text document",        H_EDIT,   FTG_TEXT,  1, FTA_OPEN)    \
    X("tex",      "TeX document",         H_EDIT,   FTG_TEXT,  1, FTA_OPEN)    \
    X("bib",      "BibTeX bibliography",  H_EDIT,   FTG_TEXT,  1, FTA_OPEN)    \
    /* ---- configuration and structured data ---- */                          \
    X("conf",     "Configuration file",   H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    X("cfg",      "Configuration file",   H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    X("ini",      "INI configuration",    H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    X("json",     "JSON data",            H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    X("xml",      "XML document",         H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    X("yaml",     "YAML document",        H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    X("yml",      "YAML document",        H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    X("toml",     "TOML configuration",   H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    X("csv",      "CSV spreadsheet",      H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    X("tsv",      "TSV data",             H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    X("env",      "Environment file",     H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    X("desktop",  "Desktop entry",        H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    X("service",  "Service unit",         H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    X("rc",       "Startup script",       H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    X("gitignore","Ignore rules",         H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    X("lock",     "Lock file",            H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    X("pid",      "PID file",             H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    X("sum",      "Checksum file",        H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    X("map",      "Map file",             H_EDIT,   FTG_DATA,  1, FTA_OPEN)    \
    /* ---- source code and scripts ---- */                                    \
    X("c",        "C source",             H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("h",        "C header",             H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("cpp",      "C++ source",           H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("cc",       "C++ source",           H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("cxx",      "C++ source",           H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("hpp",      "C++ header",           H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("hh",       "C++ header",           H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("s",        "Assembly source",      H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("asm",      "Assembly source",      H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("py",       "Python script",        H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("js",       "JavaScript source",    H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("mjs",      "JavaScript module",    H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("ts",       "TypeScript source",    H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("tsx",      "TypeScript JSX",       H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("jsx",      "JavaScript JSX",       H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("sh",       "Shell script",         H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("bash",     "Bash script",          H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("zsh",      "Zsh script",           H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("tsh",      "tsh script",           H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("pl",       "Perl script",          H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("rb",       "Ruby script",          H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("go",       "Go source",            H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("rs",       "Rust source",          H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("java",     "Java source",          H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("kt",       "Kotlin source",        H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("cs",       "C# source",            H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("swift",    "Swift source",         H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("php",      "PHP script",           H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("lua",      "Lua script",           H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("sql",      "SQL script",           H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("hs",       "Haskell source",       H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("ml",       "OCaml source",         H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("lisp",     "Lisp source",          H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("el",       "Emacs Lisp source",    H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("scm",      "Scheme source",        H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("dart",     "Dart source",          H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("zig",      "Zig source",           H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("nim",      "Nim source",           H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("awk",      "AWK script",           H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("mk",       "Makefile fragment",    H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("cmake",    "CMake script",         H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("bat",      "Batch script",         H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("ps1",      "PowerShell script",    H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("patch",    "Patch file",           H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("diff",     "Diff file",            H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("ld",       "Linker script",        H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("proto",    "Protocol schema",      H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    X("vim",      "Vim script",           H_EDIT,   FTG_CODE,  1, FTA_OPEN)    \
    /* ---- web ---- */                                                        \
    X("html",     "HTML document",        H_BROWSE, FTG_WEB,   1, FTA_OPEN)    \
    X("htm",      "HTML document",        H_BROWSE, FTG_WEB,   1, FTA_OPEN)    \
    X("css",      "Stylesheet",           H_EDIT,   FTG_WEB,   1, FTA_OPEN)    \
    X("scss",     "Sass stylesheet",      H_EDIT,   FTG_WEB,   1, FTA_OPEN)    \
    X("less",     "Less stylesheet",      H_EDIT,   FTG_WEB,   1, FTA_OPEN)    \
    X("svg",      "SVG image",            H_EDIT,   FTG_WEB,   1, FTA_OPEN)    \
    X("wasm",     "WebAssembly module",   H_VIEW,   FTG_WEB,   0, FTA_OPEN)    \
    /* ---- images the viewer can decode ---- */                               \
    X("png",      "PNG image",            H_VIEW,   FTG_IMAGE, 0, FTA_OPEN)    \
    X("jpg",      "JPEG image",           H_VIEW,   FTG_IMAGE, 0, FTA_OPEN)    \
    X("jpeg",     "JPEG image",           H_VIEW,   FTG_IMAGE, 0, FTA_OPEN)    \
    X("gif",      "GIF image",            H_VIEW,   FTG_IMAGE, 0, FTA_OPEN)    \
    X("bmp",      "Bitmap image",         H_VIEW,   FTG_IMAGE, 0, FTA_OPEN)    \
    X("tga",      "Targa image",          H_VIEW,   FTG_IMAGE, 0, FTA_OPEN)    \
    X("psd",      "Photoshop image",      H_VIEW,   FTG_IMAGE, 0, FTA_OPEN)    \
    X("pnm",      "Netpbm image",         H_VIEW,   FTG_IMAGE, 0, FTA_OPEN)    \
    X("ppm",      "Netpbm image",         H_VIEW,   FTG_IMAGE, 0, FTA_OPEN)    \
    X("pgm",      "Netpbm image",         H_VIEW,   FTG_IMAGE, 0, FTA_OPEN)    \
    X("pbm",      "Netpbm image",         H_VIEW,   FTG_IMAGE, 0, FTA_OPEN)    \
    /* ---- images it cannot: labelled, viewer reports and falls back ---- */   \
    X("webp",     "WebP image",           H_VIEW,   FTG_IMAGE, 0, FTA_OPEN)    \
    X("avif",     "AVIF image",           H_VIEW,   FTG_IMAGE, 0, FTA_OPEN)    \
    X("ico",      "Icon",                 H_VIEW,   FTG_IMAGE, 0, FTA_OPEN)    \
    X("tif",      "TIFF image",           H_VIEW,   FTG_IMAGE, 0, FTA_OPEN)    \
    X("tiff",     "TIFF image",           H_VIEW,   FTG_IMAGE, 0, FTA_OPEN)    \
    X("heic",     "HEIC image",           H_VIEW,   FTG_IMAGE, 0, FTA_OPEN)    \
    X("qoi",      "QOI image",            H_VIEW,   FTG_IMAGE, 0, FTA_OPEN)    \
    /* ---- audio: only what mediaplayer will actually list ---- */            \
    X("wav",      "WAV audio",            H_PLAY,   FTG_AUDIO, 0, FTA_OPEN)    \
    X("mp3",      "MP3 audio",            H_PLAY,   FTG_AUDIO, 0, FTA_OPEN)    \
    X("ogg",      "Ogg audio",            H_PLAY,   FTG_AUDIO, 0, FTA_OPEN)    \
    X("m4a",      "MPEG-4 audio",         H_PLAY,   FTG_AUDIO, 0, FTA_OPEN)    \
    X("flac",     "FLAC audio",           H_NONE,   FTG_AUDIO, 0, FTA_NONE)    \
    X("aac",      "AAC audio",            H_NONE,   FTG_AUDIO, 0, FTA_NONE)    \
    X("opus",     "Opus audio",           H_NONE,   FTG_AUDIO, 0, FTA_NONE)    \
    X("mid",      "MIDI sequence",        H_NONE,   FTG_AUDIO, 0, FTA_NONE)    \
    X("midi",     "MIDI sequence",        H_NONE,   FTG_AUDIO, 0, FTA_NONE)    \
    X("aiff",     "AIFF audio",           H_NONE,   FTG_AUDIO, 0, FTA_NONE)    \
    /* ---- video: nothing decodes these yet ---- */                           \
    X("mp4",      "MPEG-4 video",         H_NONE,   FTG_VIDEO, 0, FTA_NONE)    \
    X("mkv",      "Matroska video",       H_NONE,   FTG_VIDEO, 0, FTA_NONE)    \
    X("avi",      "AVI video",            H_NONE,   FTG_VIDEO, 0, FTA_NONE)    \
    X("mov",      "QuickTime video",      H_NONE,   FTG_VIDEO, 0, FTA_NONE)    \
    X("webm",     "WebM video",           H_NONE,   FTG_VIDEO, 0, FTA_NONE)    \
    X("wmv",      "Windows Media video",  H_NONE,   FTG_VIDEO, 0, FTA_NONE)    \
    X("flv",      "Flash video",          H_NONE,   FTG_VIDEO, 0, FTA_NONE)    \
    X("mpg",      "MPEG video",           H_NONE,   FTG_VIDEO, 0, FTA_NONE)    \
    X("mpeg",     "MPEG video",           H_NONE,   FTG_VIDEO, 0, FTA_NONE)    \
    /* ---- archives ---- */                                                   \
    X("zip",      "ZIP archive",          H_VIEW,   FTG_ARCHIVE, 0, FTA_OPEN)  \
    X("tar",      "TAR archive",          H_VIEW,   FTG_ARCHIVE, 0, FTA_OPEN)  \
    X("gz",       "gzip archive",         H_VIEW,   FTG_ARCHIVE, 0, FTA_OPEN)  \
    X("tgz",      "gzipped TAR archive",  H_VIEW,   FTG_ARCHIVE, 0, FTA_OPEN)  \
    X("bz2",      "bzip2 archive",        H_VIEW,   FTG_ARCHIVE, 0, FTA_OPEN)  \
    X("xz",       "xz archive",           H_VIEW,   FTG_ARCHIVE, 0, FTA_OPEN)  \
    X("zst",      "Zstandard archive",    H_VIEW,   FTG_ARCHIVE, 0, FTA_OPEN)  \
    X("lz4",      "LZ4 archive",          H_VIEW,   FTG_ARCHIVE, 0, FTA_OPEN)  \
    X("7z",       "7-Zip archive",        H_VIEW,   FTG_ARCHIVE, 0, FTA_OPEN)  \
    X("rar",      "RAR archive",          H_VIEW,   FTG_ARCHIVE, 0, FTA_OPEN)  \
    X("cpio",     "cpio archive",         H_VIEW,   FTG_ARCHIVE, 0, FTA_OPEN)  \
    X("deb",      "Debian package",       H_VIEW,   FTG_ARCHIVE, 0, FTA_OPEN)  \
    X("rpm",      "RPM package",          H_VIEW,   FTG_ARCHIVE, 0, FTA_OPEN)  \
    X("apk",      "APK package",          H_VIEW,   FTG_ARCHIVE, 0, FTA_OPEN)  \
    X("jar",      "Java archive",         H_VIEW,   FTG_ARCHIVE, 0, FTA_OPEN)  \
    X("whl",      "Python wheel",         H_VIEW,   FTG_ARCHIVE, 0, FTA_OPEN)  \
    X("tpkg",     "tobyOS package",       H_VIEW,   FTG_ARCHIVE, 0, FTA_OPEN)  \
    X("cab",      "Cabinet archive",      H_VIEW,   FTG_ARCHIVE, 0, FTA_OPEN)  \
    /* ---- disk and disc images ---- */                                       \
    X("iso",      "ISO disc image",       H_VIEW,   FTG_DISK,  0, FTA_OPEN)    \
    X("img",      "Disk image",           H_VIEW,   FTG_DISK,  0, FTA_OPEN)    \
    X("vhd",      "Virtual hard disk",    H_VIEW,   FTG_DISK,  0, FTA_OPEN)    \
    X("vmdk",     "VMware disk",          H_VIEW,   FTG_DISK,  0, FTA_OPEN)    \
    X("qcow2",    "QEMU disk image",      H_VIEW,   FTG_DISK,  0, FTA_OPEN)    \
    X("vdi",      "VirtualBox disk",      H_VIEW,   FTG_DISK,  0, FTA_OPEN)    \
    X("dmg",      "Apple disk image",     H_VIEW,   FTG_DISK,  0, FTA_OPEN)    \
    /* ---- programs and libraries ---- */                                     \
    X("elf",      "Executable",           H_NONE,   FTG_EXEC,  0, FTA_RUN)     \
    X("so",       "Shared library",       H_VIEW,   FTG_EXEC,  0, FTA_OPEN)    \
    X("o",        "Object file",          H_VIEW,   FTG_EXEC,  0, FTA_OPEN)    \
    X("a",        "Static library",       H_VIEW,   FTG_EXEC,  0, FTA_OPEN)    \
    X("ko",       "Kernel module",        H_VIEW,   FTG_EXEC,  0, FTA_OPEN)    \
    X("exe",      "Windows executable",   H_VIEW,   FTG_EXEC,  0, FTA_OPEN)    \
    X("dll",      "Windows library",      H_VIEW,   FTG_EXEC,  0, FTA_OPEN)    \
    X("sys",      "Windows driver",       H_VIEW,   FTG_EXEC,  0, FTA_OPEN)    \
    X("efi",      "EFI application",      H_VIEW,   FTG_EXEC,  0, FTA_OPEN)    \
    X("msi",      "Windows installer",    H_VIEW,   FTG_EXEC,  0, FTA_OPEN)    \
    X("com",      "DOS executable",       H_VIEW,   FTG_EXEC,  0, FTA_OPEN)    \
    X("class",    "Java class",           H_VIEW,   FTG_EXEC,  0, FTA_OPEN)    \
    X("pyc",      "Python bytecode",      H_VIEW,   FTG_EXEC,  0, FTA_OPEN)    \
    /* ---- fonts ---- */                                                      \
    X("ttf",      "TrueType font",        H_VIEW,   FTG_FONT,  0, FTA_OPEN)    \
    X("otf",      "OpenType font",        H_VIEW,   FTG_FONT,  0, FTA_OPEN)    \
    X("woff",     "Web font",             H_VIEW,   FTG_FONT,  0, FTA_OPEN)    \
    X("woff2",    "Web font",             H_VIEW,   FTG_FONT,  0, FTA_OPEN)    \
    X("pcf",      "Bitmap font",          H_VIEW,   FTG_FONT,  0, FTA_OPEN)    \
    X("bdf",      "Bitmap font",          H_VIEW,   FTG_FONT,  0, FTA_OPEN)    \
    /* ---- documents ---- */                                                  \
    X("pdf",      "PDF document",         H_VIEW,   FTG_DOC,   0, FTA_OPEN)    \
    X("doc",      "Word document",        H_VIEW,   FTG_DOC,   0, FTA_OPEN)    \
    X("docx",     "Word document",        H_VIEW,   FTG_DOC,   0, FTA_OPEN)    \
    X("xls",      "Excel spreadsheet",    H_VIEW,   FTG_DOC,   0, FTA_OPEN)    \
    X("xlsx",     "Excel spreadsheet",    H_VIEW,   FTG_DOC,   0, FTA_OPEN)    \
    X("ppt",      "PowerPoint slides",    H_VIEW,   FTG_DOC,   0, FTA_OPEN)    \
    X("pptx",     "PowerPoint slides",    H_VIEW,   FTG_DOC,   0, FTA_OPEN)    \
    X("odt",      "OpenDocument text",    H_VIEW,   FTG_DOC,   0, FTA_OPEN)    \
    X("ods",      "OpenDocument sheet",   H_VIEW,   FTG_DOC,   0, FTA_OPEN)    \
    X("rtf",      "Rich text document",   H_VIEW,   FTG_DOC,   0, FTA_OPEN)    \
    X("epub",     "EPUB book",            H_VIEW,   FTG_DOC,   0, FTA_OPEN)    \
    X("mobi",     "Kindle book",          H_VIEW,   FTG_DOC,   0, FTA_OPEN)    \
    /* ---- opaque data ---- */                                                \
    X("db",       "Database",             H_VIEW,   FTG_OTHER, 0, FTA_OPEN)    \
    X("sqlite",   "SQLite database",      H_VIEW,   FTG_OTHER, 0, FTA_OPEN)    \
    X("bin",      "Binary file",          H_VIEW,   FTG_OTHER, 0, FTA_OPEN)    \
    X("dat",      "Data file",            H_VIEW,   FTG_OTHER, 0, FTA_OPEN)    \
    X("dump",     "Dump file",            H_VIEW,   FTG_OTHER, 0, FTA_OPEN)    \
    X("core",     "Core dump",            H_VIEW,   FTG_OTHER, 0, FTA_OPEN)    \
    X("bak",      "Backup file",          H_VIEW,   FTG_OTHER, 0, FTA_OPEN)    \
    X("tmp",      "Temporary file",       H_VIEW,   FTG_OTHER, 0, FTA_OPEN)    \
    X("swp",      "Swap file",            H_VIEW,   FTG_OTHER, 0, FTA_OPEN)    \
    X("pem",      "PEM certificate",      H_EDIT,   FTG_OTHER, 1, FTA_OPEN)    \
    X("crt",      "Certificate",          H_VIEW,   FTG_OTHER, 0, FTA_OPEN)    \
    X("key",      "Key file",             H_VIEW,   FTG_OTHER, 0, FTA_OPEN)    \
    X("pub",      "Public key",           H_EDIT,   FTG_OTHER, 1, FTA_OPEN)    \
    X("sig",      "Signature",            H_VIEW,   FTG_OTHER, 0, FTA_OPEN)    \
    X("gpg",      "GPG data",             H_VIEW,   FTG_OTHER, 0, FTA_OPEN)

#endif /* TOBYOS_FILE_TYPES_H */
