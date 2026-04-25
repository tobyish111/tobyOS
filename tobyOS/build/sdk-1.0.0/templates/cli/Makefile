# CLI app template Makefile -- adjust APP and source list as needed.
#
# Quick reference (run `make help` any time):
#   make           -- build the ELF
#   make package   -- build the ELF + produce build/$(APP).tpkg
#   make clean     -- wipe build/
#   make help      -- print this list
#
# Set TOBYOS_SDK to the path of your unpacked SDK before running make:
#   export TOBYOS_SDK=/path/to/your/unpacked/sdk

TOBYOS_SDK ?= /path/to/sdk

APP     := myapp
SRCS    := src/main.c
OBJS    := $(SRCS:src/%.c=build/%.o)
ELF     := build/$(APP).elf
TPKG    := build/$(APP).tpkg

# `help` and `clean` work without an unpacked SDK -- everything else
# needs TOBYOS_SDK to point at a real SDK tree, otherwise the build
# would fail with a confusing "no such file: /path/to/sdk/lib/crt0.o".
# Detect the placeholder up front so the user gets one clear message.
SDK_CHECK_SKIP := $(filter help clean,$(MAKECMDGOALS))
ifeq (,$(SDK_CHECK_SKIP))
ifeq ($(TOBYOS_SDK),/path/to/sdk)
$(error TOBYOS_SDK is not set. Run: export TOBYOS_SDK=/path/to/your/unpacked/sdk \
    (or: make TOBYOS_SDK=/path/to/your/unpacked/sdk))
endif
endif

# Make's built-in default `CC = cc` defeats `?=`, so override only
# when the value is Make's built-in default; env / command-line wins.
ifeq ($(origin CC),default)
CC := clang
endif
ifeq ($(origin LD),default)
LD := ld.lld
endif

PKGBUILD := $(TOBYOS_SDK)/tools/pkgbuild

CFLAGS  := --target=x86_64-elf -ffreestanding -fno-stack-protector \
           -fno-pie -fno-pic -fno-unwind-tables \
           -fno-asynchronous-unwind-tables -mno-red-zone -mno-mmx \
           -mno-sse -mno-sse2 -nostdlib -O2 -Wall -Wextra -std=c11 \
           -isystem $(TOBYOS_SDK)/include

LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
           -T $(TOBYOS_SDK)/lib/program.ld

all: $(ELF)

$(ELF): $(OBJS) $(TOBYOS_SDK)/lib/crt0.o $(TOBYOS_SDK)/lib/libtoby.a | build
	$(LD) $(LDFLAGS) -o $@ $(TOBYOS_SDK)/lib/crt0.o $(OBJS) \
	    $(TOBYOS_SDK)/lib/libtoby.a

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

# `make package` -- one shot from sources to .tpkg ready to install.
package: $(TPKG)

$(TPKG): $(ELF) tobyapp.toml
	$(PKGBUILD) tobyapp.toml -o $@

clean:
	rm -rf build

help:
	@echo "tobyOS SDK CLI app -- $(APP)"
	@echo ""
	@echo "Targets:"
	@echo "  make            build $(ELF)"
	@echo "  make package    build + produce $(TPKG) (ready for pkg install)"
	@echo "  make clean      remove build/"
	@echo "  make help       this list"
	@echo ""
	@echo "Required environment:"
	@echo "  TOBYOS_SDK=/path/to/unpacked/sdk     (currently: $(TOBYOS_SDK))"
	@echo ""
	@echo "Install on a tobyOS instance:"
	@echo "  1. transfer $(TPKG) to /repo/$(APP).tpkg on the target"
	@echo "  2. in the tobyOS shell:  pkg install /repo/$(APP).tpkg"

.PHONY: all clean help package
