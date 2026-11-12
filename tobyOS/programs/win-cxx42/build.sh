#!/bin/bash
# build.sh VCVARS OUTDIR -- W7k: win-cxx42.exe built by Microsoft's cl.exe
# (/EHsc /MD), and beside it the same toolset's redistributable msvcp140.dll,
# vcruntime140.dll and vcruntime140_1.dll, copied from its Redist folder
# (%VCToolsRedistDir%, set by vcvars64.bat) -- Microsoft's files, so they are
# copied at build time and never committed (.gitignore: *.dll). VCVARS is the
# Windows path of vcvars64.bat. The Makefile runs this only where that file
# exists (programs/win-exc39/build.sh's twin).
set -e
vcvars=$1
out=$2
[ -f "$vcvars" ] || { echo "build.sh: no $vcvars" >&2; exit 1; }
mkdir -p "$out"
src=$(cygpath -w "$(cd "$(dirname "$0")" && pwd)/cxx42.cpp")
wout=$(cygpath -w "$(cd "$out" && pwd)")
wvc=$(cygpath -w "$vcvars")
bat="$out/build42.bat"
cat > "$bat" <<EOF
@echo off
rem a make recipe may hand no TMP: cl and link's temporary files go beside the output
set "TMP=$wout"
set "TEMP=$wout"
set "PATH=%PATH%;C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer"
call "$wvc" >nul
if errorlevel 1 exit /b 1
cd /d "$wout"
cl /nologo /EHsc /MD /O1 /W3 /Fe:win-cxx42.exe "$src" /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1
set found=
for /d %%d in ("%VCToolsRedistDir%x64\\Microsoft.VC*.CRT") do (
    copy /y "%%d\\msvcp140.dll" . >nul
    copy /y "%%d\\vcruntime140.dll" . >nul
    copy /y "%%d\\vcruntime140_1.dll" . >nul
    set found=1
)
if not defined found exit /b 2
exit /b 0
EOF
# cmd.exe needs the Windows environment vcvars64.bat reads; a make recipe
# may have been handed only a few variables (MSYS2's crossing)
export SystemRoot="${SystemRoot:-C:\\Windows}" windir="${windir:-C:\\Windows}"
export ProgramFiles="${ProgramFiles:-C:\\Program Files}"
export "ProgramFiles(x86)=C:\\Program Files (x86)" 2>/dev/null || true
export COMSPEC="${COMSPEC:-C:\\Windows\\System32\\cmd.exe}"
# cmd.exe itself, by its path: MSYS2's /usr/bin/cmd is a wrapper that runs
# $COMSPEC, which such a recipe may not have
/c/Windows/System32/cmd.exe //c "$(cygpath -w "$bat")"
rm -f "$bat" "$out/cxx42.obj"
for f in win-cxx42.exe msvcp140.dll vcruntime140.dll vcruntime140_1.dll; do
    [ -f "$out/$f" ] || { echo "build.sh: $f was not made" >&2; exit 1; }
done
