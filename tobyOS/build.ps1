# build.ps1 -- build tobyOS from native PowerShell.
#
# Requires that the MSYS2 UCRT64 toolchain is on PATH:
#   $env:Path = "C:\msys64\ucrt64\bin;" + $env:Path
# (Either set this permanently in System Environment Variables, or
# uncomment the line below.)
#
# $env:Path = "C:\msys64\ucrt64\bin;" + $env:Path

$ErrorActionPreference = "Stop"
Set-Location -Path $PSScriptRoot

$CFLAGS = @(
    "-ffreestanding", "-fno-stack-protector", "-fno-pie", "-fno-pic",
    "-mno-red-zone", "-mno-mmx", "-mno-sse", "-mno-sse2",
    "-nostdlib", "-O2", "-Wall", "-Wextra", "-std=c11"
)

Write-Host "[1/4] assembling boot.asm"
nasm -f elf64 boot/boot.asm -o boot/boot.o

Write-Host "[2/4] assembling long_mode.asm"
nasm -f elf64 boot/long_mode.asm -o boot/long_mode.o

Write-Host "[3/4] compiling kernel.c"
gcc @CFLAGS -c src/kernel.c -o src/kernel.o

Write-Host "[4/4] linking tobyos.bin"
ld -n -nostdlib -T linker.ld -o tobyos.bin boot/boot.o boot/long_mode.o src/kernel.o

Write-Host "OK -> tobyos.bin"
Write-Host "Run with:  qemu-system-x86_64 -kernel .\tobyos.bin"
