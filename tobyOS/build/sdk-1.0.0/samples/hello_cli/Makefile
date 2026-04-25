# hello_cli/Makefile -- build a tobyOS SDK 1.0 sample app out of tree.
#
# Usage from inside this directory:
#   make TOBYOS_SDK=/path/to/sdk
# or set TOBYOS_SDK in the environment first.

TOBYOS_SDK ?= ../..

# `CC ?= clang` doesn't work here -- Make ships with a built-in
# default `CC = cc`, so ?= never fires. Override only when the value
# is Make's built-in default; environment / command-line settings
# still win.
ifeq ($(origin CC),default)
CC := clang
endif
ifeq ($(origin LD),default)
LD := ld.lld
endif

CFLAGS  := --target=x86_64-elf -ffreestanding -fno-stack-protector \
           -fno-pie -fno-pic -fno-unwind-tables \
           -fno-asynchronous-unwind-tables -mno-red-zone -mno-mmx \
           -mno-sse -mno-sse2 -nostdlib -O2 -Wall -Wextra -std=c11 \
           -isystem $(TOBYOS_SDK)/include

LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
           -T $(TOBYOS_SDK)/lib/program.ld

APP      := hello_cli
OBJ      := build/main.o
ELF      := build/$(APP).elf
TPKG     := build/$(APP).tpkg
PKGBUILD := $(TOBYOS_SDK)/tools/pkgbuild

all: $(ELF)

$(ELF): $(OBJ) $(TOBYOS_SDK)/lib/crt0.o $(TOBYOS_SDK)/lib/libtoby.a | build
	$(LD) $(LDFLAGS) -o $@ $(TOBYOS_SDK)/lib/crt0.o $(OBJ) \
	    $(TOBYOS_SDK)/lib/libtoby.a

$(OBJ): src/main.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

package: $(TPKG)

$(TPKG): $(ELF) tobyapp.toml
	$(PKGBUILD) tobyapp.toml -o $@

clean:
	rm -rf build

help:
	@echo "tobyOS SDK sample -- $(APP)"
	@echo ""
	@echo "Targets:"
	@echo "  make            build $(ELF)"
	@echo "  make package    build + produce $(TPKG) (ready for pkg install)"
	@echo "  make clean      remove build/"
	@echo "  make help       this list"
	@echo ""
	@echo "TOBYOS_SDK currently: $(TOBYOS_SDK)"

.PHONY: all clean help package
