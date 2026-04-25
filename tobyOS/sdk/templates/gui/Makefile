# GUI app template Makefile -- links libtoby_gui.a in addition to libtoby.a.
#
# Quick reference (run `make help` any time):
#   make           -- build the ELF
#   make package   -- build the ELF + produce build/$(APP).tpkg
#   make clean     -- wipe build/
#   make help      -- print this list

TOBYOS_SDK ?= /path/to/sdk

APP     := myapp
SRCS    := src/main.c
OBJS    := $(SRCS:src/%.c=build/%.o)
ELF     := build/$(APP).elf
TPKG    := build/$(APP).tpkg

SDK_CHECK_SKIP := $(filter help clean,$(MAKECMDGOALS))
ifeq (,$(SDK_CHECK_SKIP))
ifeq ($(TOBYOS_SDK),/path/to/sdk)
$(error TOBYOS_SDK is not set. Run: export TOBYOS_SDK=/path/to/your/unpacked/sdk \
    (or: make TOBYOS_SDK=/path/to/your/unpacked/sdk))
endif
endif

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

$(ELF): $(OBJS) \
        $(TOBYOS_SDK)/lib/crt0.o \
        $(TOBYOS_SDK)/lib/libtoby_gui.a \
        $(TOBYOS_SDK)/lib/libtoby.a | build
	$(LD) $(LDFLAGS) -o $@ $(TOBYOS_SDK)/lib/crt0.o $(OBJS) \
	    $(TOBYOS_SDK)/lib/libtoby_gui.a \
	    $(TOBYOS_SDK)/lib/libtoby.a

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

package: $(TPKG)

$(TPKG): $(ELF) tobyapp.toml
	$(PKGBUILD) tobyapp.toml -o $@

clean:
	rm -rf build

help:
	@echo "tobyOS SDK GUI app -- $(APP)"
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
	@echo "  3. click [Apps] -> '$(APP)' to launch"

.PHONY: all clean help package
