# tobyOS -- 64-bit bare-metal kernel (Limine boot)
#
# One-time setup (MSYS2 UCRT64 shell):
#   pacman -S mingw-w64-ucrt-x86_64-xorriso git
#
# First build will auto-clone the Limine binary release.
# After that, just: make

CC   := clang
LD   := ld.lld
AR   := ar
QEMU := "/c/Program Files/qemu/qemu-system-x86_64.exe"

# QEMU serial / debugcon / trace logs (see `make run*` targets).
LOGDIR := logs

# OVMF firmware blob shipped with the QEMU-for-Windows installer.
# Used by `make run-uefi` / `run-installed-uefi` to boot the same
# image under a real UEFI implementation. The CODE.fd is read-only;
# we keep a writable VARS.fd next to it so the firmware has somewhere
# to stash NVRAM (boot order, etc.) between runs. Override OVMF_SRC
# on the command line if your install lives elsewhere:
#   make run-uefi OVMF_SRC=/usr/share/OVMF/OVMF_CODE.fd
#
# Note: x86 `-bios` only accepts ROM blobs <= 256 KiB, but OVMF is
# multi-MB, so we MUST load it via -drive if=pflash. We stage local
# copies of CODE.fd + VARS.fd under build/ so the original install
# isn't touched (vars get written on every run).
OVMF_SRC      ?= /c/Program Files/qemu/share/edk2-x86_64-code.fd
OVMF_CODE     := build/ovmf-code.fd
OVMF_VARS     := build/ovmf-vars.fd
OVMF_VARS_SZ  := 540672

# Header dependency tracking. -MMD writes a `.d` beside every object listing
# the headers that object actually included; -MP adds a phony target for each
# of those headers so DELETING one does not wedge the build with "No rule to
# make target". The generated files are pulled in by the `-include` near the
# bottom of this file.
#
# Why this exists: before it, `src/%.o` depended on $(HEADERS), which is
# `include/tobyos/*.h` -- ONE directory level. `include/tobyos/abi/abi.h` (the
# entire Linux ABI) was not in that glob, and libtoby/ and programs/ had no
# header prerequisites AT ALL. That is the mechanical cause of the two rules
# this tree carried by hand: "struct proc grows => make clean" and "struct
# tk_window grew => touch all program sources". Both are now the build's job.
#
# The $(HEADERS) prerequisite is deliberately KEPT below: on the first build
# after this change no .d files exist yet, and stale objects from before it
# would otherwise never notice a header edit. It costs a full kernel rebuild
# when a top-level header is touched -- exactly the old behaviour, no worse.
DEPFLAGS := -MMD -MP

CFLAGS := --target=x86_64-elf \
          -Iinclude \
          -Ithird_party/monocypher-4.0.2/src \
          -Ithird_party/monocypher-4.0.2/src/optional \
          -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
          -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
          -mcmodel=kernel \
          -nostdlib -O2 -Wall -Wextra -std=c11 $(DEPFLAGS)

# Faster default boot: shorter DHCP budget, quicker ARP pre-warm, and
# no inline example.com DNS+TCP+HTTP smoke in kmain (that alone can add
# many seconds when offline).  For the old behaviour: `make fullboot`.
#
# Default builds also add -DQUICK_BOOT so kmain skips
# always-on harness spawns (devtest_boot_run, M26A/M27A, M28–M35 circles).
EXTRA_CFLAGS ?= -DFAST_BOOT -DQUICK_BOOT

LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -T linker.ld

KERNEL      := tobyos.bin
ISO         := tobyOS.iso
BASE_ISO    := build/base.iso
INSTALL_IMG := build/install.img
LIMINE_DIR  := limine
LIMINE_REPO := https://github.com/limine-bootloader/limine.git
LIMINE_TAG  := v11.x-binary
LIMINE_FILES := $(LIMINE_DIR)/limine \
                $(LIMINE_DIR)/limine-bios.sys \
                $(LIMINE_DIR)/limine-bios-cd.bin \
                $(LIMINE_DIR)/limine-uefi-cd.bin \
                $(LIMINE_DIR)/BOOTX64.EFI

# Milestone 20: reserved boot region on an installed disk, in bytes.
# MUST match INSTALLER_BOOT_BYTES in include/tobyos/installer.h. The
# installer flashes install.img into this region and then formats a
# tobyfs "/data" partition immediately after it.
INSTALL_BOOT_BYTES := 4194304
# Total size of an installable disk image (8 MiB = 4 MiB boot region
# + 4 MiB tobyfs region). The mkfs tool only formats 4 MiB; the
# remaining 4 MiB are padding the kernel ignores until the installer
# stamps tobyfs there.
DISK_IMG_BYTES     := 16777216

# Source files -- add new .c files here and they'll be picked up by the
# pattern rule below.
C_SRCS := src/kernel.c \
          src/klibc.c \
          src/serial.c \
          src/font8x8.c \
          src/font8x16.c \
          src/kfont.c \
          src/kmath.c \
          src/console.c \
          src/printk.c \
          src/bootlog.c \
          src/panic.c \
          src/gdt.c \
          src/idt.c \
          src/isr.c \
          src/pic.c \
          src/ioapic.c \
          src/irq.c \
          src/pit.c \
          src/rtc.c \
          src/keyboard.c \
          src/pmm.c \
          src/vmm.c \
          src/heap.c \
          src/shell.c \
          src/elf.c \
          src/tss.c \
          src/syscall.c \
          src/acpi.c \
          src/apic.c \
          src/smp.c \
          src/vfs.c \
          src/ramfs.c \
          src/tmpfs.c \
          src/ptrace.c \
          src/initrd.c \
          src/proc.c \
          src/thread.c \
          src/mmap.c \
          src/fork.c \
          src/shm.c \
          src/nsproxy.c \
          src/pid_ns.c \
          src/user_ns.c \
          src/net_ns.c \
          src/veth.c \
          src/bridge.c \
          src/nat.c \
          src/rtnetlink.c \
          src/seccomp.c \
          src/cgroup.c \
          src/xserver.c \
          src/unix_socket.c \
          src/sysfs.c \
          src/aslr.c \
          src/hardening.c \
          src/aml_interp.c \
          src/wifi.c \
          src/nic_igb.c \
          src/nic_rtl8168.c \
          src/ahci_quirks.c \
          src/input.c \
          src/audio_mixer.c \
          src/usb_ehci.c \
          src/bluetooth.c \
          src/pe_loader.c \
          src/hidpi.c \
          src/tcp_user.c \
          src/sched.c \
          src/blk.c \
          src/blk_ata.c \
          src/blk_ahci.c \
          src/blk_nvme.c \
          src/blk_offset.c \
          src/partition.c \
          src/provision.c \
          src/tobyfs.c \
          src/fat32.c \
          src/ext4.c \
          src/ext2.c \
          src/usb_msc.c \
          src/file.c \
          src/flock.c \
          src/ptimer.c \
          src/xattr.c \
          src/pipe.c \
          src/signal.c \
          src/pci.c \
          src/e1000.c \
          src/e1000e.c \
          src/virtio_net.c \
          src/virtio_rng.c \
          src/rtl8169.c \
          src/usb_legacy.c \
          src/xhci.c \
          src/usb_hid.c \
          src/usb_hub.c \
          src/virtio_gpu.c \
          src/linux_drm.c \
          src/net.c \
          src/eth.c \
          src/arp.c \
          src/ip.c \
          src/ip_frag.c \
          src/icmp.c \
          src/ipv6.c \
          src/icmpv6.c \
          src/dhcpv6.c \
          src/udp.c \
          src/dhcp.c \
          src/dns.c \
          src/tcp.c \
          src/tcp_echo.c \
          src/tcp_shell.c \
          src/ssh.c \
          src/ssh_crypto.c \
          src/http.c \
          src/http_async.c \
          src/ws.c \
          src/puff.c \
          src/brotli.c \
          src/woff2.c \
          src/socket.c \
          src/gfx.c \
          src/mouse.c \
          src/gui.c \
          src/term.c \
          src/settings.c \
          src/service.c \
          src/session.c \
          src/users.c \
          src/pkg.c \
          src/cap.c \
          src/perf.c \
          src/sysmon.c \
          src/installer.c \
          src/audio_hda.c \
          src/snd_pcm.c \
          src/fw_cfg.c \
          src/acpi_bat.c \
          src/devtest.c \
          src/hotplug.c \
          src/display.c \
          src/slog.c \
          src/watchdog.c \
          src/safemode.c \
          src/hwinfo.c \
          src/drvmatch.c \
          src/theme.c \
          src/notify.c \
          src/rng.c \
          src/sec.c \
          src/sysprot.c \
          src/sectest.c \
          src/drvdb.c \
          src/drvconf.c \
          src/virtio_blk.c \
          src/usbreg.c \
          src/hwdb.c \
          src/m35selftest.c \
          src/tls.c \
          src/tls13.c \
          src/quic_crypto.c \
          src/quic_packet.c \
          src/quic_conn.c \
          src/http3.c \
          src/tls_x509.c \
          src/tls_trust.c \
          src/procfs.c \
          src/tty.c \
          src/pty.c \
          src/inotify.c \
          src/clipboard.c \
          src/smb.c \
          src/http2.c \
          src/ntp.c \
          src/mdns.c \
          src/audio_engine.c \
          src/video.c \
          src/cryptfs.c \
          src/secureboot.c \
          src/taskd.c \
          src/slog_rotate.c \
          src/devmgr.c \
          src/print.c \
          src/a11y.c \
          src/unicode.c \
          src/i18n.c \
          src/registry.c \
          src/gpu_drm.c \
          src/gpu_cursor.c \
          src/notify_svc.c \
          src/msgbus.c \
          src/acpi_sleep.c \
          src/wifi_mgr.c \
          src/firewall.c \
          src/wireguard.c \
          src/pnp.c \
          src/usb_hotplug.c \
          src/snapshot.c \
          src/crashdump.c \
          src/user.c \
          src/page_fault.c \
          src/lz4.c \
          src/zram.c \
          src/swap.c \
          src/oom.c \
          third_party/monocypher-4.0.2/src/monocypher.c \
          third_party/monocypher-4.0.2/src/optional/monocypher-ed25519.c \
          src/wm.c \
          src/stacktrace.c \
          src/selftest.c \
          src/kgdb.c \
          src/heap_debug.c \
          src/svc_mgr.c \
          src/gpu_intel_modeset.c \
          src/compositor_accel.c \
          src/driver_autoload.c \
          src/vfs_perms.c \
          src/uac.c \
          src/dnd.c \
          src/file_assoc.c \
          src/anim.c \
          src/context_menu.c \
          src/system_sounds.c \
          src/media_session.c \
          src/autoupdate.c \
          src/startup_mgr.c \
          src/crash_recovery.c \
          src/sys_restore.c \
          src/health_monitor.c \
          src/vfs_helpers.c \
          src/bcache.c \
          src/module.c

S_SRCS := src/isr_stubs.S \
          src/syscall_entry.S \
          src/ap_trampoline.S \
          src/proc_switch.S

# stage 13H: BearSSL X.509 chain validation (RSA + ECDSA) for TLS cert
# verification. The decode/verify-only subset of the vendored reference
# (third_party/bearssl); trust anchors live in src/tls_trust.c.
BEARSSL_SRCS := \
          third_party/bearssl/src/codec/ccopy.c \
          third_party/bearssl/src/codec/dec16be.c \
          third_party/bearssl/src/codec/dec32be.c \
          third_party/bearssl/src/codec/dec32le.c \
          third_party/bearssl/src/codec/dec64be.c \
          third_party/bearssl/src/codec/enc16be.c \
          third_party/bearssl/src/codec/enc32be.c \
          third_party/bearssl/src/codec/enc32le.c \
          third_party/bearssl/src/codec/enc64be.c \
          third_party/bearssl/src/codec/pemdec.c \
          third_party/bearssl/src/hash/dig_oid.c \
          third_party/bearssl/src/hash/dig_size.c \
          third_party/bearssl/src/hash/md5.c \
          third_party/bearssl/src/hash/multihash.c \
          third_party/bearssl/src/hash/sha1.c \
          third_party/bearssl/src/hash/sha2big.c \
          third_party/bearssl/src/hash/sha2small.c \
          third_party/bearssl/src/hash/ghash_ctmul.c \
          third_party/bearssl/src/hash/mgf1.c \
          third_party/bearssl/src/symcipher/aes_ct.c \
          third_party/bearssl/src/symcipher/aes_ct_enc.c \
          third_party/bearssl/src/symcipher/aes_ct_ctr.c \
          third_party/bearssl/src/aead/gcm.c \
          third_party/bearssl/src/ec/ec_prime_i31.c \
          third_party/bearssl/src/ec/ec_secp256r1.c \
          third_party/bearssl/src/ec/ec_secp384r1.c \
          third_party/bearssl/src/ec/ec_secp521r1.c \
          third_party/bearssl/src/ec/ecdsa_atr.c \
          third_party/bearssl/src/ec/ecdsa_i31_bits.c \
          third_party/bearssl/src/ec/ecdsa_i31_vrfy_asn1.c \
          third_party/bearssl/src/ec/ecdsa_i31_vrfy_raw.c \
          third_party/bearssl/src/rsa/rsa_i31_pkcs1_vrfy.c \
          third_party/bearssl/src/rsa/rsa_i31_pss_vrfy.c \
          third_party/bearssl/src/rsa/rsa_i31_pub.c \
          third_party/bearssl/src/rsa/rsa_pkcs1_sig_unpad.c \
          third_party/bearssl/src/rsa/rsa_pss_sig_unpad.c \
          third_party/bearssl/src/rsa/rsa_ssl_decrypt.c \
          third_party/bearssl/src/int/i31_add.c \
          third_party/bearssl/src/int/i31_bitlen.c \
          third_party/bearssl/src/int/i31_decmod.c \
          third_party/bearssl/src/int/i31_decode.c \
          third_party/bearssl/src/int/i31_decred.c \
          third_party/bearssl/src/int/i31_encode.c \
          third_party/bearssl/src/int/i31_fmont.c \
          third_party/bearssl/src/int/i31_iszero.c \
          third_party/bearssl/src/int/i31_moddiv.c \
          third_party/bearssl/src/int/i31_modpow.c \
          third_party/bearssl/src/int/i31_modpow2.c \
          third_party/bearssl/src/int/i31_montmul.c \
          third_party/bearssl/src/int/i31_mulacc.c \
          third_party/bearssl/src/int/i31_muladd.c \
          third_party/bearssl/src/int/i31_ninv31.c \
          third_party/bearssl/src/int/i31_reduce.c \
          third_party/bearssl/src/int/i31_rshift.c \
          third_party/bearssl/src/int/i31_sub.c \
          third_party/bearssl/src/int/i31_tmont.c \
          third_party/bearssl/src/int/i32_div32.c \
          third_party/bearssl/src/x509/skey_decoder.c \
          third_party/bearssl/src/x509/x509_decoder.c \
          third_party/bearssl/src/x509/x509_minimal.c

OBJS := $(C_SRCS:.c=.o) $(S_SRCS:.S=.o) $(BEARSSL_SRCS:.c=.o)

HEADERS := $(wildcard include/tobyos/*.h)

# ---------- Milestone 25E: ports/sbase/* userland tools (forward decl) ----------
# Full rules live further below. We declare the per-tool ELF list up
# here so PROGRAMS (built with := immediate-expansion) can reference it.
PORTS_SBASE_DIR    := ports/sbase
PORTS_SBASE_TOOLS  := echo cat wc head ls grep \
                      basename dirname sleep tee expr tr cut uniq sort
PORTS_SBASE_ELFS   := $(addprefix $(PORTS_SBASE_DIR)/,$(addsuffix .elf,$(PORTS_SBASE_TOOLS)))

# Programs built and bundled into the boot initrd. Each is a tiny
# static ELF; programs/user_hello/user_hello.elf becomes /bin/hello
# inside the initramfs (see initrd-stage rules below).
PROGRAMS := programs/awk/awk.elf \
            programs/hello/hello.elf \
            programs/user_hello/user_hello.elf \
            programs/user_bad/user_bad.elf \
            programs/user_echo/user_echo.elf \
            programs/user_cat/user_cat.elf \
            programs/user_long/user_long.elf \
            programs/user_netecho/user_netecho.elf \
            programs/user_gui_demo/user_gui_demo.elf \
            programs/x2gui/x2gui.elf \
            programs/user_gui_clock/user_gui_clock.elf \
            programs/user_gui_widgets/user_gui_widgets.elf \
            programs/user_gui_about/user_gui_about.elf \
            programs/user_cpptest/user_cpptest.elf \
            programs/user_oh264test/user_oh264test.elf \
            programs/user_bsdroute/user_bsdroute.elf \
            programs/user_mp4play/user_mp4play.elf \
            programs/user_stltest/user_stltest.elf \
            programs/user_av1test/user_av1test.elf \
            programs/user_aactest/user_aactest.elf \
            programs/user_wasmtest/user_wasmtest.elf \
            programs/user_gui_term/user_gui_term.elf \
            programs/user_gui_files/user_gui_files.elf \
            programs/user_gui_viewer/user_gui_viewer.elf \
            programs/user_gui_browser/user_gui_browser.elf \
            programs/user_login/user_login.elf \
            programs/user_gui_settings/user_gui_settings.elf \
            programs/user_gui_helloapp/user_gui_helloapp.elf \
            programs/user_gui_taskmgr/user_gui_taskmgr.elf \
            programs/user_gui_edit/user_gui_edit.elf \
            programs/user_abi_test/user_abi_test.elf \
            programs/c_hello/c_hello.elf \
            programs/nsetuid/nsetuid.elf \
            programs/c_args/c_args.elf \
            programs/c_filedemo/c_filedemo.elf \
            programs/c_alloctest/c_alloctest.elf \
            programs/c_envtest/c_envtest.elf \
            programs/c_dynhello/c_dynhello.elf \
            programs/devlist/devlist.elf \
            programs/drvtest/drvtest.elf \
            programs/usbtest/usbtest.elf \
            programs/audiotest/audiotest.elf \
            programs/audioplay/audioplay.elf \
            programs/batterytest/batterytest.elf \
            programs/displayinfo/displayinfo.elf \
            programs/drawtest/drawtest.elf \
            programs/rendertest/rendertest.elf \
            programs/fonttest/fonttest.elf \
            programs/logview/logview.elf \
            programs/crashinfo/crashinfo.elf \
            programs/wdogtest/wdogtest.elf \
            programs/safesh/safesh.elf \
            programs/fscheck/fscheck.elf \
            programs/services/services.elf \
            programs/svc_crasher/svc_crasher.elf \
            programs/stabilitytest/stabilitytest.elf \
            programs/hwinfo/hwinfo.elf \
            programs/drvmatch/drvmatch.elf \
            programs/hwcompat/hwcompat.elf \
            programs/hwreport/hwreport.elf \
            programs/compattest/compattest.elf \
            programs/sigtest/sigtest.elf \
            programs/mctest/mctest.elf \
            programs/reclaimtest/reclaimtest.elf \
            programs/tobycc/tobycc.elf \
            programs/tobybuild/tobybuild.elf \
            programs/selfhosttest/selfhosttest.elf \
            programs/user_sh/user_sh.elf \
            programs/user_pkg/user_pkg.elf \
            programs/user_svcctl/user_svcctl.elf \
            programs/user_ps/user_ps.elf \
            programs/user_top/user_top.elf \
            programs/user_cp/user_cp.elf \
            programs/user_mv/user_mv.elf \
            programs/user_rm/user_rm.elf \
            programs/user_find/user_find.elf \
            programs/user_sort/user_sort.elf \
            programs/user_tee/user_tee.elf \
            programs/user_env/user_env.elf \
            programs/user_chmod/user_chmod.elf \
            programs/user_df/user_df.elf \
            programs/user_mkdir/user_mkdir.elf \
            programs/user_touch/user_touch.elf \
            programs/user_kill/user_kill.elf \
            programs/user_tail/user_tail.elf \
            programs/user_uniq/user_uniq.elf \
            programs/user_date/user_date.elf \
            programs/user_wget/user_wget.elf \
            programs/user_nc/user_nc.elf \
            programs/user_ifconfig/user_ifconfig.elf \
            programs/user_nslookup/user_nslookup.elf \
            programs/user_gui_calc/user_gui_calc.elf \
            programs/installer/installer.elf \
            programs/strace/strace.elf \
            programs/debugger/debugger.elf \
            programs/pkgmgr/pkgmgr.elf \
            programs/init/init.elf \
            programs/sh/sh.elf \
            programs/shparity/shparity.elf \
            programs/oilspec/oilspec.elf \
            programs/id/id.elf \
            programs/newgrp/newgrp.elf \
            programs/ttyparity/ttyparity.elf \
            programs/spechelp/spechelp.elf \
            programs/tsh/tsh.elf \
            programs/devmgr/devmgr.elf \
            programs/indexer/indexer.elf \
            programs/updater/updater.elf \
            programs/bluescreen/bluescreen.elf \
            programs/lockscreen/lockscreen.elf \
            programs/mediaplayer/mediaplayer.elf \
            programs/chromewin/chromewin.elf \
            programs/diskmgr/diskmgr.elf \
            programs/eventview/eventview.elf \
            programs/lua/lua.elf \
            programs/make/make.elf \
            programs/less/less.elf \
            programs/curl/curl.elf \
            programs/tcc/tcc.elf \
            programs/as/as.elf \
            programs/ld/ld.elf \
            programs/wifipicker/wifipicker.elf \
            programs/volctrl/volctrl.elf \
            programs/appinstall/appinstall.elf \
            $(PORTS_SBASE_ELFS) \
            $(LDTOBY_SO) $(LIBTOBY_SO)

# Initramfs staging dir + final tar. The dir mirrors the on-FS layout
# the kernel will see at runtime ("/", "/bin", "/etc", ...). The tar
# is what Limine ships as a module.
INITRD_STAGE := build/initrd
INITRD_TAR   := build/initrd.tar

# Bash-parity corpus. Wildcarded on purpose -- this is the ONE place the
# initrd's otherwise-explicit file list uses a glob, because the corpus is
# meant to grow a case at a time and a case that silently failed to ship
# would show up as a PASS. The tar list globs the staged copies to match.
SHPARITY_CASES := $(wildcard initrd/etc/shparity/*.sh)

# ---------- Disk image (tobyfs, milestone 6) ----------
#
# Backed by a 4 MiB raw file passed to QEMU as the IDE primary master.
# The host-side mkfs tool initialises a blank tobyfs image on demand;
# we DO NOT regenerate disk.img automatically on every build, because
# that would wipe whatever the running OS has persisted. Use `make
# wipe-disk` (or just delete disk.img) to start fresh.

DISK_IMG       := disk.img
MKFS_SRC       := tools/mkfs_tobyfs.c
MKFS_BIN       := build/mkfs_tobyfs
MKPKG_SRC      := tools/mkpkg.c
MKPKG_BIN      := build/mkpkg
MKDISK_GPT_SRC := tools/mkdisk_gpt.c
MKDISK_GPT_BIN := build/mkdisk_gpt
GPT_DISK_IMG   := build/disk_gpt.img
HOST_CC        := gcc
HOST_CFLAGS    := -O2 -Wall -Wextra -std=c11

# Milestone-16 demo package: built from the user_gui_helloapp ELF +
# a little README, wrapped by the host-side mkpkg tool, then shipped
# inside the initrd at /repo/helloapp.tpkg. `pkg install helloapp`
# finds it via the /repo fallback search path.
#
# Milestone 17 also ships a SECOND, newer copy of the same package as
# /repo/helloapp_v2.tpkg (VERSION 1.1.0) so that, after the user
# `pkg install helloapp` (1.0.0), `pkg update` sees an upgrade
# available and `pkg upgrade` can demonstrably bump the install.
HELLOAPP_TPKG       := build/helloapp.tpkg
HELLOAPP_TPKG_V2    := build/helloapp_v2.tpkg
HELLOAPP_README     := build/helloapp.txt
HELLOAPP_README_V2  := build/helloapp_v2.txt

# M34A integrity-fixture .tpkg variants. Variable defs MUST live up
# here (alongside HELLOAPP_TPKG) because $(INITRD_TAR)'s prereq list
# expands them at parse time -- a definition further down would expand
# to empty and the cp recipe in the initrd rule would fail at "No such
# file". The actual build rules can stay near the rest of the M34
# fixture block further down.
HELLOAPP_TPKG_CORRUPT  := build/helloapp_corrupt.tpkg
HELLOAPP_TPKG_BADHASH  := build/helloapp_badhash.tpkg
HELLOAPP_TPKG_NOHASH   := build/helloapp_nohash.tpkg
HELLOAPP_TPKG_BADFHASH := build/helloapp_badfhash.tpkg

# M34B update-verification fixture. NAME=helloapp VERSION=1.0.0 with
# --corrupt-body N so the body's HASH no longer matches; we ship it
# as a separate file (NOT auto-picked by repo_find_latest because the
# version equals the v1.0.0 already in /repo) and the M34B self-test
# drives it via pkg_upgrade_path() to exercise the upgrade integrity
# gate + rollback preservation.
HELLOAPP_TPKG_V2_CORRUPT := build/helloapp_v2_corrupt.tpkg

# M34C signing fixtures + supporting test material:
#   TEST_KEY_BIN   -- deterministic 32-byte HMAC-SHA256 key (host file
#                     consumed by mkpkg --sign-key-file).
#   TRUST_DB       -- /system/keys/trust.db shipped inside the initrd;
#                     consulted by sig_trust_store_init at boot.
#   _SIGNED        -- helloapp signed with the trusted key.
#   _SIG_UNKNOWN   -- signed with the same KEY BYTES but tagged with
#                     id "rogue" (absent from trust.db -> reject).
#   _SIG_TAMPERED  -- signed (no HASH) AND --corrupt-body so the SIG
#                     check fires on a body that the in-band HASH
#                     wouldn't catch.
TEST_KEY_BIN              := build/keys/test_key.bin
TEST_KEY_ID               := tobyOS-test
TRUST_DB                  := build/system/keys/trust.db
HELLOAPP_TPKG_SIGNED      := build/helloapp_signed.tpkg
HELLOAPP_TPKG_SIG_UNKNOWN := build/helloapp_signed_unknown.tpkg
HELLOAPP_TPKG_SIG_TAMPER  := build/helloapp_signed_tampered.tpkg

# M34D capability fixtures.
#   _CAPS_RO    -- declares "FILE_READ,GUI" only. The selftest reads
#                  this manifest back from the install record, parses
#                  the CAPS line, and confirms cap_apply_declared
#                  narrows a synthetic proc to exactly those bits.
#   _CAPS_NONE  -- declares an empty CAPS list (no CAPS line emitted)
#                  -- this is the back-compat / legacy app fixture.
HELLOAPP_TPKG_CAPS_RO     := build/helloapp_caps_ro.tpkg

# M32 SDK variable declarations need to live up here, BEFORE any rule
# (such as $(INITRD_TAR) below) references them in a prerequisite list.
# Make expands prereqs at parse time, so a definition further down the
# Makefile would expand to empty at line-read and the initrd recipe
# would silently `cp` non-existent files. The SDK *rules* (sdk, sdk-pack,
# pattern rule for build/samples/%.tpkg, ...) live in the dedicated SDK
# section near the bottom -- they're parsed late but executed correctly
# because Make completes a full Makefile read before running anything.
SDK_VERSION         := 1.0.0
SDK_STAGE           := build/sdk-$(SDK_VERSION)
SDK_TARBALL         := build/tobyOS-sdk-$(SDK_VERSION).tar.gz
PKGBUILD_SRC        := tools/pkgbuild.c
PKGBUILD_BIN        := build/pkgbuild
SDK_SAMPLES         := hello_cli hello_gui notes_app
SDK_SAMPLE_TPKGS    := $(addprefix build/samples/,$(addsuffix .tpkg,$(SDK_SAMPLES)))

.DEFAULT_GOAL := all

.PHONY: help all build iso rebuild qemu run run-ssh run-uefi run-ahci run-ahci-uefi run-nvme run-nvme-uefi \
        run-virtio-net run-virtio-net-uefi run-e1000e run-e1000e-uefi \
        run-rtl8139 run-rtl8139-uefi \
        run-xhci run-xhci-uefi \
        run-xhci-usb run-xhci-usb-uefi run-xhci-hub run-gpt-usb usb-stick-img \
        run-virtio-gpu run-virtio-gpu-uefi run-virtio-gpu-gl \
        run-installed run-installed-uefi clean inspect \
        programs initrd disk wipe-disk mkfs mkpkg helloapp-tpkg \
        helloapp-tpkg-v2 sdk sdk-tree sdk-headers sdk-libs sdk-tools \
        sdk-samples sdk-pack pkgbuild \
        m17test m20test m22shutdowntest m24dtest m34test m35test \
        base-iso install-img \
        install-target usb-image \
        mkdisk-gpt gpt-disk run-gpt run-gpt-uefi run-gpt-nvme

help:
	@echo "tobyOS make commands"
	@echo ""
	@echo "Build:"
	@echo "  make              Build the ISO and persistent disk image"
	@echo "  make build        Same as make"
	@echo "  make iso          Build only tobyOS.iso"
	@echo "  make quickiso     Clean rebuild with FAST_BOOT + QUICK_BOOT"
	@echo "  make fullboot     Rebuild without FAST_BOOT"
	@echo "  make rebuild      Clean, then build"
	@echo ""
	@echo "Run:"
	@echo "  make run          Boot ISO in QEMU with IDE disk"
	@echo "  make qemu         Same as make run"
	@echo "  make run-ssh      Boot QEMU with SSH on localhost:2222 + virtio-rng"
	@echo "  make run-uefi     Boot ISO in QEMU with UEFI firmware"
	@echo "  make run-installed       Boot the installed disk image"
	@echo "  make run-installed-uefi  Boot installed disk image with UEFI"
	@echo ""
	@echo "Images and tools:"
	@echo "  make clean        Remove build outputs; preserves disk.img"
	@echo "  make wipe-disk    Reformat disk.img"
	@echo "  make usb-image    Copy hybrid ISO to tobyOS-usb.img"
	@echo "  make inspect      Show kernel ELF headers"
	@echo "  make sdk-pack     Build the SDK tarball"
	@echo ""
	@echo "Tests:"
	@echo "  make m17test m20test m22shutdowntest m24dtest m34test m35test m36test sectest"
	@echo ""
	@echo "Hardware variants:"
	@echo "  make run-ahci run-nvme run-gpt run-xhci run-virtio-gpu run-virtio-net"
	@echo "  make run-ahci-uefi run-nvme-uefi run-gpt-uefi run-xhci-uefi"

all: $(ISO) $(DISK_IMG)

build: all

iso: $(ISO)

rebuild:
	$(MAKE) clean
	$(MAKE) all

qemu: run

programs: $(PROGRAMS)

initrd: $(INITRD_TAR)

base-iso: $(BASE_ISO)
install-img: $(INSTALL_IMG)

# ---------- Compile ----------

src/%.o: src/%.c $(HEADERS) Makefile
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

# C14b: kfont.c runs stb_truetype's floating-point math, so it must be built
# with SSE (overriding the kernel's default -mno-sse). The FP usage is bracketed
# by an FXSAVE/FXRSTOR guard (kfpu_begin/end) so the calling user process's live
# SSE state is preserved. -Wno-* silence the vendored single-header.
src/kfont.o: src/kfont.c $(HEADERS) third_party/stb_truetype.h Makefile
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -msse -msse2 -Wno-unused-function \
	      -Wno-sign-compare -Wno-cast-align -c $< -o $@

# C20: kmath.c is the kernel-side libm for the Win32 float-return shims; it runs
# real FP, so (like kfont.c) it is built -msse2 with an FXSAVE/FXRSTOR guard
# (kmfpu_begin/end) preserving the calling user process's live SSE state.
src/kmath.o: src/kmath.c $(HEADERS) Makefile
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -msse -msse2 -fno-math-errno \
	      -Wno-unused-function -c $< -o $@

# stage 13G: src/brotli.c amalgamates the vendored brotli v1.0.9 decoder
# (third_party/brotli). It needs the brotli public headers (<brotli/...>)
# and the freestanding <string.h>/<stdlib.h> shims on its include path.
# The shim dir comes FIRST so its string.h/stdlib.h win over any hosted
# headers. -Wno-* silence the vendored reference under -Wall -Wextra.
BROTLI_INC := -Ithird_party/brotli/shim -Ithird_party/brotli/include
src/brotli.o: src/brotli.c $(HEADERS) $(wildcard third_party/brotli/*/*.c) \
              $(wildcard third_party/brotli/*/*.h) Makefile
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(BROTLI_INC) \
	      -Wno-unused-function -Wno-sign-compare -Wno-unused-parameter \
	      -Wno-cast-align -c $< -o $@

src/%.o: src/%.S $(HEADERS) Makefile
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

third_party/monocypher-4.0.2/src/%.o: third_party/monocypher-4.0.2/src/%.c Makefile
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

third_party/monocypher-4.0.2/src/optional/%.o: third_party/monocypher-4.0.2/src/optional/%.c Makefile
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

# stage 13H: BearSSL X.509 objects. Needs its public headers + the
# freestanding <string.h> shim (shim first); -w silences the vendored
# reference under -Wall -Wextra.
BEARSSL_INC := -Ithird_party/bearssl/shim -Ithird_party/bearssl/inc -Ithird_party/bearssl/src
third_party/bearssl/src/%.o: third_party/bearssl/src/%.c Makefile
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(BEARSSL_INC) -w -c $< -o $@

# The trust-anchor array pulls in br_x509_trust_anchor from bearssl.h.
# test_ca_anchor.inc (the tobyOS test CA, tests/quic-ca/) is only
# compiled in under -DTLS_TEST_CA; it's a prerequisite so regenerating
# it rebuilds either way.
src/tls_trust.o: src/tls_trust.c $(HEADERS) third_party/bearssl/trust_anchors.inc third_party/bearssl/test_ca_anchor.inc Makefile
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(BEARSSL_INC) -w -c $< -o $@

# tls.c calls the BearSSL X.509 API, so it needs the public headers +
# the freestanding <string.h> shim they pull (shim first).
src/tls.o: src/tls.c $(HEADERS) Makefile
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -Ithird_party/bearssl/shim -Ithird_party/bearssl/inc -c $< -o $@

# ws.c uses BearSSL's SHA-1 for the Sec-WebSocket-Accept check, so it
# needs the same headers + shim.
src/ws.o: src/ws.c $(HEADERS) Makefile
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -Ithird_party/bearssl/shim -Ithird_party/bearssl/inc -c $< -o $@

# quic_crypto.c uses BearSSL AES-128-GCM (block/aead) for QUIC packet +
# header protection, so it needs the BearSSL headers + <string.h> shim.
src/quic_crypto.o: src/quic_crypto.c $(HEADERS) Makefile
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -Ithird_party/bearssl/shim -Ithird_party/bearssl/inc -c $< -o $@

# tls_x509.c (shared cert validation) + quic_conn.c (which now calls it
# for QUIC cert auth) use the BearSSL X.509 API -> same headers + shim.
src/tls_x509.o: src/tls_x509.c $(HEADERS) Makefile
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -Ithird_party/bearssl/shim -Ithird_party/bearssl/inc -c $< -o $@

src/quic_conn.o: src/quic_conn.c $(HEADERS) Makefile
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -Ithird_party/bearssl/shim -Ithird_party/bearssl/inc -c $< -o $@

# ---------- Link ----------

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# ---------- Clone Limine binary release ----------

$(LIMINE_FILES):
	rm -rf $(LIMINE_DIR)
	git clone $(LIMINE_REPO) --branch=$(LIMINE_TAG) --depth=1 $(LIMINE_DIR)

# ---------- Kernel-mode programs (loaded via Limine modules) ----------
#
# Each program is a tiny static ELF64 built with the same toolchain +
# CFLAGS as the kernel and linked at a fixed virtual address by its own
# program.ld. The ELF loader (src/elf.c) consumes them at runtime; the
# `run` shell command kicks them off.

PROG_LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000

# User-mode program flags: same freestanding base as the kernel, but
# DROP -mcmodel=kernel because the program is linked at 0x400000 (low
# half) and small code model is the right default. Everything else
# (no SSE, no red zone, no PIE) carries over.
USER_CFLAGS := --target=x86_64-elf \
               -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
               -fno-unwind-tables -fno-asynchronous-unwind-tables \
               -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
               -nostdlib -O2 -Wall -Wextra -std=c11 $(DEPFLAGS)

programs/hello/hello.o: programs/hello/main.c
	$(CC) $(CFLAGS) -c $< -o $@

programs/hello/hello.elf: programs/hello/hello.o programs/hello/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/hello/program.ld -o $@ $<

programs/user_hello/user_hello.o: programs/user_hello/main.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

programs/user_hello/user_hello.elf: programs/user_hello/user_hello.o \
                                    programs/user_hello/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/user_hello/program.ld -o $@ $<

programs/user_bad/user_bad.o: programs/user_bad/main.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

programs/user_bad/user_bad.elf: programs/user_bad/user_bad.o \
                                programs/user_bad/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/user_bad/program.ld -o $@ $<

# Shared user-mode entry trampoline -- reads argc/argv off the stack and
# calls main(). Linked into every program that takes argv (echo, cat).
programs/common/start.o: programs/common/start.S
	$(CC) $(USER_CFLAGS) -c $< -o $@

programs/user_echo/user_echo.o: programs/user_echo/main.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

programs/user_echo/user_echo.elf: programs/user_echo/user_echo.o \
                                  programs/common/start.o \
                                  programs/user_echo/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/user_echo/program.ld -o $@ \
	      programs/common/start.o programs/user_echo/user_echo.o

programs/user_cat/user_cat.o: programs/user_cat/main.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

programs/user_cat/user_cat.elf: programs/user_cat/user_cat.o \
                                programs/common/start.o \
                                programs/user_cat/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/user_cat/program.ld -o $@ \
	      programs/common/start.o programs/user_cat/user_cat.o

programs/user_long/user_long.o: programs/user_long/main.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

programs/user_long/user_long.elf: programs/user_long/user_long.o \
                                  programs/common/start.o \
                                  programs/user_long/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/user_long/program.ld -o $@ \
	      programs/common/start.o programs/user_long/user_long.o

# Milestone 25A: ABI surface test. Lives in /bin/abi_test on the
# initrd; running it from the shell verifies every M25A syscall
# end-to-end on real hardware (no libc, no shared object, just
# inline-asm syscall trampolines).
programs/user_abi_test/user_abi_test.o: programs/user_abi_test/main.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

programs/user_abi_test/user_abi_test.elf: programs/user_abi_test/user_abi_test.o \
                                          programs/common/start.o \
                                          programs/user_abi_test/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/user_abi_test/program.ld -o $@ \
	      programs/common/start.o programs/user_abi_test/user_abi_test.o

# ---------- Milestone 25B: libtoby (the minimal libc) ----------
#
# libtoby is a static archive (libtoby.a) plus a CRT entry (crt0.o).
# Every libtoby-linked C user program builds with LIBTOBY_CFLAGS so
# the public headers under libtoby/include/ shadow whatever the host
# toolchain provides, and links against libtoby/libtoby.a + crt0.o.
#
# The implementation files DO need <tobyos/abi/abi.h> from the kernel
# tree (so the syscall numbers stay in lockstep), so we point -I at
# the kernel include dir for libtoby's own .c files. User programs
# get -isystem ONLY at libtoby/include so they see the POSIX-shape
# headers and not the kernel internals.

LIBTOBY_DIR     := libtoby
LIBTOBY_INC     := $(LIBTOBY_DIR)/include
LIBTOBY_SRC     := $(LIBTOBY_DIR)/src
LIBTOBY_AR      := $(LIBTOBY_DIR)/libtoby.a
LIBTOBY_CRT0    := $(LIBTOBY_DIR)/crt0.o

LIBTOBY_C_SRCS  := $(LIBTOBY_SRC)/init.c \
                   $(LIBTOBY_SRC)/int128.c \
                   $(LIBTOBY_SRC)/syscall.c \
                   $(LIBTOBY_SRC)/errno.c \
                   $(LIBTOBY_SRC)/string.c \
                   $(LIBTOBY_SRC)/stdlib.c \
                   $(LIBTOBY_SRC)/unistd.c \
                   $(LIBTOBY_SRC)/process.c \
                   $(LIBTOBY_SRC)/time.c \
                   $(LIBTOBY_SRC)/assert.c \
                   $(LIBTOBY_SRC)/stdio.c \
                   $(LIBTOBY_SRC)/dirent.c \
                   $(LIBTOBY_SRC)/getopt.c \
                   $(LIBTOBY_SRC)/devtest.c \
                   $(LIBTOBY_SRC)/slog.c \
                   $(LIBTOBY_SRC)/wdog.c \
                   $(LIBTOBY_SRC)/fscheck.c \
                   $(LIBTOBY_SRC)/svc.c \
                   $(LIBTOBY_SRC)/stab.c \
                   $(LIBTOBY_SRC)/hwinfo.c \
                   $(LIBTOBY_SRC)/notify.c \
                   $(LIBTOBY_SRC)/gl.c \
                   $(LIBTOBY_SRC)/gl_raster.c \
                   $(LIBTOBY_SRC)/pthread.c \
                   $(LIBTOBY_SRC)/font.c \
                   $(LIBTOBY_SRC)/emoji.c \
                   $(LIBTOBY_SRC)/posix_compat.c \
                   $(LIBTOBY_SRC)/signal.c \
                   $(LIBTOBY_SRC)/dlfcn.c \
                   $(LIBTOBY_SRC)/net.c \
                   $(LIBTOBY_SRC)/youtube.c \
                   $(LIBTOBY_SRC)/paint.c \
                   $(LIBTOBY_SRC)/image.c \
                   $(LIBTOBY_SRC)/audio.c \
                   $(LIBTOBY_SRC)/html_parser.c \
                   $(LIBTOBY_SRC)/css_parser.c \
                   $(LIBTOBY_SRC)/layout.c \
                   $(LIBTOBY_SRC)/mp3_decode.c \
                   $(LIBTOBY_SRC)/h264_decode.c \
                   $(LIBTOBY_SRC)/media.c \
                   $(LIBTOBY_SRC)/fileassoc.c \
                   $(LIBTOBY_SRC)/render.c \
                   $(LIBTOBY_SRC)/clipboard_ext.c \
                   $(LIBTOBY_SRC)/unicode.c \
                   $(LIBTOBY_SRC)/math.c \
                   $(LIBTOBY_SRC)/regex.c \
                   $(LIBTOBY_SRC)/termios.c \
                   $(LIBTOBY_SRC)/poll.c \
                   $(LIBTOBY_SRC)/mman.c \
                   $(LIBTOBY_SRC)/config.c \
                   $(LIBTOBY_SRC)/wasm_bridge.c \
                   $(LIBTOBY_SRC)/tk.c
# M29B SYS_DRVMATCH ships in libtoby/src/hwinfo.c -- no new translation
# unit. The list above is intentionally non-exhaustive of headers; the
# hwinfo.c TU now also exports tobyhw_drvmatch.
#
# M32 SDK: notify.c wraps SYS_NOTIFY_POST/LIST/DISMISS as POSIX-style
# errno+(-1) calls. The same TU is mirrored into the SDK as part of
# libtoby.a, so out-of-tree apps get notification client support for
# free without us shipping a separate libtoby_notify.a.

LIBTOBY_OBJS    := $(LIBTOBY_C_SRCS:.c=.o)

# Assembly sources (setjmp.S)
LIBTOBY_ASM_SRCS := $(LIBTOBY_SRC)/setjmp.S
LIBTOBY_ASM_OBJS := $(LIBTOBY_ASM_SRCS:.S=.o)
LIBTOBY_OBJS     += $(LIBTOBY_ASM_OBJS)

# CFLAGS for compiling the libtoby implementation. Same freestanding
# base as USER_CFLAGS, plus access to its OWN public headers and to
# the kernel ABI header (read-only for the syscall numbers).
# NOTE: the LIVE libtoby headers must come first as a plain -I --
# plain -I dirs are searched before every -isystem dir, so without it
# the STALE toby/* mirror under build/sdk-1.0.0/include (refreshed only
# by the sdk target) shadows libtoby/include after any header change.
LIBTOBY_BUILD_CFLAGS := $(USER_CFLAGS) \
                        -I $(LIBTOBY_INC) \
                        -I include \
                        -I build/sdk-1.0.0/include

$(LIBTOBY_SRC)/%.o: $(LIBTOBY_SRC)/%.c
	$(CC) $(LIBTOBY_BUILD_CFLAGS) -c $< -o $@

# Assembly rule for setjmp.S
$(LIBTOBY_SRC)/%.o: $(LIBTOBY_SRC)/%.S
	$(CC) $(USER_CFLAGS) -c $< -o $@

# math.c needs SSE for double-precision floating-point arithmetic
$(LIBTOBY_SRC)/math.o: $(LIBTOBY_SRC)/math.c
	$(CC) $(LIBTOBY_BUILD_CFLAGS) -msse -msse2 -c $< -o $@

# stdio.c needs SSE for %f/%e/%g (double math + reading the double vararg)
$(LIBTOBY_SRC)/stdio.o: $(LIBTOBY_SRC)/stdio.c
	$(CC) $(LIBTOBY_BUILD_CFLAGS) -msse -msse2 -c $< -o $@

# stdlib.c needs SSE for strtod/atof floating-point conversion
$(LIBTOBY_SRC)/stdlib.o: $(LIBTOBY_SRC)/stdlib.c
	$(CC) $(LIBTOBY_BUILD_CFLAGS) -msse -msse2 -c $< -o $@

# time.c needs SSE for difftime (returns double)
$(LIBTOBY_SRC)/time.o: $(LIBTOBY_SRC)/time.c
	$(CC) $(LIBTOBY_BUILD_CFLAGS) -msse -msse2 -c $< -o $@

# font.c needs SSE for floating-point math in stb_truetype
$(LIBTOBY_SRC)/font.o: $(LIBTOBY_SRC)/font.c
	$(CC) $(LIBTOBY_BUILD_CFLAGS) -msse -msse2 -Wno-unused-function -c $< -o $@

# emoji.c calls stb_truetype (float scale) -> needs SSE like font.c
$(LIBTOBY_SRC)/emoji.o: $(LIBTOBY_SRC)/emoji.c
	$(CC) $(LIBTOBY_BUILD_CFLAGS) -msse -msse2 -Wno-unused-function -c $< -o $@

# image.c needs SSE for stb_image SIMD paths
$(LIBTOBY_SRC)/image.o: $(LIBTOBY_SRC)/image.c
	$(CC) $(LIBTOBY_BUILD_CFLAGS) -msse -msse2 -Wno-unused-function -Wno-sign-compare -c $< -o $@

# mp3_decode.c embeds the real minimp3 implementation (float synthesis
# + SSE2 intrinsics on x86-64), so it needs SSE like stb_image/stb_tt.
$(LIBTOBY_SRC)/mp3_decode.o: $(LIBTOBY_SRC)/mp3_decode.c
	$(CC) $(LIBTOBY_BUILD_CFLAGS) -msse -msse2 -Wno-unused-function -c $< -o $@

# wasm_bridge.c reaches into wasm3's private headers (M3Module layout),
# so it needs the wasm3 include path; -msse -msse2 to match the engine.
$(LIBTOBY_SRC)/wasm_bridge.o: $(LIBTOBY_SRC)/wasm_bridge.c
	$(CC) $(LIBTOBY_BUILD_CFLAGS) -msse -msse2 -I third_party/wasm3 -w -c $< -o $@

# ---------- stage 13: freestanding C++ for tobyOS userland ----------
# CXX is derived from CC (clang -> clang++), so the TMP='C:\t' wrapper
# and any user override carry through. C++ TUs build with the same
# freestanding base as C, minus -std=c11, plus the no-runtime-features
# trio: no exceptions, no RTTI, no hosted C++ stdlib. What remains of
# the language (classes, virtuals, templates, new/delete, static
# ctors/dtors, local statics) is backed by libtoby/src/cxxrt.cpp and
# the ctor walk in init.c.
CXX := $(CC:clang=clang++)
LIBTOBY_CXXFLAGS := $(filter-out -std=c11,$(LIBTOBY_BUILD_CFLAGS)) \
                    -std=c++17 -fno-exceptions -fno-rtti -nostdinc++

$(LIBTOBY_SRC)/cxxrt.o: $(LIBTOBY_SRC)/cxxrt.cpp $(LIBTOBY_INC)/new
	$(CXX) $(LIBTOBY_CXXFLAGS) -c $< -o $@

$(LIBTOBY_SRC)/cxxrt.pic.o: $(LIBTOBY_SRC)/cxxrt.cpp $(LIBTOBY_INC)/new
	$(CXX) $(LIBTOBY_CXXFLAGS) -fPIC -c $< -o $@

# oh264_glue.cpp (media slice 3): C-linkage bridge from the C
# video_decoder to the openh264 C++ ISVCDecoder + I420->ARGB. Needs the
# openh264 public API header. Only pulled from libtoby.a by a program
# that actually decodes Main/High-profile H.264 (via video_decoder).
$(LIBTOBY_SRC)/oh264_glue.o: $(LIBTOBY_SRC)/oh264_glue.cpp
	$(CXX) $(LIBTOBY_CXXFLAGS) -I third_party/openh264/api -c $< -o $@

$(LIBTOBY_SRC)/oh264_glue.pic.o: $(LIBTOBY_SRC)/oh264_glue.cpp
	$(CXX) $(LIBTOBY_CXXFLAGS) -fPIC -I third_party/openh264/api -c $< -o $@

LIBTOBY_OBJS += $(LIBTOBY_SRC)/cxxrt.o $(LIBTOBY_SRC)/oh264_glue.o
# (the .pic.o append lives next to the other vendored PIC appends,
# after LIBTOBY_PIC_OBJS is first assigned)

# WebP decode (stage 13 beyond-tier): the vendored libwebp v1.4.0
# decode-only subset (src/dec + the decode-side src/dsp + src/utils),
# compiled into libtoby so toby_image_load() decodes RIFF/WEBP like any
# other format. The include path is rooted at the package dir (libwebp
# uses "src/..."-rooted includes). SSE2 on, like stb_image's rule; the
# *_sse41 variants compile to empty TUs without -msse4.1 and the
# runtime dispatch never selects them. -w: vendored reference code
# under -Wall -Wextra. WEBP_USE_THREAD stays undefined (sequential).
LIBWEBP_DIR  := third_party/libwebp
LIBWEBP_SRCS := $(wildcard $(LIBWEBP_DIR)/src/dec/*.c) \
                $(wildcard $(LIBWEBP_DIR)/src/dsp/*.c) \
                $(wildcard $(LIBWEBP_DIR)/src/utils/*.c)
LIBWEBP_OBJS     := $(LIBWEBP_SRCS:.c=.o)
LIBWEBP_PIC_OBJS := $(LIBWEBP_SRCS:.c=.pic.o)
LIBTOBY_OBJS += $(LIBWEBP_OBJS)

$(LIBWEBP_DIR)/%.o: $(LIBWEBP_DIR)/%.c Makefile
	$(CC) $(LIBTOBY_BUILD_CFLAGS) -msse -msse2 -I $(LIBWEBP_DIR) -w -c $< -o $@

$(LIBWEBP_DIR)/%.pic.o: $(LIBWEBP_DIR)/%.c Makefile
	$(CC) $(LIBTOBY_PIC_CFLAGS) -msse -msse2 -I $(LIBWEBP_DIR) -w -c $< -o $@

# Real H.264 decode (stage 13 media): the vendored h264bsd baseline-
# profile decoder (Android AOSP-derived, pure integer C) backing
# libtoby's video_decoder API. -w: vendored reference code.
H264BSD_DIR  := third_party/h264bsd
H264BSD_SRCS := $(wildcard $(H264BSD_DIR)/*.c)
H264BSD_OBJS     := $(H264BSD_SRCS:.c=.o)
H264BSD_PIC_OBJS := $(H264BSD_SRCS:.c=.pic.o)
LIBTOBY_OBJS += $(H264BSD_OBJS)

$(H264BSD_DIR)/%.o: $(H264BSD_DIR)/%.c Makefile
	$(CC) $(LIBTOBY_BUILD_CFLAGS) -I $(H264BSD_DIR) -w -c $< -o $@

$(H264BSD_DIR)/%.pic.o: $(H264BSD_DIR)/%.c Makefile
	$(CC) $(LIBTOBY_PIC_CFLAGS) -I $(H264BSD_DIR) -w -c $< -o $@

# AAC-LC audio decode (media parity): the vendored Helix fixed-point
# AAC decoder (RealNetworks RPSL), pure integer, freestanding on the
# x86-64 portable-C path in assembly.h.
HELIXAAC_DIR  := third_party/libhelix-aac
HELIXAAC_SRCS := $(wildcard $(HELIXAAC_DIR)/*.c)
HELIXAAC_OBJS     := $(HELIXAAC_SRCS:.c=.o)
HELIXAAC_PIC_OBJS := $(HELIXAAC_SRCS:.c=.pic.o)
LIBTOBY_OBJS += $(HELIXAAC_OBJS)

$(HELIXAAC_DIR)/%.o: $(HELIXAAC_DIR)/%.c Makefile
	$(CC) $(LIBTOBY_BUILD_CFLAGS) -I $(HELIXAAC_DIR) -w -c $< -o $@

$(HELIXAAC_DIR)/%.pic.o: $(HELIXAAC_DIR)/%.c Makefile
	$(CC) $(LIBTOBY_PIC_CFLAGS) -I $(HELIXAAC_DIR) -w -c $< -o $@

# WebAssembly runtime (app-platform parity): the vendored wasm3 0.5.2
# interpreter (MIT), pure portable C, freestanding on tobyOS. Only the
# core parse/compile/exec engine is vendored -- the WASI/uvwasi/tracer
# host-API files are excluded (we expose imports/exports through the JS
# WebAssembly bridge instead of WASI). -w: vendored reference code.
WASM3_DIR  := third_party/wasm3
WASM3_SRCS := $(wildcard $(WASM3_DIR)/*.c)
WASM3_OBJS     := $(WASM3_SRCS:.c=.o)
WASM3_PIC_OBJS := $(WASM3_SRCS:.c=.pic.o)
LIBTOBY_OBJS += $(WASM3_OBJS)

$(WASM3_DIR)/%.o: $(WASM3_DIR)/%.c Makefile
	$(CC) $(LIBTOBY_BUILD_CFLAGS) -msse -msse2 -I $(WASM3_DIR) -w -c $< -o $@

$(WASM3_DIR)/%.pic.o: $(WASM3_DIR)/%.c Makefile
	$(CC) $(LIBTOBY_PIC_CFLAGS) -msse -msse2 -I $(WASM3_DIR) -w -c $< -o $@

# High-profile H.264 decode (stage 13 media): the vendored openh264
# decoder (Cisco, BSD-2), C++ built freestanding via the cpp-runtime.
# Single-threaded decode -- only WelsThreadPool + WelsTaskThread are
# excluded (the pool carries a global ctor and both are used only at
# threadCount > 1); WelsThread + wels_decoder_thread stay in because the
# decode core references their Sem*/Event*/GetCPUCount helpers even
# single-threaded. Threading/OS gaps are filled by
# third_party/openh264/shim. Archive semantics mean a program that never
# calls the H.264 decoder does not pull these objects, so this is inert
# until video_decode.c routes to it. -w: vendored reference code.
OPENH264_DIR  := third_party/openh264
OPENH264_SRCS := $(filter-out \
    $(OPENH264_DIR)/common/src/WelsThreadPool.cpp \
    $(OPENH264_DIR)/common/src/WelsTaskThread.cpp, \
    $(wildcard $(OPENH264_DIR)/common/src/*.cpp) \
    $(wildcard $(OPENH264_DIR)/decoder/core/src/*.cpp) \
    $(wildcard $(OPENH264_DIR)/decoder/plus/src/*.cpp))
OPENH264_OBJS     := $(OPENH264_SRCS:.cpp=.o)
OPENH264_PIC_OBJS := $(OPENH264_SRCS:.cpp=.pic.o)
OPENH264_INC := -I $(OPENH264_DIR)/shim -I $(OPENH264_DIR)/api \
    -I $(OPENH264_DIR)/common/inc -I $(OPENH264_DIR)/decoder/core/inc \
    -I $(OPENH264_DIR)/decoder/plus/inc
LIBTOBY_OBJS += $(OPENH264_OBJS)

$(OPENH264_DIR)/%.o: $(OPENH264_DIR)/%.cpp Makefile
	$(CXX) $(OPENH264_INC) $(LIBTOBY_CXXFLAGS) -msse -msse2 -w -c $< -o $@

$(OPENH264_DIR)/%.pic.o: $(OPENH264_DIR)/%.cpp Makefile
	$(CXX) $(OPENH264_INC) $(LIBTOBY_CXXFLAGS) -fPIC -msse -msse2 -w -c $< -o $@

# libgav1 (Google AV1 decoder, BSD-3) -- decode subset for AVIF, built
# freestanding on the tobyOS C++ STL. No arch SIMD (LIBGAV1_ENABLE_*=0 ->
# C fallbacks); the thread pool uses the STL's std::mutex/condition_variable
# (LIBGAV1_THREADPOOL_USE_STD_MUTEX=1) so no abseil is pulled. malloc.h is
# shimmed for posix_memalign. Archive semantics keep this inert until the
# image loader routes to it. -w: vendored reference code.
LIBGAV1_DIR  := third_party/libgav1
LIBGAV1_SRCS := $(wildcard $(LIBGAV1_DIR)/src/*.cc) \
    $(wildcard $(LIBGAV1_DIR)/src/dsp/*.cc) \
    $(wildcard $(LIBGAV1_DIR)/src/tile/*.cc) \
    $(wildcard $(LIBGAV1_DIR)/src/tile/bitstream/*.cc) \
    $(wildcard $(LIBGAV1_DIR)/src/utils/*.cc) \
    $(wildcard $(LIBGAV1_DIR)/src/post_filter/*.cc)
LIBGAV1_OBJS     := $(LIBGAV1_SRCS:.cc=.o)
LIBGAV1_PIC_OBJS := $(LIBGAV1_SRCS:.cc=.pic.o)
LIBGAV1_INC := -I $(LIBGAV1_DIR)/shim -I $(LIBGAV1_DIR) -I $(LIBGAV1_DIR)/src
LIBGAV1_CFG := -DLIBGAV1_MAX_BITDEPTH=10 -DLIBGAV1_THREADPOOL_USE_STD_MUTEX=1 \
    -DLIBGAV1_ENABLE_NEON=0 -DLIBGAV1_ENABLE_SSE4_1=0 -DLIBGAV1_ENABLE_AVX2=0 \
    -DLIBGAV1_ENABLE_LOGGING=0
LIBTOBY_OBJS += $(LIBGAV1_OBJS)

$(LIBGAV1_DIR)/%.o: $(LIBGAV1_DIR)/%.cc Makefile
	$(CXX) $(LIBGAV1_INC) $(LIBGAV1_CFG) $(LIBTOBY_CXXFLAGS) -msse -msse2 -w -c $< -o $@

$(LIBGAV1_DIR)/%.pic.o: $(LIBGAV1_DIR)/%.cc Makefile
	$(CXX) $(LIBGAV1_INC) $(LIBGAV1_CFG) $(LIBTOBY_CXXFLAGS) -fPIC -msse -msse2 -w -c $< -o $@

# avif_decode.cpp: C-linkage bridge from the image loader to libgav1
# (HEIF box parse -> AV1 decode -> RGBA). Needs the libgav1 include roots.
# Only pulled from libtoby.a by a program that decodes an AVIF.
$(LIBTOBY_SRC)/avif_decode.o: $(LIBTOBY_SRC)/avif_decode.cpp
	$(CXX) $(LIBGAV1_INC) $(LIBGAV1_CFG) $(LIBTOBY_CXXFLAGS) -msse -msse2 -c $< -o $@

$(LIBTOBY_SRC)/avif_decode.pic.o: $(LIBTOBY_SRC)/avif_decode.cpp
	$(CXX) $(LIBGAV1_INC) $(LIBGAV1_CFG) $(LIBTOBY_CXXFLAGS) -fPIC -msse -msse2 -c $< -o $@

LIBTOBY_OBJS += $(LIBTOBY_SRC)/avif_decode.o

$(LIBTOBY_CRT0): $(LIBTOBY_SRC)/crt0.S
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(LIBTOBY_AR): $(LIBTOBY_OBJS)
	rm -f $@
	$(AR) rcs $@ $(LIBTOBY_OBJS)

# CFLAGS for user programs that LINK against libtoby. They get the
# public POSIX-shape headers under libtoby/include only -- not the
# kernel tree. Users wanting raw syscalls can still #include
# <tobyos/abi/abi.h> by adding `-I ../../include` themselves.
LIBTOBY_CFLAGS  := $(USER_CFLAGS) -isystem $(LIBTOBY_INC) -isystem third_party/wasm3

# ---------- Milestone 25D: libtoby.so (shared variant) ----------
#
# Same source files as libtoby.a, rebuilt with -fPIC and linked as a
# shared object so dynamic programs can use them via /lib/libtoby.so.
# Static programs continue to link the .a -- the two builds coexist
# verbatim from the same .c files. We don't put crt0.S into .so: each
# program owns its own _start.
#
# Link options worth highlighting:
#   -shared             : produce ET_DYN
#   -soname libtoby.so  : DT_SONAME the dynamic linker uses for matching
#   -z now              : eager binding (we don't implement lazy PLT)
#   -Bsymbolic          : intra-DSO references resolve at link time,
#                          which avoids any PLT trampoline within
#                          libtoby.so itself
#   --no-undefined      : every symbol must be defined at link time --
#                          there is no "find this elsewhere" because
#                          libtoby.so has no DT_NEEDED of its own
#                          (its only "external" interface is the
#                          kernel SYSCALL instruction)

LIBTOBY_SO          := $(LIBTOBY_DIR)/libtoby.so
LIBTOBY_PIC_OBJS    := $(LIBTOBY_C_SRCS:.c=.pic.o)
LIBTOBY_ASM_PIC_OBJS := $(LIBTOBY_ASM_SRCS:.S=.pic.o)
LIBTOBY_PIC_OBJS    += $(LIBTOBY_ASM_PIC_OBJS)
LIBTOBY_PIC_OBJS    += $(LIBWEBP_PIC_OBJS)
LIBTOBY_PIC_OBJS    += $(H264BSD_PIC_OBJS)
LIBTOBY_PIC_OBJS    += $(HELIXAAC_PIC_OBJS)
LIBTOBY_PIC_OBJS    += $(WASM3_PIC_OBJS)
LIBTOBY_PIC_OBJS    += $(OPENH264_PIC_OBJS)
LIBTOBY_PIC_OBJS    += $(LIBGAV1_PIC_OBJS)
LIBTOBY_PIC_OBJS    += $(LIBTOBY_SRC)/avif_decode.pic.o
LIBTOBY_PIC_OBJS    += $(LIBTOBY_SRC)/cxxrt.pic.o $(LIBTOBY_SRC)/oh264_glue.pic.o
LIBTOBY_PIC_CFLAGS  := $(LIBTOBY_BUILD_CFLAGS) -fPIC

$(LIBTOBY_SRC)/%.pic.o: $(LIBTOBY_SRC)/%.c
	$(CC) $(LIBTOBY_PIC_CFLAGS) -c $< -o $@

$(LIBTOBY_SRC)/%.pic.o: $(LIBTOBY_SRC)/%.S
	$(CC) $(USER_CFLAGS) -fPIC -c $< -o $@

$(LIBTOBY_SRC)/math.pic.o: $(LIBTOBY_SRC)/math.c
	$(CC) $(LIBTOBY_PIC_CFLAGS) -msse -msse2 -c $< -o $@

$(LIBTOBY_SRC)/stdio.pic.o: $(LIBTOBY_SRC)/stdio.c
	$(CC) $(LIBTOBY_PIC_CFLAGS) -msse -msse2 -c $< -o $@

$(LIBTOBY_SRC)/stdlib.pic.o: $(LIBTOBY_SRC)/stdlib.c
	$(CC) $(LIBTOBY_PIC_CFLAGS) -msse -msse2 -c $< -o $@

$(LIBTOBY_SRC)/time.pic.o: $(LIBTOBY_SRC)/time.c
	$(CC) $(LIBTOBY_PIC_CFLAGS) -msse -msse2 -c $< -o $@

$(LIBTOBY_SRC)/font.pic.o: $(LIBTOBY_SRC)/font.c
	$(CC) $(LIBTOBY_PIC_CFLAGS) -msse -msse2 -Wno-unused-function -c $< -o $@

$(LIBTOBY_SRC)/emoji.pic.o: $(LIBTOBY_SRC)/emoji.c
	$(CC) $(LIBTOBY_PIC_CFLAGS) -msse -msse2 -Wno-unused-function -c $< -o $@

$(LIBTOBY_SRC)/image.pic.o: $(LIBTOBY_SRC)/image.c
	$(CC) $(LIBTOBY_PIC_CFLAGS) -msse -msse2 -Wno-unused-function -Wno-sign-compare -c $< -o $@

$(LIBTOBY_SRC)/mp3_decode.pic.o: $(LIBTOBY_SRC)/mp3_decode.c
	$(CC) $(LIBTOBY_PIC_CFLAGS) -msse -msse2 -Wno-unused-function -c $< -o $@

$(LIBTOBY_SRC)/wasm_bridge.pic.o: $(LIBTOBY_SRC)/wasm_bridge.c
	$(CC) $(LIBTOBY_PIC_CFLAGS) -msse -msse2 -I third_party/wasm3 -w -c $< -o $@

$(LIBTOBY_SO): $(LIBTOBY_PIC_OBJS)
	$(LD) -m elf_x86_64 -nostdlib -shared -soname libtoby.so \
	      -z max-page-size=0x1000 -z now -Bsymbolic \
	      --allow-multiple-definition \
	      --unresolved-symbols=ignore-all \
	      -o $@ $(LIBTOBY_PIC_OBJS)

# ---------- Milestone 25D: ld-toby.so (dynamic linker) ----------
#
# tobyOS's userspace dynamic linker, named in PT_INTERP for every
# dynamically linked program (path: /lib/ld-toby.so).
#
# Self-contained: must NOT depend on libtoby.so (libtoby is the
# thing it loads). Therefore:
#   - Its own SYSCALLs go through ld_syscall.S (ld_syscall1/3),
#     not the libtoby toby_sc* helpers.
#   - It defines its own tiny strlen/strcmp/memcpy/puts.
#   - --no-undefined catches any accidental dependency.
#
# Build flags:
#   -fPIC -ffreestanding         freestanding PIC C
#   -fvisibility=hidden          force RIP-relative addressing for
#                                _DYNAMIC and __ehdr_start (GOT
#                                isn't usable before self-relocation)
#   -fno-stack-protector         no canaries (no TLS yet)
#   -fno-builtin                 prevent the compiler from synthesising
#                                memcpy/memset calls inside the
#                                self-relocator
#   -mno-{red-zone,sse,...}      same kernel-style flags we use
#                                everywhere
#   -O2                          enables RIP-relative addressing
#                                optimisations
#
# Link flags:
#   -shared -soname ld-toby.so   ET_DYN with a SONAME entry
#   -e _start                    explicit entry point so the kernel
#                                jumps directly into the asm trampoline
#   -z now -Bsymbolic            never lazy bind (we don't have a
#                                lazy binder!) and resolve internal
#                                refs at link time
#   --no-undefined               see above

LDTOBY_DIR    := ld
LDTOBY_SRC    := $(LDTOBY_DIR)/src
LDTOBY_SO     := $(LDTOBY_DIR)/ld-toby.so

LDTOBY_C_SRCS := $(LDTOBY_SRC)/ld_self.c $(LDTOBY_SRC)/ld_main.c
LDTOBY_S_SRCS := $(LDTOBY_SRC)/ld_start.S $(LDTOBY_SRC)/ld_syscall.S

LDTOBY_C_OBJS := $(LDTOBY_C_SRCS:.c=.o)
LDTOBY_S_OBJS := $(LDTOBY_S_SRCS:.S=.o)
LDTOBY_OBJS   := $(LDTOBY_S_OBJS) $(LDTOBY_C_OBJS)

LDTOBY_CFLAGS := --target=x86_64-elf -ffreestanding -fno-stack-protector \
                 -fno-unwind-tables -fno-asynchronous-unwind-tables \
                 -fno-builtin -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
                 -fPIC -fvisibility=hidden \
                 -O2 -Wall -Wextra -std=c11 -nostdlib

$(LDTOBY_SRC)/%.o: $(LDTOBY_SRC)/%.c $(LDTOBY_SRC)/ld_internal.h
	$(CC) $(LDTOBY_CFLAGS) -c $< -o $@

$(LDTOBY_SRC)/%.o: $(LDTOBY_SRC)/%.S
	$(CC) $(LDTOBY_CFLAGS) -c $< -o $@

$(LDTOBY_SO): $(LDTOBY_OBJS)
	$(LD) -m elf_x86_64 -nostdlib -shared -soname ld-toby.so \
	      -e _start -z max-page-size=0x1000 -z now -Bsymbolic \
	      --no-undefined -o $@ $(LDTOBY_OBJS)

.PHONY: ldtoby
ldtoby: $(LDTOBY_SO)

# Helper template: link a user program with crt0.o + main.o + libtoby.a.
# Args: $(1) = directory under programs/, $(2) = output name (defaults to dir).
define LIBTOBY_PROGRAM_RULES
programs/$(1)/$(2).o: programs/$(1)/main.c $(LIBTOBY_AR)
	$$(CC) $$(LIBTOBY_CFLAGS) -c $$< -o $$@

programs/$(1)/$(2).elf: programs/$(1)/$(2).o \
                        $(LIBTOBY_CRT0) \
                        $(LIBTOBY_AR) \
                        programs/$(1)/program.ld
	$$(LD) $$(PROG_LDFLAGS) -T programs/$(1)/program.ld -o $$@ \
	      $(LIBTOBY_CRT0) programs/$(1)/$(2).o $(LIBTOBY_AR)
endef

# Variant of LIBTOBY_PROGRAM_RULES for M26A test programs that need the
# raw kernel ABI header <tobyos/abi/abi.h> (for ABI_DEVT_* constants and
# struct abi_dev_info). Adds `-I include` so the kernel-side ABI header
# is reachable; the rest of the build remains identical so we still link
# against libtoby.a + crt0.o the same way every other static program does.
# Other programs continue to see ONLY libtoby/include headers.
define LIBTOBY_KABI_PROGRAM_RULES
# Slice 69: PROG_EXTRA_CFLAGS lets a flavour build pass defines to USER
# programs. EXTRA_CFLAGS only reaches kernel objects, so -DCHROME_FULL
# silently missed chromewin and it exec'd the (now unstaged) headless-shell
# path -- an exit-127 that looked like a loader failure.
programs/$(1)/$(2).o: programs/$(1)/main.c $(LIBTOBY_AR)
	$$(CC) $$(LIBTOBY_CFLAGS) $$(PROG_EXTRA_CFLAGS) -I include -c $$< -o $$@

programs/$(1)/$(2).elf: programs/$(1)/$(2).o \
                        $(LIBTOBY_CRT0) \
                        $(LIBTOBY_AR) \
                        programs/$(1)/program.ld
	$$(LD) $$(PROG_LDFLAGS) -T programs/$(1)/program.ld -o $$@ \
	      $(LIBTOBY_CRT0) programs/$(1)/$(2).o $(LIBTOBY_AR)
endef

# C++ variant (stage 13): main.cpp compiled with CXX + the freestanding
# C++ flags; links the same crt0 + libtoby.a (which carries cxxrt.o).
# The program's own program.ld must define the .init_array/.fini_array
# bounds for static constructors (see programs/user_cpptest/program.ld).
define LIBTOBY_CPP_PROGRAM_RULES
programs/$(1)/$(2).o: programs/$(1)/main.cpp $(LIBTOBY_AR)
	$$(CXX) $$(filter-out -std=c11,$$(LIBTOBY_CFLAGS)) \
	      -std=c++17 -fno-exceptions -fno-rtti -nostdinc++ -c $$< -o $$@

programs/$(1)/$(2).elf: programs/$(1)/$(2).o \
                        $(LIBTOBY_CRT0) \
                        $(LIBTOBY_AR) \
                        programs/$(1)/program.ld
	$$(LD) $$(PROG_LDFLAGS) -T programs/$(1)/program.ld -o $$@ \
	      $(LIBTOBY_CRT0) programs/$(1)/$(2).o $(LIBTOBY_AR)
endef

$(eval $(call LIBTOBY_CPP_PROGRAM_RULES,user_cpptest,user_cpptest))

# /bin/stltest exercises floating point (numeric_limits<float>, <cmath>),
# so it is built with SSE enabled -- like other FP user code and the
# codec libraries (openh264, and libgav1 next). Otherwise a double/float
# return hits "SSE register return with SSE disabled".
programs/user_stltest/user_stltest.o: programs/user_stltest/main.cpp $(LIBTOBY_AR)
	$(CXX) $(filter-out -std=c11,$(LIBTOBY_CFLAGS)) -msse -msse2 \
	      -std=c++17 -fno-exceptions -fno-rtti -nostdinc++ -c $< -o $@

programs/user_stltest/user_stltest.elf: programs/user_stltest/user_stltest.o \
                                        $(LIBTOBY_CRT0) $(LIBTOBY_AR) \
                                        programs/user_stltest/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/user_stltest/program.ld -o $@ \
	      $(LIBTOBY_CRT0) programs/user_stltest/user_stltest.o $(LIBTOBY_AR)

$(eval $(call LIBTOBY_PROGRAM_RULES,user_bsdroute,user_bsdroute))
$(eval $(call LIBTOBY_PROGRAM_RULES,user_mp4play,user_mp4play))
$(eval $(call LIBTOBY_PROGRAM_RULES,user_aactest,user_aactest))
programs/user_aactest/user_aactest.o: programs/user_aactest/aac_clip.h

$(eval $(call LIBTOBY_PROGRAM_RULES,user_wasmtest,user_wasmtest))
programs/user_wasmtest/user_wasmtest.o: programs/user_wasmtest/wasm_clip.h
# wasm3 is built -msse -msse2; this program marshals f32 args/results
# across the m3_CallV/m3_GetResultsV variadic boundary, which requires
# the SSE calling convention (floats in XMM) on the caller side too.
programs/user_wasmtest/user_wasmtest.o: LIBTOBY_CFLAGS += -msse -msse2

# /bin/av1test (libgav1 slice 2): C++ decode-correctness test that links
# libgav1's Decoder out of libtoby.a. Needs the libgav1 include roots +
# config, and SSE (codec ABI).
programs/user_av1test/user_av1test.o: programs/user_av1test/main.cpp \
                                      programs/user_av1test/av1_clip.h \
                                      programs/user_av1test/avif_clip.h $(LIBTOBY_AR)
	$(CXX) $(filter-out -std=c11,$(LIBTOBY_CFLAGS)) $(LIBGAV1_INC) $(LIBGAV1_CFG) \
	      -msse -msse2 -std=c++17 -fno-exceptions -fno-rtti -nostdinc++ -c $< -o $@

programs/user_av1test/user_av1test.elf: programs/user_av1test/user_av1test.o \
                                        $(LIBTOBY_CRT0) $(LIBTOBY_AR) \
                                        programs/user_av1test/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/user_av1test/program.ld -o $@ \
	      $(LIBTOBY_CRT0) programs/user_av1test/user_av1test.o $(LIBTOBY_AR)

# /bin/oh264test (media slice 2): C++ decode-correctness test that links
# the vendored openh264 ISVCDecoder out of libtoby.a. Same C++ program
# shape as cpptest, plus the openh264 public API include for codec_api.h.
programs/user_oh264test/user_oh264test.o: programs/user_oh264test/main.cpp \
                                          programs/user_oh264test/oh264_clip.h \
                                          $(LIBTOBY_AR)
	$(CXX) $(filter-out -std=c11,$(LIBTOBY_CFLAGS)) \
	      -I third_party/openh264/api -std=c++17 -fno-exceptions -fno-rtti \
	      -nostdinc++ -c $< -o $@

programs/user_oh264test/user_oh264test.elf: programs/user_oh264test/user_oh264test.o \
                                            $(LIBTOBY_CRT0) $(LIBTOBY_AR) \
                                            programs/user_oh264test/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/user_oh264test/program.ld -o $@ \
	      $(LIBTOBY_CRT0) programs/user_oh264test/user_oh264test.o $(LIBTOBY_AR)

$(eval $(call LIBTOBY_PROGRAM_RULES,c_hello,c_hello))
# Slice 14: native privilege-drop proof (ABI_SYS_SETUID).
$(eval $(call LIBTOBY_PROGRAM_RULES,nsetuid,nsetuid))
$(eval $(call LIBTOBY_PROGRAM_RULES,c_args,c_args))
$(eval $(call LIBTOBY_PROGRAM_RULES,c_filedemo,c_filedemo))
$(eval $(call LIBTOBY_PROGRAM_RULES,c_alloctest,c_alloctest))
$(eval $(call LIBTOBY_PROGRAM_RULES,c_envtest,c_envtest))
$(eval $(call LIBTOBY_PROGRAM_RULES,init,init))

# ---------- M26A: peripheral test harness userland tools ----------
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,devlist,devlist))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,drvtest,drvtest))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,usbtest,usbtest))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,audiotest,audiotest))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,audioplay,audioplay))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,batterytest,batterytest))

# ---------- M27A: display test harness userland tools ----------
# All three tools ride the same KABI plumbing as the M26A peripheral
# tools because they need <tobyos/abi/abi.h> (display syscall numbers,
# pixel-format / backend tags, struct abi_display_info layout).
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,displayinfo,displayinfo))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,drawtest,drawtest))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,fonttest,fonttest))

# rendertest needs both the kernel ABI header and the SDK GL header.
# LIVE libtoby headers first (plain -I beats -isystem) so the stale
# sdk-stage toby/* mirror can never shadow them -- see LIBTOBY_BUILD_CFLAGS.
programs/rendertest/rendertest.o: programs/rendertest/main.c $(LIBTOBY_AR)
	$(CC) $(LIBTOBY_CFLAGS) -I $(LIBTOBY_INC) -I include -I build/sdk-1.0.0/include -c $< -o $@

programs/rendertest/rendertest.elf: programs/rendertest/rendertest.o \
                                    $(LIBTOBY_CRT0) \
                                    $(LIBTOBY_AR) \
                                    programs/rendertest/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/rendertest/program.ld -o $@ \
	      $(LIBTOBY_CRT0) programs/rendertest/rendertest.o $(LIBTOBY_AR)

# ---------- M28A: structured-log viewer userland tool ----------
# logview reads the kernel slog ring via SYS_SLOG_READ, so it needs
# the raw kernel ABI header just like the M26A/M27A tools above.
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,logview,logview))

# ---------- M28B: crash-dump inspector userland tool ----------
# crashinfo reads /data/crash/last.dump and decodes the abi_crash_header
# at the start. Plain libtoby + ABI is enough.
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,crashinfo,crashinfo))

# ---------- M28C: watchdog inspector userland tool ----------
# wdogtest reads the kernel watchdog state via SYS_WDOG_STATUS. Same
# pattern as logview/crashinfo above.
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,wdogtest,wdogtest))

# ---------- M28D: safe-mode recovery shell ----------
# safesh runs in safe mode in lieu of the GUI desktop. It needs the
# kernel ABI for slog + watchdog access.
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,safesh,safesh))

# ---------- M28E: filesystem-integrity tool ----------
# fscheck reads SYS_FS_CHECK and pretty-prints the kernel's verdict
# on a tobyfs mount. Same KABI plumbing as the other M28 tools.
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,fscheck,fscheck))

# ---------- M28F: service-supervisor tools ----------
# `services` reads SYS_SVC_LIST and pretty-prints the supervisor
# registry. svc_crasher is a tiny userland program that exits
# non-zero so the M28F harness can drive crash-loop detection.
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,services,services))
$(eval $(call LIBTOBY_PROGRAM_RULES,svc_crasher,svc_crasher))

# ---------- M28G: stability self-test tool ----------
# `stabilitytest` calls SYS_STAB_SELFTEST and reports per-subsystem
# status; --boot prints sentinels for the M28G harness; --stress
# adds a heap+disk+syscall stress workload.
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,stabilitytest,stabilitytest))

# ---------- M29A: hardware-inventory tool ----------
# `hwinfo` calls SYS_HWINFO and prints the unified hardware summary
# (CPU/memory/PCI/USB/blk/display/etc) with optional --json,
# --snapshot, --persist, and --boot modes. Same KABI plumbing as
# the rest of the M28/M29 introspection tools.
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,hwinfo,hwinfo))

# ---------- M29B: driver match + fallback report ----------
# `drvmatch` calls SYS_DRVMATCH for every device returned by
# SYS_DEV_LIST and prints the resulting strategy (EXACT, CLASS,
# GENERIC, NONE, FORCED_OFF). --boot mode emits the M29B_DRV
# sentinels test_m29b.ps1 greps for.
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,drvmatch,drvmatch))

# ---------- M35D: hardware-compatibility tool ----------
# `hwcompat` calls SYS_HWCOMPAT_LIST and prints the joined
# PCI+USB compatibility view (status: supported/partial/unsupported,
# bound driver, friendly chip name from drvdb, one-line reason).
# --boot mode emits the M35D_HWC sentinels test_m35.ps1 greps for.
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,hwcompat,hwcompat))

# ---------- M35F: enhanced diagnostics + reporting ----------
# `hwreport` stitches hwinfo + hwcompat + drvmatch into one operator
# report: boot mode, profile, per-tier compat tally, error/warning
# list, and a GREEN/YELLOW/RED system compatibility verdict.
# --boot mode emits the M35F_HWR sentinels test_m35.ps1 greps for.
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,hwreport,hwreport))

# ---------- M35G: final compatibility validation ----------
# `compattest` runs the eight-bucket M35G validation suite (system
# boot / driver match / fallback / network / storage / USB input /
# log capture / no crashes) and emits sentinels that test_m35g.ps1
# pattern-matches against. Tests requiring real hardware return
# SKIPPED_REAL_HARDWARE_REQUIRED rather than touching host devices.
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,compattest,compattest))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,sigtest,sigtest))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,mctest,mctest))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,reclaimtest,reclaimtest))

# ---------- Developer tools ----------
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,strace,strace))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,debugger,debugger))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,pkgmgr,pkgmgr))

# ---------- Advanced userland: shell, utilities, networking, GUI ----------
$(eval $(call LIBTOBY_PROGRAM_RULES,user_sh,user_sh))
# /bin/shparity -- the bash-parity gate. Runs every /etc/shparity/*.sh case
# under BOTH the real GNU bash 5.2 we already ship and /bin/tsh, and requires
# byte-identical stdout plus an identical exit status. See programs/shparity.
$(eval $(call LIBTOBY_PROGRAM_RULES,shparity,shparity))

# /bin/id and /bin/newgrp -- POSIX identity utilities. newgrp changes the
# real/effective gid (checked against /etc/group membership) and execs a
# fresh shell; id is its observer. Both read /etc/passwd + /etc/group.
$(eval $(call LIBTOBY_PROGRAM_RULES,id,id))
$(eval $(call LIBTOBY_PROGRAM_RULES,newgrp,newgrp))

# /bin/ttyparity -- the INTERACTIVE bash-parity gate: both shells on real
# pty pairs, transcripts compared byte-for-byte. See programs/ttyparity.
$(eval $(call LIBTOBY_PROGRAM_RULES,ttyparity,ttyparity))
# /bin/oilspec -- the THIRD-PARTY conformance gate. Same differential engine
# as shparity, but over the ~2.8k-case Oils spec suite in third_party/oils-spec
# rather than the 54 cases we wrote ourselves. See programs/oilspec.
$(eval $(call LIBTOBY_PROGRAM_RULES,oilspec,oilspec))
# The corpus's Python helpers (argv.py and friends) reimplemented in C, so the
# gate does not depend on CPython and the host oracle runs the SAME code.
$(eval $(call LIBTOBY_PROGRAM_RULES,spechelp,spechelp))

# ---------- /bin/tsh: the shell language, hosted in userspace ----------
#
# src/shell.c is compiled TWICE -- once into the kernel (where the keyboard
# poll loop drives it) and once here, where programs/tsh/host.c does. One
# language implementation, two hosts, so the expansion/arithmetic/glob core
# cannot drift between the kernel shell and /bin/tsh.
#
# The kernel object and this one are built from the same file with different
# flags into DIFFERENT output paths, so neither can clobber the other. Note
# -I include: shell.c speaks the kernel's headers (vfs.h, proc.h, file.h) and
# the host implements that interface rather than the other way round.
TSH_OBJS := programs/tsh/shell_hosted.o \
            programs/tsh/host.o \
            programs/tsh/kstubs.o

# -DSHELL_HOSTED: userspace exposes only the POSIX/bash builtin set, so the
# kernel shell's convenience builtins (cat, ls, mkdir, ...) stop shadowing the
# real utilities in /bin. See shell_hosted_builtin() in src/shell.c.
# EXTRA_CFLAGS must reach THIS copy too. src/shell.c is compiled twice, and a
# -D passed on the make line used to land only in the kernel's copy -- so
# instrumentation added to diagnose a /bin/tsh failure silently was not in
# /bin/tsh, and its absent output read as "this code path is never reached".
# Same shape as the PROG_EXTRA_CFLAGS trap: a define reaching one half of a
# twice-compiled file.
programs/tsh/shell_hosted.o: src/shell.c $(LIBTOBY_AR)
	$(CC) $(LIBTOBY_CFLAGS) $(EXTRA_CFLAGS) -I include -DSHELL_HOSTED -c $< -o $@

programs/tsh/%.o: programs/tsh/%.c $(LIBTOBY_AR)
	$(CC) $(LIBTOBY_CFLAGS) -I include -c $< -o $@

programs/tsh/tsh.elf: $(TSH_OBJS) $(LIBTOBY_CRT0) $(LIBTOBY_AR) \
                      programs/tsh/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/tsh/program.ld -o $@ \
	      $(LIBTOBY_CRT0) $(TSH_OBJS) $(LIBTOBY_AR)
$(eval $(call LIBTOBY_PROGRAM_RULES,user_pkg,user_pkg))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,user_svcctl,user_svcctl))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,user_ps,user_ps))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,user_top,user_top))
$(eval $(call LIBTOBY_PROGRAM_RULES,user_cp,user_cp))
$(eval $(call LIBTOBY_PROGRAM_RULES,user_mv,user_mv))
$(eval $(call LIBTOBY_PROGRAM_RULES,user_rm,user_rm))
$(eval $(call LIBTOBY_PROGRAM_RULES,user_find,user_find))
$(eval $(call LIBTOBY_PROGRAM_RULES,user_sort,user_sort))
$(eval $(call LIBTOBY_PROGRAM_RULES,user_tee,user_tee))
$(eval $(call LIBTOBY_PROGRAM_RULES,user_env,user_env))
$(eval $(call LIBTOBY_PROGRAM_RULES,user_chmod,user_chmod))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,user_df,user_df))
$(eval $(call LIBTOBY_PROGRAM_RULES,user_mkdir,user_mkdir))
$(eval $(call LIBTOBY_PROGRAM_RULES,user_touch,user_touch))
$(eval $(call LIBTOBY_PROGRAM_RULES,user_kill,user_kill))
$(eval $(call LIBTOBY_PROGRAM_RULES,user_tail,user_tail))
$(eval $(call LIBTOBY_PROGRAM_RULES,user_uniq,user_uniq))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,user_date,user_date))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,user_wget,user_wget))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,user_nc,user_nc))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,user_ifconfig,user_ifconfig))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,user_nslookup,user_nslookup))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,user_gui_calc,user_gui_calc))

# ---------- Browser application ----------

# ---------- Milestone 6: Settings application ----------

# ---------- Phase 10: Graphical installer ----------
$(eval $(call LIBTOBY_PROGRAM_RULES,installer,installer))

# ---------- Milestones 9-12: Advanced Networking / PnP / Search / Recovery ----------
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,devmgr,devmgr))
$(eval $(call LIBTOBY_PROGRAM_RULES,indexer,indexer))
$(eval $(call LIBTOBY_PROGRAM_RULES,updater,updater))

# bluescreen uses raw syscall stubs (no libtoby, same pattern as gui_calc)
programs/bluescreen/bluescreen.o: programs/bluescreen/main.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

programs/bluescreen/bluescreen.elf: programs/bluescreen/bluescreen.o \
                                    programs/common/start.o \
                                    programs/bluescreen/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/bluescreen/program.ld -o $@ \
	      programs/common/start.o programs/bluescreen/bluescreen.o

# ---------- Milestones 4-5: Multi-User + System Applications ----------
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,lockscreen,lockscreen))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,mediaplayer,mediaplayer))
# Slice 39 FRONT C: chromium-in-a-window host (DevTools pipe + TobyTK blit)
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,chromewin,chromewin))

# ---------- Phase 2-4: System Management + PnP + Security ----------
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,diskmgr,diskmgr))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,eventview,eventview))

# ---------- Phases 5-8: Networking UX / Multimedia / App Lifecycle / Reliability ----------
$(eval $(call LIBTOBY_PROGRAM_RULES,wifipicker,wifipicker))
$(eval $(call LIBTOBY_PROGRAM_RULES,volctrl,volctrl))
$(eval $(call LIBTOBY_PROGRAM_RULES,appinstall,appinstall))

# ---------- Pillar B3: Port Key Programs (lua, make, less, curl) ----------
# lua uses double (floating-point) -- needs SSE like math.c / stdlib.c
programs/lua/lua.o: programs/lua/main.c $(LIBTOBY_AR)
	$(CC) $(LIBTOBY_CFLAGS) -msse -msse2 -Wno-unused-function -c $< -o $@
programs/lua/lua.elf: programs/lua/lua.o $(LIBTOBY_CRT0) $(LIBTOBY_AR) programs/lua/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/lua/program.ld -o $@ \
	      $(LIBTOBY_CRT0) programs/lua/lua.o $(LIBTOBY_AR)
$(eval $(call LIBTOBY_PROGRAM_RULES,make,make))
$(eval $(call LIBTOBY_PROGRAM_RULES,less,less))
$(eval $(call LIBTOBY_PROGRAM_RULES,curl,curl))

# ---------- Pillar D: Self-Hosting (tcc, as, ld) ----------
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,tcc,tcc))
$(eval $(call LIBTOBY_PROGRAM_RULES,as,as))
$(eval $(call LIBTOBY_PROGRAM_RULES,ld,ld))

# ---------- Milestone 3 desktop shell: RETIRED ----------
# The userspace desktop shell (programs/shell/{main,taskbar,startmenu,
# desktop_wire}.c) was superseded by the in-kernel compositor (src/gui.c),
# which owns the taskbar/launcher/desktop. It was built but never shipped;
# removed in the unify-on-TobyTK cleanup.

# ---------- Phase 5: Real POSIX shell (multi-file program) ----------
SH_SRCS := programs/sh/main.c \
            programs/sh/parser.c
SH_OBJS := $(SH_SRCS:.c=.o)

programs/sh/%.o: programs/sh/%.c $(LIBTOBY_AR)
	$(CC) $(LIBTOBY_CFLAGS) -c $< -o $@

programs/sh/sh.elf: $(SH_OBJS) \
                    $(LIBTOBY_CRT0) \
                    $(LIBTOBY_AR) \
                    programs/sh/program.ld
	$(LD) $(PROG_LDFLAGS) --allow-multiple-definition -T programs/sh/program.ld -o $@ \
	      $(LIBTOBY_CRT0) $(SH_OBJS) $(LIBTOBY_AR)

# ---------- Milestone 36: in-OS self-hosting (TobyC + tobybuild) ----------
programs/tobycc/m36_elf.o: programs/tobycc/m36_elf.c
	$(CC) $(LIBTOBY_CFLAGS) -Iinclude -c $< -o $@
programs/tobycc/tobycc.o: programs/tobycc/tobycc.c
	$(CC) $(LIBTOBY_CFLAGS) -Iinclude -c $< -o $@
programs/tobycc/tobycc.elf: programs/tobycc/tobycc.o programs/tobycc/m36_elf.o \
    $(LIBTOBY_CRT0) $(LIBTOBY_AR) programs/c_hello/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/c_hello/program.ld -o $@ \
	    $(LIBTOBY_CRT0) programs/tobycc/tobycc.o programs/tobycc/m36_elf.o $(LIBTOBY_AR)
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,tobybuild,tobybuild))
$(eval $(call LIBTOBY_KABI_PROGRAM_RULES,selfhosttest,selfhosttest))

# ---------- Track B (foreign-binary compat): linux-hello ----------
#
# linux-hello is NOT a tobyOS program: it is a GENUINE Linux x86-64 static
# ELF, compiled with the Linux target triple + -nostdlib (no libtoby, no
# tobyOS headers, raw Linux syscalls). It would run unmodified on a real
# Linux kernel. The ONLY build-time touch is the standard FreeBSD-style
# "brandelf" stamp -- e_ident[EI_OSABI]=3 (Linux) -- which tells tobyOS's
# ELF loader to run it under the Linux ABI personality (ABI_PERS_LINUX).
# Proves the Linux-compat layer (personality latch -> Linux syscall
# translation -> arch_prctl TLS -> writev -> exit_group) end to end.
programs/linux-hello/linux-hello.elf: programs/linux-hello/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# Tier 2.5: chrome forks+execs xdg-settings for default-browser checks.
# A Linux-personality no-op avoids ENOENT PATH storms and shortens the
# CoW-fork child lifetime versus a failed exec.
programs/linux-xdg-settings/linux-xdg-settings.elf: programs/linux-xdg-settings/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# Slice 90: a crashpad handler that ANSWERS. /opt/chrome/chrome_crashpad_handler
# used to be staged as a COPY of the xdg-settings stub above, whose whole body
# is exit_group(0) -- so the handler died in the same millisecond it exec'd,
# its AF_UNIX endpoint went with it, and every later client sendmsg on that
# socket got -EPIPE -> INT3 -> exit_group(191). chrome runs the crashpad
# handshake even under --disable-crash-reporter (slice 75), so the handler has
# to stay alive and reply, not merely exist.
programs/linux-crashpad/linux-crashpad.elf: programs/linux-crashpad/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# Track B/B4: a genuine Linux x86-64 static ELF proving the Linux signal
# path (rt_sigaction + restorer + rt_sigreturn). Same build shape as
# linux-hello (raw syscalls, no libc, brandelf'd EI_OSABI=Linux).
programs/linux-sigtest/linux-sigtest.elf: programs/linux-sigtest/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# Track B/B6: a genuine Linux x86-64 static ELF proving file-backed mmap --
# mmaps files at offset 0 and at a page offset and verifies the bytes.
programs/linux-mmaptest/linux-mmaptest.elf: programs/linux-mmaptest/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# Track B/B9: a genuine Linux x86-64 static ELF proving thread support --
# spawns a thread with clone(CLONE_VM) and verifies it ran in shared memory.
programs/linux-thread/linux-thread.elf: programs/linux-thread/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# Track B (graphics): a genuine Linux x86-64 fbdev app -- open /dev/fb0,
# FBIOGET_VSCREENINFO/FSCREENINFO, mmap the scanout, draw pixels, FBIOPAN_DISPLAY.
programs/linux-fb/linux-fb.elf: programs/linux-fb/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# GUI-framework M3: a long-lived (animated) Linux fbdev app for the three-worlds
# demo. Same toolchain/branding as linux-fb; presents into a compositor window.
programs/linux-fbwin/linux-fbwin.elf: programs/linux-fbwin/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

programs/linux-join/linux-join.elf: programs/linux-join/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

programs/linux-poll/linux-poll.elf: programs/linux-poll/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

programs/linux-netserver/linux-netserver.elf: programs/linux-netserver/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# Track B/B15: a genuine Linux x86-64 static ELF proving signal completeness --
# SA_SIGINFO 3-arg handlers receiving x86-64 *Linux-layout* siginfo_t/ucontext_t,
# plus a catchable SYNCHRONOUS SIGSEGV (fault delivered to the user handler with
# the correct si_addr). Same build shape as the other brandelf'd Linux ELFs.
programs/linux-siginfo/linux-siginfo.elf: programs/linux-siginfo/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# Track B/B16: a genuine Linux x86-64 static ELF proving the networking CLIENT
# path -- a Linux binary RESOLVES a hostname over a UDP DNS socket and FETCHES
# it over a TCP active-open + HTTP GET, all via the Linux BSD-socket syscalls
# (socket/connect/sendto/recvfrom). Over QEMU SLIRP this reaches the real
# internet. Same brandelf'd build shape as the other Linux ELFs.
programs/linux-httpget/linux-httpget.elf: programs/linux-httpget/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# Track B/B18: a genuine Linux x86-64 static ELF proving the socket-depth
# additions -- a REAL getsockopt (SO_TYPE/SO_ERROR) + scatter-gather
# sendmsg/recvmsg, exercised over the live DNS+HTTP path. Same brandelf'd shape.
programs/linux-msgsock/linux-msgsock.elf: programs/linux-msgsock/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# Track B/B20: a genuine Linux x86-64 static ELF proving the extended /proc
# filesystem -- reads /proc/cpuinfo, /proc/self/status, /proc/self/maps and
# readlinks /proc/self/exe. Same brandelf'd build shape as the other Linux ELFs.
programs/linux-procfs/linux-procfs.elf: programs/linux-procfs/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# Track B/B21: a genuine Linux x86-64 static ELF proving the console TTY +
# termios -- TCGETS (isatty), TCSETS raw-mode roundtrip, TIOCGWINSZ, and a
# cooked canonical read of injected keyboard input. Same brandelf'd shape.
programs/linux-tty/linux-tty.elf: programs/linux-tty/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<

# B23: pseudoterminal proof -- openpty handshake, bidirectional line discipline,
# termios + winsize, and a real busybox shell run on the slave. Static, auto-
# detects as Linux via its PT_GNU_STACK phdr (B19), no brandelf needed.
programs/linux-pty/linux-pty.elf: programs/linux-pty/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<

# B24: /proc + /sys breadth proof (meminfo/uptime/mounts/stat, /proc/self/fd,
# /sys cpu/online). Static, branded Linux by the %.elf EI_OSABI dd step.
programs/linux-procfs2/linux-procfs2.elf: programs/linux-procfs2/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# Futex unit test: a TIMED FUTEX_WAIT must be wakeable by FUTEX_WAKE (ledger DL2,
# the DOM-blocker bug). Static Linux ELF, brand it Linux via the EI_OSABI dd.
programs/linux-futex/linux-futex.elf: programs/linux-futex/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# Slice 82: AF_UNIX socketpair sendmsg/recvmsg ABI test -- reproduces
# crashpad's handshake shape (SEQPACKET + MSG_NOSIGNAL + 40 bytes) in ~30
# lines so the ABI can be compared against a real kernel in seconds instead
# of through a 7-minute full-chrome boot. Same freestanding/static recipe.
programs/linux-scmsg/linux-scmsg.elf: programs/linux-scmsg/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# Eventfd/epoll unit test: epoll_wait on an eventfd must wake when another thread
# writes it (the renderer's efd-14 case). Static Linux ELF, branded via dd.
programs/linux-eventfd/linux-eventfd.elf: programs/linux-eventfd/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# Concurrent-demand-fault lost-write guard (slice 50b): N threads first-touch
# the same madvise-dropped pages in lock-step rounds; a map_4k PTE replace
# (the 0x208c13a YouTube corruption) loses the winner's writes. Static Linux ELF.
programs/linux-demrace/linux-demrace.elf: programs/linux-demrace/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# ALSA PCM guard (audio slice 3): drives /dev/snd/{controlC0,pcmC0D0p} the
# way alsa-lib does -- CARD_INFO, PCM_INFO, HW_REFINE, HW_PARAMS, SW_PARAMS,
# PREPARE, WRITEI_FRAMES, SYNC_PTR -- and requires hw_ptr to ADVANCE, which
# only happens if the DAC really consumed the samples. Static Linux ELF.
programs/linux-sndtest/linux-sndtest.elf: programs/linux-sndtest/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# DRM render-node guard (tier 3 Phase 1b): walks /dev/dri/renderD128 the way
# Mesa's virgl driver does -- VERSION, GETPARAM, GET_CAPS, CONTEXT_INIT,
# RESOURCE_CREATE, MAP+mmap, TRANSFER_TO_HOST, EXECBUFFER -- so a real
# virglrenderer answering is a PASS. Static Linux ELF.
programs/linux-drmtest/linux-drmtest.elf: programs/linux-drmtest/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# CoW-fork STW deadlock guard (slice 92): hammer threads keep the kernel
# writing user pages (clock_gettime out-params) while main fork()s over a
# 256MB region; the old BKL-holding vm_quiesce park wedged the whole kernel
# on the first sweep collision (-smp 4 WHPX only). Static Linux ELF.
programs/linux-qfreeze/linux-qfreeze.elf: programs/linux-qfreeze/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# MAP_SHARED cross-process coherence test: two processes independently mmap the
# same (unlinked) file MAP_SHARED and must be coherent both directions -- the
# ipcz NodeLinkMemory-fragment case behind the DOM blocker. Static Linux ELF.
programs/linux-mapshare/linux-mapshare.elf: programs/linux-mapshare/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# Fake X server for the chromium render tier (slice 38): answers the X11
# connection-setup handshake on /tmp/.X11-unix/X0 so xcb_connect succeeds and
# the Vulkan loader advertises VK_KHR_xcb_surface to ANGLE. Static Linux ELF.
programs/linux-xstub/linux-xstub.elf: programs/linux-xstub/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# ML-KEM/Kyber-768 computational-correctness test (slice 38): chrome's https
# is blocked by BoringSSL's ML-KEM computing a wrong result on tobyOS. Three
# concurrent processes run the kyber768 reference (NTT mod-3329 + Keccak + FO
# decaps -- the exact error-amplifying shape) with deterministic seeds; the
# checksums must match the host reference (programs/linux-mlkem/host_main.c).
# Static freestanding Linux ELF; -I supplies the local <string.h> shim.
MLKEM_SRCS := programs/linux-mlkem/main.c programs/linux-mlkem/selftest.c \
              programs/linux-mlkem/kem.c programs/linux-mlkem/indcpa.c \
              programs/linux-mlkem/polyvec.c programs/linux-mlkem/poly.c \
              programs/linux-mlkem/ntt.c programs/linux-mlkem/reduce.c \
              programs/linux-mlkem/cbd.c programs/linux-mlkem/symmetric-shake.c \
              programs/linux-mlkem/fips202.c programs/linux-mlkem/verify.c
programs/linux-mlkem/linux-mlkem.elf: $(MLKEM_SRCS)
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Iprograms/linux-mlkem \
	      -Wl,-e,_start -o $@ $(MLKEM_SRCS)
	printf '\003' | dd of=$@ bs=1 seek=7 count=1 conv=notrunc 2>/dev/null

# B25: the REAL glibc 2.41 static proof (programs/linux-glibc) is deliberately
# NOT built here -- it must link against an actual glibc sysroot, which the
# freestanding in-tree clang cannot supply. Build it out-of-tree with
# programs/linux-glibc/build.sh; the initrd stages it opt-in when present.

# Track B/B19: a genuine STATIC, NOTE-LESS, UNBRANDED Linux x86-64 ELF -- the
# one shape the loader could not previously auto-detect (no OSABI brand, no
# PT_INTERP). Built exactly like the others EXCEPT there is deliberately NO
# `brandelf` (dd) step, so e_ident[EI_OSABI] stays 0 (ELFOSABI_SYSV). Its only
# Linux fingerprint is the PT_GNU_STACK phdr the linux-gnu toolchain emits,
# which B19 keys off to switch it to ABI_PERS_LINUX with zero preparation.
programs/linux-static-auto/linux-static-auto.elf: programs/linux-static-auto/main.c
	$(CC) --target=x86_64-unknown-linux-gnu -static -nostdlib -ffreestanding \
	      -fno-builtin -fno-stack-protector -fno-pic -fno-pie -O2 -fuse-ld=lld \
	      -Wl,-e,_start -o $@ $<

# ---------- Track C (Win32/PE app compat): win-hello.exe ----------
#
# win-hello.exe is NOT a tobyOS program: it is a GENUINE Windows x86-64
# console PE, built with the Windows target triple and linked against the
# real mingw kernel32 import library, so it is byte-for-byte a normal
# Windows .exe (it would run unmodified on Windows). -nostdlib + a custom
# entry keep the import surface to the three kernel32 symbols C1 shims:
# GetStdHandle / WriteFile / ExitProcess. PE is self-identifying by its
# 'MZ' magic, so -- unlike the brandelf'd Linux ELFs -- it needs NO brand;
# tobyOS's exec path sniffs the magic and routes it to the PE loader.
# awk keeps numeric values as doubles, so this one program needs SSE
# re-enabled -- the default userland flags disable it kernel-style.
programs/awk/awk.o: programs/awk/main.c $(LIBTOBY_AR)
	$(CC) $(LIBTOBY_CFLAGS) -msse -msse2 -c $< -o $@

programs/awk/awk.elf: programs/awk/awk.o $(LIBTOBY_CRT0) $(LIBTOBY_AR) \
                      $(PORTS_SBASE_LD)
	$(LD) $(PROG_LDFLAGS) -T $(PORTS_SBASE_LD) -o $@ \
	      $(LIBTOBY_CRT0) programs/awk/awk.o $(LIBTOBY_AR)

programs/win-hello/win-hello.exe: programs/win-hello/hello.c
	$(CC) --target=x86_64-w64-mingw32 -nostdlib -ffreestanding -fno-builtin \
	      -fno-stack-protector -O1 -Wl,-e,hello_entry -Wl,--subsystem,console \
	      -o $@ $< -lkernel32

# ---------- Track X / X1 (cross-personality pipeline): win-x*.exe ----------
#
# Three GENUINE Windows x86-64 console PEs (same -nostdlib / kernel32-only build
# as win-hello), used as stages of busybox `sh` pipelines that mix Windows and
# Linux binaries on one kernel. They prove X1's execve()->PE path: a Linux shell
# stage execve()s a .exe, whose WriteFile/ReadFile bytes flow through kernel
# pipes to/from Linux stages.
#   win-xprod   : producer  -- WriteFile(stdout, token)
#   win-xcons   : consumer  -- ReadFile(stdin), assert token, exit 0/1
#   win-xfilter : middle    -- ReadFile(stdin) -> WriteFile(stdout, "WIN-SAW:"+in)
programs/win-xprod/win-xprod.exe: programs/win-xprod/main.c
	$(CC) --target=x86_64-w64-mingw32 -nostdlib -ffreestanding -fno-builtin \
	      -fno-stack-protector -O1 -Wl,-e,xprod_entry -Wl,--subsystem,console \
	      -o $@ $< -lkernel32

programs/win-xcons/win-xcons.exe: programs/win-xcons/main.c
	$(CC) --target=x86_64-w64-mingw32 -nostdlib -ffreestanding -fno-builtin \
	      -fno-stack-protector -O1 -Wl,-e,xcons_entry -Wl,--subsystem,console \
	      -o $@ $< -lkernel32

programs/win-xfilter/win-xfilter.exe: programs/win-xfilter/main.c
	$(CC) --target=x86_64-w64-mingw32 -nostdlib -ffreestanding -fno-builtin \
	      -fno-stack-protector -O1 -Wl,-e,xfilter_entry -Wl,--subsystem,console \
	      -o $@ $< -lkernel32

# Track C/C2: a Windows PE that uses the real C RUNTIME. Unlike win-hello
# (-nostdlib), this keeps the ucrt libs so `printf`/`puts` link to their real
# mingw wrappers -> the ucrt stdio primitives (__acrt_iob_func +
# __stdio_common_vfprintf, whose va_list is a stack arg), which tobyOS shims.
# -nostartfiles + a custom entry skip the full mainCRTStartup (SEH/_initterm/
# ~9 import DLLs) so the import surface stays small. -D__USE_MINGW_ANSI_STDIO=0
# routes printf to the ucrt stdio primitives, not the libmingwex wrapper.
programs/win-crt/win-crt.exe: programs/win-crt/main.c
	$(CC) --target=x86_64-w64-mingw32 -nostartfiles -D__USE_MINGW_ANSI_STDIO=0 \
	      -fno-stack-protector -O1 -Wl,-e,c2_entry -Wl,--subsystem,console \
	      -o $@ $<

# Track C/C21: float PRINTING through the kernel printf engine. Same build as
# win-crt (-D__USE_MINGW_ANSI_STDIO=0 routes printf/fprintf to the ucrt stdio
# primitive __stdio_common_vfprintf -> win32_vformat) so the %f/%e/%g doubles are
# rendered by the kernel engine (via the -msse kmath unit), not in-process.
programs/win-printf21/win-printf21.exe: programs/win-printf21/main.c
	@mkdir -p $(dir $@)
	$(CC) --target=x86_64-w64-mingw32 -nostartfiles -D__USE_MINGW_ANSI_STDIO=0 \
	      -fno-stack-protector -O1 -Wl,-e,c21_entry -Wl,--subsystem,console \
	      -o $@ $<

# Track C/C22: formatted/number INPUT (scanf family + strtod/atof). Same build
# as win-crt/win-printf21 (-D__USE_MINGW_ANSI_STDIO=0) so sscanf/fscanf/strtod
# route to the ucrt stdio/convert primitives -> the kernel win32_vscanf engine +
# the -msse kmath_strtod, not libmingwex in-process.
programs/win-scan22/win-scan22.exe: programs/win-scan22/main.c
	@mkdir -p $(dir $@)
	$(CC) --target=x86_64-w64-mingw32 -nostartfiles -D__USE_MINGW_ANSI_STDIO=0 \
	      -fno-stack-protector -O1 -Wl,-e,c22_entry -Wl,--subsystem,console \
	      -o $@ $<

# Track C/C23: wide-char (UTF-16) APIs -- wide CRT string family + wide printf
# (wprintf/swprintf) + wide stdio. Same build as win-printf21/win-scan22
# (-D__USE_MINGW_ANSI_STDIO=0) so the wprintf family routes to the ucrt wide
# stdio primitives -> the kernel win32_wvformat engine, not libmingwex.
programs/win-wide23/win-wide23.exe: programs/win-wide23/main.c
	@mkdir -p $(dir $@)
	$(CC) --target=x86_64-w64-mingw32 -nostartfiles -D__USE_MINGW_ANSI_STDIO=0 \
	      -fno-stack-protector -O1 -Wl,-e,c23_entry -Wl,--subsystem,console \
	      -o $@ $<

# Track C/C3: a STOCK Windows .exe -- a plain `clang main.c -o win-hello3.exe`
# with NO special flags and NO custom entry, so it runs the FULL ucrt
# mainCRTStartup before main(). Proves tobyOS runs unmodified off-the-shelf
# Windows console programs (TEB via SWAPGS + user heap + argv + ~40 CRT shims).
programs/win-hello3/win-hello3.exe: programs/win-hello3/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -o $@ $<

# Track C/C4: a STOCK C++ Windows .exe -- a plain `clang++ main.cpp -o
# win-cpp.exe`. Beyond C3 this runs C++ GLOBAL CONSTRUCTORS (mingw calls them
# from static __main -> __do_global_ctors in CPL3, no kernel trampoline needed)
# and pulls in the wider ucrt surface a C++ program needs, which tobyOS shims.
programs/win-cpp/win-cpp.exe: programs/win-cpp/main.cpp
	clang++ --target=x86_64-w64-mingw32 -O1 -o $@ $<

# Track C/C5: a STOCK Windows .exe exercising the Win32 FILE I/O API
# (CreateFileA/WriteFile/ReadFile/CloseHandle) on top of the C3 CRT startup.
# Proves real HANDLE<->fd mapping + Windows-path translation onto the VFS.
programs/win-fileio/win-fileio.exe: programs/win-fileio/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -o $@ $<

# Track C/C6: a STOCK multithreaded Windows .exe -- CreateThread +
# WaitForSingleObject + a real CRITICAL_SECTION. N workers increment a shared
# counter under the lock; a correct total proves threads + mutual exclusion.
programs/win-thread/win-thread.exe: programs/win-thread/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -o $@ $<

# Track C/C18b: a STOCK console .exe using compiler thread-local storage
# (_Thread_local) across 4 threads. -fno-emulated-tls makes clang emit NATIVE
# Windows TLS (gs:[0x58] + _tls_index via the PE .tls directory) instead of the
# __emutls_get_address default; proves the loader's TLS template/_tls_index/
# ThreadLocalStoragePointer setup + per-thread blocks (per-thread isolation).
programs/win-tls18b/win-tls18b.exe: programs/win-tls18b/main.c
	$(CC) --target=x86_64-w64-mingw32 -fno-emulated-tls -O1 -o $@ $<

# Track C/C18d: a STOCK console .exe using shell32 -- SHGetFolderPathA /
# SHGetSpecialFolderPathA (CSIDL -> C:\ -> /data), ShellExecuteA, Shell_NotifyIcon,
# DragQueryFile. Proves the CSIDL path resolves end-to-end (writes a sentinel the
# kernel re-reads) + the launch/tray/drag shims.
programs/win-shell18d/win-shell18d.exe: programs/win-shell18d/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -o $@ $< -lshell32

# Track C/C24: a STOCK console .exe linked with --dynamicbase (ASLR) so the PE
# keeps its .reloc table + IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE. tobyOS loads it
# at a randomized 64KB-aligned slide and rewrites all absolute addresses via the
# base-reloc fixups; the program proves relocated CODE + DATA + self pointers
# still resolve (exit 24). --high-entropy-va is disabled to keep the slide inside
# the loader's verified user window.
programs/win-reloc24/win-reloc24.exe: programs/win-reloc24/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -o $@ $< \
	      -Wl,--dynamicbase -Wl,--disable-high-entropy-va

# Track C/C7: a STOCK Win32 GUI .exe (Windows subsystem) -- RegisterClass +
# CreateWindowEx + a GetMessage/DispatchMessage loop whose WndProc draws via
# BeginPaint/FillRect/TextOut. Proves the user32/gdi32 -> SYS_GUI_* bridge incl.
# the user-mode WndProc trampoline.
programs/win-gui/win-gui.exe: programs/win-gui/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -Wl,--subsystem,windows \
	      -o $@ $< -lgdi32 -luser32

# Track C/C8: a STOCK, INTERACTIVE Win32 GUI .exe -- same shape as win-gui but
# its WndProc handles real input: a click recolours the window (blue->green via
# CreateSolidBrush) and the close button runs WM_CLOSE->WM_DESTROY->PostQuitMessage.
# Proves tobyOS delivers mouse/keyboard/close as WM_* and the window is visible
# on the logged-in desktop.
programs/win-gui8/win-gui8.exe: programs/win-gui8/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -Wl,--subsystem,windows \
	      -o $@ $< -lgdi32 -luser32

# Track C/C9: a STOCK Windows .exe that resolves Win32 APIs at RUNTIME via
# GetModuleHandle/GetProcAddress/LoadLibrary and calls through the returned
# pointer -- the mechanism real Windows software uses for runtime API binding.
programs/win-c9/win-c9.exe: programs/win-c9/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -o $@ $<

# Track C/C10: a STOCK Win32 GUI .exe with TWO top-level windows sharing one
# WndProc that discriminates by HWND. Proves tobyOS routes each window's paint /
# input / close independently (per-window state table + per-message WndProc).
programs/win-gui10/win-gui10.exe: programs/win-gui10/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -Wl,--subsystem,windows \
	      -o $@ $< -lgdi32 -luser32

# Track C/C11: a STOCK console .exe exercising the EXTENDED Win32 file API
# (SetFilePointer/GetFileSize/DeleteFile/CreateDirectory/GetFileAttributes/
# FindFirstFile) on the C5 base.
programs/win-c11file/win-c11file.exe: programs/win-c11file/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -o $@ $<

# Track C/C11: a STOCK Win32 GUI .exe drawing richer GDI (pen line + Rectangle +
# Ellipse) plus a CHILD window owned by the main window. Proves more GDI + the
# 10-arg gate (hWndParent) + parent->child destroy cascade.
programs/win-gui11/win-gui11.exe: programs/win-gui11/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -Wl,--subsystem,windows \
	      -o $@ $< -lgdi32 -luser32

# Track C/C12: a STOCK Win32 GUI .exe that BitBlts an in-memory bitmap onto its
# window (CreateCompatibleDC + CreateBitmap + SelectObject + BitBlt).
programs/win-bmp12/win-bmp12.exe: programs/win-bmp12/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -Wl,--subsystem,windows \
	      -o $@ $< -lgdi32 -luser32

# Track C/C12: a STOCK Win32 GUI .exe using REAL system controls -- a BUTTON
# (click -> WM_COMMAND) and an EDIT (typed text, GetWindowText) -- which tobyOS
# draws + handles; the app never implements them.
programs/win-ctrl12/win-ctrl12.exe: programs/win-ctrl12/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -Wl,--subsystem,windows \
	      -o $@ $< -lgdi32 -luser32

# Track C/C13: a STOCK Win32 GUI .exe using STATIC/CHECKBOX/LISTBOX/SCROLLBAR
# controls (via SendMessage) + a MessageBoxA modal dialog.
programs/win-ctrl13/win-ctrl13.exe: programs/win-ctrl13/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -Wl,--subsystem,windows \
	      -o $@ $< -lgdi32 -luser32

# Track C/C13: a STOCK Win32 GUI .exe exercising GDI fonts + text metrics
# (CreateFont/SelectObject/GetTextExtentPoint32/SetTextColor/DrawText).
programs/win-font13/win-font13.exe: programs/win-font13/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -Wl,--subsystem,windows \
	      -o $@ $< -lgdi32 -luser32

# Track C/C14a: a STOCK Win32 GUI .exe exercising the new controls -- a GROUPBOX,
# two mutually-exclusive RADIO buttons, a COMBOBOX, and a SysTabControl32 TAB.
programs/win-ctrl14/win-ctrl14.exe: programs/win-ctrl14/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -Wl,--subsystem,windows \
	      -o $@ $< -lgdi32 -luser32

# Track C/C25: a STOCK Win32 GUI .exe exercising the comctl32 COMMON CONTROLS --
# a report-view SysListView32, a SysTreeView32, an msctls_statusbar32, an
# msctls_progress32 and a ToolbarWindow32 -- driven entirely through the real
# comctl32 API (InitCommonControlsEx + LVM_*/TVM_*/SB_*/PBM_*/TB_*) with every
# value read back to prove the round-trip. Links comctl32.
programs/win-cc25/win-cc25.exe: programs/win-cc25/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -Wl,--subsystem,windows \
	      -o $@ $< -lcomctl32 -lgdi32 -luser32

# Track C/C14a: a STOCK Win32 GUI .exe driven from a RESOURCE dialog template
# (DialogBoxParamA + RT_DIALOG in .rsrc + DialogProc + EndDialog).
programs/win-dlg14/win-dlg14.exe: programs/win-dlg14/main.c programs/win-dlg14/dlg.rc
	windres -i programs/win-dlg14/dlg.rc -o programs/win-dlg14/dlg_res.o --target=pe-x86-64
	$(CC) --target=x86_64-w64-mingw32 -O1 -Wl,--subsystem,windows \
	      -o $@ $< programs/win-dlg14/dlg_res.o -lgdi32 -luser32

# Track C/C14b: a STOCK Win32 GUI .exe drawing real TrueType text via the kernel
# rasterizer -- CreateFontA at several sizes + TextOutA/DrawTextA/GetTextExtent.
programs/win-font14b/win-font14b.exe: programs/win-font14b/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -Wl,--subsystem,windows \
	      -o $@ $< -lgdi32 -luser32

# Track C/C18c: a STOCK Win32 GUI .exe proving bold/italic TTF face selection --
# CreateFontA's lfWeight/lfItalic pick a distinct Lato face (Regular/Bold/Italic/
# BoldItalic) in the kernel rasterizer.
programs/win-font18c/win-font18c.exe: programs/win-font18c/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -Wl,--subsystem,windows \
	      -o $@ $< -lgdi32 -luser32

# Track C/C15: a STOCK Win32 GUI .exe exercising a menu bar, a timer, and a
# multi-button (Yes/No) MessageBox.
programs/win-menu15/win-menu15.exe: programs/win-menu15/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -Wl,--subsystem,windows \
	      -o $@ $< -lgdi32 -luser32

# Track C/C16a: a STOCK Win32 console .exe exercising the registry (advapi32),
# persisted across reboot.
programs/win-reg16/win-reg16.exe: programs/win-reg16/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -o $@ $< -ladvapi32

# Track C/C16b: a STOCK Win32 GUI .exe using the common Open file dialog (comdlg32).
programs/win-fdlg16/win-fdlg16.exe: programs/win-fdlg16/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -Wl,--subsystem,windows \
	      -o $@ $< -lgdi32 -luser32 -lcomdlg32

# Track C/C16c: a STOCK Win32 console .exe exercising command line + environment.
programs/win-env16/win-env16.exe: programs/win-env16/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -o $@ $<

# Track C/C16e: a STOCK multithreaded Win32 console .exe proving per-thread TLS.
programs/win-tls16/win-tls16.exe: programs/win-tls16/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -o $@ $<

# Track C/C17a: a STOCK Win32 console .exe -- a winsock TCP client (HTTP GET).
programs/win-net17/win-net17.exe: programs/win-net17/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -o $@ $< -lws2_32

# Track C/C17b: a STOCK Win32 console .exe -- a winsock TCP server (bind/listen/accept).
programs/win-srv17/win-srv17.exe: programs/win-srv17/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -o $@ $< -lws2_32

# Track C/C17c: a STOCK Win32 console .exe -- a winsock UDP echo server (sendto/recvfrom).
programs/win-udp17/win-udp17.exe: programs/win-udp17/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -o $@ $< -lws2_32

# Track C/C18a: a REAL off-the-shelf SQLite database engine (third_party/sqlite,
# unmodified public-domain amalgamation) built as a Windows PE. Standard embed
# build options; the engine source is verbatim upstream.
programs/win-sqlite/win-sqlite.exe: third_party/sqlite/sqlite3.c third_party/sqlite/shell.c
	@mkdir -p $(dir $@)
	$(CC) --target=x86_64-w64-mingw32 -O1 -Ithird_party/sqlite \
	      -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_MAX_MMAP_SIZE=0 \
	      -o $@ third_party/sqlite/shell.c third_party/sqlite/sqlite3.c

# Track C/C19b: REAL off-the-shelf Lua 5.4.7 interpreter (third_party/lua,
# unmodified upstream MIT source) built as a Windows PE. Compiles the interpreter
# front-end (lua.c) + the core/standard-library translation units; luac.c (the
# standalone bytecode-compiler tool) is excluded. luaconf.h auto-selects the
# LUA_USE_WINDOWS profile on the _WIN32 mingw target -- the source is unchanged.
LUA_SRCS := $(filter-out third_party/lua/luac.c, $(wildcard third_party/lua/*.c))
programs/win-lua/win-lua.exe: $(LUA_SRCS)
	@mkdir -p $(dir $@)
	$(CC) --target=x86_64-w64-mingw32 -O1 -Ithird_party/lua -o $@ $(LUA_SRCS)

# Track C/C19c: a first-party COM-ABI proof -- imports the REAL ole32 entry
# points (CoInitializeEx/CoCreateInstance/CoTaskMem*) and dispatches a coclass's
# methods through its vtable. Proves the genuine COM ABI against tobyOS's tightly
# scoped (one coclass/interface) ole32 shim.
programs/win-com19c/win-com19c.exe: programs/win-com19c/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -o $@ $< -lole32

# Track C/C20: float-return proof -- calls the REAL ucrt libm (sqrt/sin/pow/...),
# each returning a double in xmm0. -fno-builtin forces genuine imported calls
# (no inlined sqrtsd / constant fold) so the float-return gate is exercised.
programs/win-math20/win-math20.exe: programs/win-math20/main.c
	$(CC) --target=x86_64-w64-mingw32 -O1 -fno-builtin -o $@ $<

# ---------- Milestone 25D: dynamic-linked sample (c_dynhello) ----------
#
# Same source-shape as the other libtoby samples, but built as a
# PIE that loads /lib/ld-toby.so as its interpreter and pulls
# /lib/libtoby.so for libc.
#
# Compile flags: USER_CFLAGS minus -fno-pie/-fno-pic, plus -fPIC.
# Build a PIE crt0 by reassembling the same crt0.S with -fPIE so
# every `call` against an external symbol becomes R_X86_64_PLT32
# (which the linker turns into a PLT entry + JUMP_SLOT reloc).
#
# Link flags:
#   -pie                              produce ET_DYN executable
#   -dynamic-linker /lib/ld-toby.so   sets PT_INTERP the kernel
#                                     reads to find our linker
#   --no-undefined                    catch missing libtoby symbols
#                                     at link time, not at load time
#   -z now                            BIND_NOW for libc references
#                                     (ld-toby.so doesn't lazy-bind)
#
# Pass libtoby.so directly as a positional input so lld matches its
# SONAME and records DT_NEEDED=libtoby.so. We do NOT add a runtime
# search path -- ld-toby.so always looks under /lib/.

LIBTOBY_PIE_CFLAGS := --target=x86_64-elf \
                      -ffreestanding -fno-stack-protector \
                      -fno-unwind-tables -fno-asynchronous-unwind-tables \
                      -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
                      -fPIC -nostdlib -O2 -Wall -Wextra -std=c11 \
                      -isystem $(LIBTOBY_INC)

LIBTOBY_SCRT0      := $(LIBTOBY_DIR)/Scrt0.o

$(LIBTOBY_SCRT0): $(LIBTOBY_SRC)/crt0.S
	$(CC) $(LIBTOBY_PIE_CFLAGS) -c $< -o $@

programs/c_dynhello/c_dynhello.o: programs/c_dynhello/main.c $(LIBTOBY_SO)
	$(CC) $(LIBTOBY_PIE_CFLAGS) -c $< -o $@

programs/c_dynhello/c_dynhello.elf: programs/c_dynhello/c_dynhello.o \
                                    $(LIBTOBY_SCRT0) \
                                    $(LIBTOBY_SO) \
                                    $(LDTOBY_SO)
	MSYS2_ARG_CONV_EXCL='--dynamic-linker' MSYS_NO_PATHCONV=1 \
	$(LD) -m elf_x86_64 -nostdlib -pie -z max-page-size=0x1000 -z now \
	      --allow-shlib-undefined --dynamic-linker=/lib/ld-toby.so \
	      -o $@ $(LIBTOBY_SCRT0) programs/c_dynhello/c_dynhello.o \
	      $(LIBTOBY_SO)

# Convenience targets so 'make libtoby' / 'make c-samples' work too.
.PHONY: libtoby c-samples
libtoby: $(LIBTOBY_AR) $(LIBTOBY_CRT0) $(LIBTOBY_SO) $(LIBTOBY_SCRT0)
c-samples: programs/c_hello/c_hello.elf \
           programs/c_args/c_args.elf \
           programs/c_filedemo/c_filedemo.elf \
           programs/c_alloctest/c_alloctest.elf \
           programs/c_envtest/c_envtest.elf \
           programs/c_dynhello/c_dynhello.elf

# ---------- Milestone 25E: ports/sbase/* userland tools (rules) ----------
#
# Each tool lives at ports/sbase/<name>.c, links against the shared
# ports/sbase/util.o helper, libtoby.a (static), and our common
# crt0.o; it uses ports/sbase/program.ld (a copy of c_hello's).
#
# We deliberately keep these static so the ports stress libtoby's
# main APIs (open/read/write/stat/opendir/getopt/printf) rather than
# also stressing the dynamic linker; M25D already validated dynamic
# linking with c_dynhello.
#
# PORTS_SBASE_DIR / PORTS_SBASE_TOOLS / PORTS_SBASE_ELFS are forward-
# declared near the top of the Makefile so PROGRAMS can reference them
# while still being a := assignment.
PORTS_SBASE_LD     := $(PORTS_SBASE_DIR)/program.ld
PORTS_SBASE_UTIL_O := $(PORTS_SBASE_DIR)/util.o
PORTS_SBASE_OBJS   := $(addprefix $(PORTS_SBASE_DIR)/,$(addsuffix .o,$(PORTS_SBASE_TOOLS)))

$(PORTS_SBASE_DIR)/util.o: $(PORTS_SBASE_DIR)/util.c $(PORTS_SBASE_DIR)/util.h $(LIBTOBY_AR)
	$(CC) $(LIBTOBY_CFLAGS) -I$(PORTS_SBASE_DIR) -c $< -o $@

$(PORTS_SBASE_DIR)/%.o: $(PORTS_SBASE_DIR)/%.c $(PORTS_SBASE_DIR)/util.h $(LIBTOBY_AR)
	$(CC) $(LIBTOBY_CFLAGS) -I$(PORTS_SBASE_DIR) -c $< -o $@

$(PORTS_SBASE_DIR)/%.elf: $(PORTS_SBASE_DIR)/%.o $(PORTS_SBASE_UTIL_O) \
                          $(LIBTOBY_CRT0) $(LIBTOBY_AR) $(PORTS_SBASE_LD)
	$(LD) $(PROG_LDFLAGS) -T $(PORTS_SBASE_LD) -o $@ \
	      $(LIBTOBY_CRT0) $< $(PORTS_SBASE_UTIL_O) $(LIBTOBY_AR)

.PHONY: ports
ports: $(PORTS_SBASE_ELFS)

programs/user_netecho/user_netecho.o: programs/user_netecho/main.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

programs/user_netecho/user_netecho.elf: programs/user_netecho/user_netecho.o \
                                        programs/common/start.o \
                                        programs/user_netecho/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/user_netecho/program.ld -o $@ \
	      programs/common/start.o programs/user_netecho/user_netecho.o

# Migrated to TobyTK: the demo app is now the TobyTK widget gallery, so it
# links libtoby (crt0 + libtoby.a) like gui_calc / gui_settings.
programs/user_gui_demo/user_gui_demo.o: programs/user_gui_demo/main.c \
                                        $(LIBTOBY_AR)
	$(CC) $(LIBTOBY_CFLAGS) -I include -c $< -o $@

programs/user_gui_demo/user_gui_demo.elf: programs/user_gui_demo/user_gui_demo.o \
                                          $(LIBTOBY_CRT0) \
                                          $(LIBTOBY_AR) \
                                          programs/user_gui_demo/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/user_gui_demo/program.ld -o $@ \
	      $(LIBTOBY_CRT0) programs/user_gui_demo/user_gui_demo.o $(LIBTOBY_AR)

# X2 capstone: native tobyOS GUI tail of the cross-personality pipeline.
programs/x2gui/x2gui.o: programs/x2gui/main.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

programs/x2gui/x2gui.elf: programs/x2gui/x2gui.o \
                          programs/common/start.o \
                          programs/x2gui/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/x2gui/program.ld -o $@ \
	      programs/common/start.o programs/x2gui/x2gui.o

# Clock -- migrated to TobyTK (crt0 + libtoby.a).
$(eval $(call LIBTOBY_PROGRAM_RULES,user_gui_clock,user_gui_clock))

# NOTE: the old user-mode toby_gui toolkit is gone -- every in-tree app and the
# SDK now use TobyTK (libtoby/include/toby/tk.h, inside libtoby.a).

# Notes app -- migrated to TobyTK: links libtoby (crt0 + libtoby.a) like
# every other native app instead of the old start.o + toby_gui.o.
$(eval $(call LIBTOBY_PROGRAM_RULES,user_gui_widgets,user_gui_widgets))

# Migrated to TobyTK: links libtoby (crt0 + libtoby.a) like gui_calc.
programs/user_gui_about/user_gui_about.o: programs/user_gui_about/main.c \
                                          $(LIBTOBY_AR)
	$(CC) $(LIBTOBY_CFLAGS) -I include -c $< -o $@

programs/user_gui_about/user_gui_about.elf: \
        programs/user_gui_about/user_gui_about.o \
        $(LIBTOBY_CRT0) \
        $(LIBTOBY_AR) \
        programs/user_gui_about/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/user_gui_about/program.ld -o $@ \
	      $(LIBTOBY_CRT0) programs/user_gui_about/user_gui_about.o $(LIBTOBY_AR)

# Milestone-13 GUI apps: terminal emulator, file manager, text viewer.
# Each uses raw syscall stubs (no toby_gui toolkit) because they build
# their own custom layouts (text grid / scrollable list / scrolling
# line viewer).

# Terminal -- migrated to TobyTK (crt0 + libtoby.a); uses tk_draw_text_mono.
$(eval $(call LIBTOBY_PROGRAM_RULES,user_gui_term,user_gui_term))

# File Explorer -- migrated to TobyTK (crt0 + libtoby.a).
$(eval $(call LIBTOBY_PROGRAM_RULES,user_gui_files,user_gui_files))

# File viewer (text/hex) -- migrated to TobyTK; monospace via tk_draw_text_mono.
$(eval $(call LIBTOBY_PROGRAM_RULES,user_gui_viewer,user_gui_viewer))

# ---------- QuickJS (M-JS: browser JavaScript engine, phase 9) ----------
# Vendored Bellard QuickJS 2025-04-26 (third_party/quickjs), compiled
# freestanding against libtoby. FP-heavy like math.c/stdio.c, so it
# builds -msse -msse2 (the kernel FXSAVEs user FP state). Atomics are
# opted out (single-threaded browser; no pthread dependency).
QJS_DIR  := third_party/quickjs
QJS_OBJS := $(QJS_DIR)/quickjs.o $(QJS_DIR)/libregexp.o \
            $(QJS_DIR)/libunicode.o $(QJS_DIR)/cutils.o $(QJS_DIR)/dtoa.o
QJS_CFLAGS := $(LIBTOBY_CFLAGS) -msse -msse2 \
              -DCONFIG_VERSION='"2025-04-26"' -DQJS_NO_ATOMICS -D__TOBYOS__ \
              -Dalloca=__builtin_alloca \
              -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable \
              -Wno-unused-but-set-variable -Wno-implicit-fallthrough

$(QJS_DIR)/%.o: $(QJS_DIR)/%.c $(LIBTOBY_AR)
	$(CC) $(QJS_CFLAGS) -c $< -o $@

# Web browser -- TobyTK shell + DOM/CSS engine + QuickJS. main.c builds
# -msse (it exchanges doubles with QuickJS) and sees the QuickJS API.
programs/user_gui_browser/user_gui_browser.o: programs/user_gui_browser/main.c $(LIBTOBY_AR)
	$(CC) $(LIBTOBY_CFLAGS) -msse -msse2 -I $(QJS_DIR) $(BROWSER_EXTRA) -c $< -o $@

programs/user_gui_browser/user_gui_browser.elf: programs/user_gui_browser/user_gui_browser.o \
                        $(QJS_OBJS) \
                        $(LIBTOBY_CRT0) \
                        $(LIBTOBY_AR) \
                        programs/user_gui_browser/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/user_gui_browser/program.ld -o $@ \
	      $(LIBTOBY_CRT0) programs/user_gui_browser/user_gui_browser.o \
	      $(QJS_OBJS) $(LIBTOBY_AR)

# Milestone-14 GUI login screen. Migrated to TobyTK: links libtoby
# (crt0 + libtoby.a) like every other native app instead of the old
# start.o + toby_gui.o. The settings get/login/opacity syscall stubs
# remain inline in its main.c.
$(eval $(call LIBTOBY_PROGRAM_RULES,user_login,user_login))

# GUI-framework Milestone 1: the Settings app is now a LIBTOBY_PROGRAM built on
# the TobyTK toolkit (toby/tk.h) -- it links crt0.o + libtoby.a like every other
# native app, instead of the old start.o + toby_gui.o.
$(eval $(call LIBTOBY_PROGRAM_RULES,user_gui_settings,user_gui_settings))

# Milestone-16 packaged app: built with the same flags as every other
# GUI program, but NOT dropped into /bin/ at initrd time. Instead the
# `mkpkg` host tool wraps the built ELF into a .tpkg that ships inside
# initrd/repo/, and the user runs `pkg install helloapp` to extract it
# onto /data/apps/.

# Migrated to TobyTK: links libtoby (crt0 + libtoby.a) like gui_calc.
programs/user_gui_helloapp/user_gui_helloapp.o: \
        programs/user_gui_helloapp/main.c \
        $(LIBTOBY_AR)
	$(CC) $(LIBTOBY_CFLAGS) -I include -c $< -o $@

programs/user_gui_helloapp/user_gui_helloapp.elf: \
        programs/user_gui_helloapp/user_gui_helloapp.o \
        $(LIBTOBY_CRT0) \
        $(LIBTOBY_AR) \
        programs/user_gui_helloapp/program.ld
	$(LD) $(PROG_LDFLAGS) -T programs/user_gui_helloapp/program.ld -o $@ \
	      $(LIBTOBY_CRT0) programs/user_gui_helloapp/user_gui_helloapp.o $(LIBTOBY_AR)

# Task Manager -- migrated to TobyTK (crt0 + libtoby.a). Needs <tobyos/abi/abi.h>?
# No: it declares its own syscall numbers inline, so the plain rule suffices.
$(eval $(call LIBTOBY_PROGRAM_RULES,user_gui_taskmgr,user_gui_taskmgr))

# Text editor -- migrated to TobyTK (crt0 + libtoby.a); monospace via tk_draw_text_mono.
$(eval $(call LIBTOBY_PROGRAM_RULES,user_gui_edit,user_gui_edit))

# ---------- Build initramfs (USTAR tar) ----------
#
# The initramfs is just a POSIX tar file containing the runtime
# filesystem the kernel will see at /. We:
#   1. Wipe + re-create build/initrd/ so stale files never sneak in.
#   2. Copy the source-controlled bits (initrd/etc/motd, initrd/readme.txt).
#   3. Drop programs/user_hello/user_hello.elf in as /bin/hello.
#   4. tar everything up using --format=ustar (so the kernel parser
#      sees the canonical 512-byte header layout).
#
# The .gitkeep file in initrd/bin/ is excluded -- it only exists so
# the bin/ directory survives in source control.
#
# MODES ARE FORCED AT TAR TIME, in two passes, because the host filesystem
# cannot carry them: this tree is built under MSYS on Windows, where
# `chmod 755` on a staged file is silently a no-op (verified -- `ls -l`
# still reports 0644 afterwards). Every binary was therefore tar'd as 0644
# owned by the BUILD USER, and src/ramfs.c papered over that by reporting a
# fabricated 00444 for every file and 00555 for every directory, ignoring
# the header entirely. ramfs now reads the real header, so the header has to
# be true:
#   pass 1 (-c)  bin/ + lib/ + $$EXTRA_FILES -> 0755 root:root -- executables,
#                shared objects, and the staged chrome/alpine trees
#   pass 2 (-r)  etc/ + repo/ + system/ + m36/ + readme.txt -> 0644 root:root
# --numeric-owner keeps this working on a host with no `root` account.
#
# The two passes MUST NOT OVERLAP: ramfs's find_node() returns the FIRST
# matching entry, so a path present in both passes would silently resolve to
# whichever mode was archived first. If you add a file here, put it in
# exactly one list. `tar -tf` before and after should differ only in modes.
#
# KNOWN OVER-APPROXIMATION, stated rather than hidden: $$EXTRA_FILES is whole
# staged TREES (opt/chrome, the alpine rootfs, the OCI bundle, usr/, lib64/),
# and the host cannot tell their binaries from their data -- everything in
# them lands on disk 0644, chrome-headless-shell included. So pass 1 marks
# that entire subtree 0755. Erring the other way would leave the actual
# executables non-executable, which is the worse failure; the cost is that
# e.g. opt/chrome/*.pak is reported executable. Files tobyOS itself builds
# (bin/, lib/, etc/, repo/, ...) are exact -- this caveat is only the
# opt-in third-party trees.

$(INITRD_TAR): $(PROGRAMS) programs/linux-hello/linux-hello.elf \
               programs/linux-xdg-settings/linux-xdg-settings.elf \
               programs/linux-crashpad/linux-crashpad.elf \
               programs/linux-sigtest/linux-sigtest.elf \
               programs/linux-mmaptest/linux-mmaptest.elf \
               programs/linux-thread/linux-thread.elf \
               programs/linux-fb/linux-fb.elf \
               programs/linux-fbwin/linux-fbwin.elf \
               programs/linux-join/linux-join.elf \
               programs/linux-poll/linux-poll.elf \
               programs/linux-netserver/linux-netserver.elf \
               programs/linux-siginfo/linux-siginfo.elf \
               programs/linux-httpget/linux-httpget.elf \
               programs/linux-msgsock/linux-msgsock.elf \
               programs/linux-static-auto/linux-static-auto.elf \
               programs/linux-futex/linux-futex.elf \
               programs/linux-scmsg/linux-scmsg.elf \
               programs/linux-eventfd/linux-eventfd.elf \
               programs/linux-mapshare/linux-mapshare.elf \
               programs/linux-demrace/linux-demrace.elf \
               programs/linux-qfreeze/linux-qfreeze.elf \
               programs/linux-drmtest/linux-drmtest.elf \
               programs/linux-sndtest/linux-sndtest.elf \
               programs/linux-mlkem/linux-mlkem.elf \
               programs/linux-xstub/linux-xstub.elf \
               programs/linux-procfs/linux-procfs.elf \
               programs/linux-tty/linux-tty.elf \
               programs/linux-pty/linux-pty.elf \
               programs/linux-procfs2/linux-procfs2.elf \
               programs/win-hello/win-hello.exe \
               programs/win-crt/win-crt.exe \
               programs/win-hello3/win-hello3.exe \
               programs/win-cpp/win-cpp.exe \
               programs/win-fileio/win-fileio.exe \
               programs/win-thread/win-thread.exe \
               programs/win-tls18b/win-tls18b.exe \
               programs/win-shell18d/win-shell18d.exe \
               programs/win-gui/win-gui.exe \
               programs/win-gui8/win-gui8.exe \
               programs/win-c9/win-c9.exe \
               programs/win-gui10/win-gui10.exe \
               programs/win-c11file/win-c11file.exe \
               programs/win-gui11/win-gui11.exe \
               programs/win-bmp12/win-bmp12.exe \
               programs/win-ctrl12/win-ctrl12.exe \
               programs/win-ctrl13/win-ctrl13.exe \
               programs/win-font13/win-font13.exe \
               programs/win-ctrl14/win-ctrl14.exe \
               programs/win-cc25/win-cc25.exe \
               programs/win-dlg14/win-dlg14.exe \
               programs/win-font14b/win-font14b.exe \
               programs/win-menu15/win-menu15.exe \
               programs/win-reg16/win-reg16.exe \
               programs/win-fdlg16/win-fdlg16.exe \
               programs/win-env16/win-env16.exe \
               programs/win-tls16/win-tls16.exe \
               programs/win-net17/win-net17.exe \
               programs/win-srv17/win-srv17.exe \
               programs/win-udp17/win-udp17.exe \
               programs/win-sqlite/win-sqlite.exe \
               programs/win-font18c/win-font18c.exe \
               programs/win-lua/win-lua.exe \
               programs/win-com19c/win-com19c.exe \
               programs/win-math20/win-math20.exe \
               programs/win-printf21/win-printf21.exe \
               programs/win-scan22/win-scan22.exe \
               programs/win-wide23/win-wide23.exe \
               programs/win-reloc24/win-reloc24.exe \
               programs/win-xprod/win-xprod.exe \
               programs/win-xcons/win-xcons.exe \
               programs/win-xfilter/win-xfilter.exe \
                initrd/etc/c19b.lua \
                initrd/etc/c20.lua \
                initrd/etc/c21.lua \
               initrd/etc/motd initrd/etc/anim.html initrd/etc/webgl.html initrd/etc/input.html initrd/etc/input2.html initrd/etc/fonttest.html initrd/etc/audiotone.html initrd/etc/start.html initrd/etc/egl_vendor.json \
               initrd/readme.txt initrd/etc/Lato-Regular.ttf \
               initrd/etc/Lato-Bold.ttf initrd/etc/Lato-Italic.ttf \
               initrd/etc/Lato-BoldItalic.ttf initrd/etc/fallback.otf \
               initrd/etc/emoji.ttf \
               initrd/etc/DejaVuSerif.ttf initrd/etc/DejaVuSerif-Bold.ttf \
               initrd/etc/DejaVuSansMono.ttf initrd/etc/DejaVuSansMono-Bold.ttf \
               initrd/etc/drvmatch.conf initrd/etc/drvmatch.conf.test \
               initrd/etc/lua_selftest.lua initrd/etc/resolv.conf \
               initrd/etc/fonts.conf \
               $(HELLOAPP_TPKG) $(HELLOAPP_TPKG_V2) $(SDK_SAMPLE_TPKGS) \
               $(HELLOAPP_TPKG_CORRUPT) $(HELLOAPP_TPKG_BADHASH) \
               $(HELLOAPP_TPKG_NOHASH)  $(HELLOAPP_TPKG_BADFHASH) \
               $(HELLOAPP_TPKG_V2_CORRUPT) \
               $(TRUST_DB) \
               $(HELLOAPP_TPKG_SIGNED) $(HELLOAPP_TPKG_SIG_UNKNOWN) \
               $(HELLOAPP_TPKG_SIG_TAMPER) \
               $(HELLOAPP_TPKG_CAPS_RO) \
               initrd/m36/m36_part2.c initrd/m36/m36_sample.c initrd/m36/Tobyfile \
               $(SHPARITY_CASES)
	rm -rf $(INITRD_STAGE)
	mkdir -p $(INITRD_STAGE)/bin $(INITRD_STAGE)/etc $(INITRD_STAGE)/repo $(INITRD_STAGE)/lib \
	         $(INITRD_STAGE)/system/keys $(INITRD_STAGE)/m36
	cp initrd/etc/motd                       $(INITRD_STAGE)/etc/motd
	cp initrd/etc/anim.html                  $(INITRD_STAGE)/etc/anim.html
	cp initrd/etc/webgl.html                 $(INITRD_STAGE)/etc/webgl.html
	cp initrd/etc/input.html                 $(INITRD_STAGE)/etc/input.html
	cp initrd/etc/input2.html                $(INITRD_STAGE)/etc/input2.html
	cp initrd/etc/fonttest.html             $(INITRD_STAGE)/etc/fonttest.html
	cp initrd/etc/audiotone.html            $(INITRD_STAGE)/etc/audiotone.html
	cp initrd/etc/start.html                $(INITRD_STAGE)/etc/start.html
	cp initrd/etc/egl_vendor.json            $(INITRD_STAGE)/etc/egl_vendor.json
	cp initrd/etc/Lato-Regular.ttf           $(INITRD_STAGE)/etc/Lato-Regular.ttf
	cp initrd/etc/Lato-Bold.ttf              $(INITRD_STAGE)/etc/Lato-Bold.ttf
	cp initrd/etc/Lato-Italic.ttf            $(INITRD_STAGE)/etc/Lato-Italic.ttf
	cp initrd/etc/Lato-BoldItalic.ttf        $(INITRD_STAGE)/etc/Lato-BoldItalic.ttf
	cp initrd/etc/fallback.otf               $(INITRD_STAGE)/etc/fallback.otf
	cp initrd/etc/emoji.ttf                  $(INITRD_STAGE)/etc/emoji.ttf
	cp initrd/etc/DejaVuSerif.ttf            $(INITRD_STAGE)/etc/DejaVuSerif.ttf
	cp initrd/etc/DejaVuSerif-Bold.ttf       $(INITRD_STAGE)/etc/DejaVuSerif-Bold.ttf
	cp initrd/etc/DejaVuSansMono.ttf         $(INITRD_STAGE)/etc/DejaVuSansMono.ttf
	cp initrd/etc/DejaVuSansMono-Bold.ttf    $(INITRD_STAGE)/etc/DejaVuSansMono-Bold.ttf
	cp initrd/etc/drvmatch.conf              $(INITRD_STAGE)/etc/drvmatch.conf
	cp initrd/etc/drvmatch.conf.test         $(INITRD_STAGE)/etc/drvmatch.conf.test
	cp initrd/etc/lua_selftest.lua           $(INITRD_STAGE)/etc/lua_selftest.lua
	cp initrd/etc/resolv.conf                $(INITRD_STAGE)/etc/resolv.conf
	mkdir -p $(INITRD_STAGE)/etc/fonts
	cp initrd/etc/fonts.conf                 $(INITRD_STAGE)/etc/fonts/fonts.conf
	cp initrd/readme.txt                     $(INITRD_STAGE)/readme.txt
	cp programs/user_hello/user_hello.elf    $(INITRD_STAGE)/bin/hello
	cp programs/user_bad/user_bad.elf        $(INITRD_STAGE)/bin/bad
	cp programs/user_echo/user_echo.elf      $(INITRD_STAGE)/bin/echo
	cp programs/user_cat/user_cat.elf        $(INITRD_STAGE)/bin/cat
	cp programs/user_long/user_long.elf      $(INITRD_STAGE)/bin/long_task
	cp programs/user_netecho/user_netecho.elf $(INITRD_STAGE)/bin/netecho
	cp programs/user_gui_demo/user_gui_demo.elf   $(INITRD_STAGE)/bin/gui_demo
	cp programs/x2gui/x2gui.elf                   $(INITRD_STAGE)/bin/x2gui
	cp programs/user_gui_clock/user_gui_clock.elf $(INITRD_STAGE)/bin/gui_clock
	cp programs/user_gui_widgets/user_gui_widgets.elf $(INITRD_STAGE)/bin/gui_widgets
	cp programs/user_gui_about/user_gui_about.elf     $(INITRD_STAGE)/bin/gui_about
	cp programs/user_cpptest/user_cpptest.elf         $(INITRD_STAGE)/bin/cpptest
	cp programs/user_oh264test/user_oh264test.elf     $(INITRD_STAGE)/bin/oh264test
	cp programs/user_bsdroute/user_bsdroute.elf       $(INITRD_STAGE)/bin/bsdroute
	cp programs/user_mp4play/user_mp4play.elf         $(INITRD_STAGE)/bin/mp4play
	cp programs/user_stltest/user_stltest.elf         $(INITRD_STAGE)/bin/stltest
	cp programs/user_av1test/user_av1test.elf         $(INITRD_STAGE)/bin/av1test
	cp programs/user_aactest/user_aactest.elf         $(INITRD_STAGE)/bin/aactest
	cp programs/user_wasmtest/user_wasmtest.elf       $(INITRD_STAGE)/bin/wasmtest
	cp programs/user_gui_term/user_gui_term.elf       $(INITRD_STAGE)/bin/gui_term
	cp programs/user_gui_files/user_gui_files.elf     $(INITRD_STAGE)/bin/gui_files
	cp programs/user_gui_viewer/user_gui_viewer.elf   $(INITRD_STAGE)/bin/gui_viewer
	cp programs/user_gui_browser/user_gui_browser.elf $(INITRD_STAGE)/bin/gui_browser
	cp programs/user_login/user_login.elf             $(INITRD_STAGE)/bin/login
	cp programs/user_gui_settings/user_gui_settings.elf $(INITRD_STAGE)/bin/gui_settings
	cp programs/user_gui_taskmgr/user_gui_taskmgr.elf $(INITRD_STAGE)/bin/gui_taskmgr
	cp programs/user_gui_edit/user_gui_edit.elf       $(INITRD_STAGE)/bin/gui_edit
	cp programs/user_abi_test/user_abi_test.elf       $(INITRD_STAGE)/bin/abi_test
	cp programs/c_hello/c_hello.elf                   $(INITRD_STAGE)/bin/c_hello
	cp programs/nsetuid/nsetuid.elf                   $(INITRD_STAGE)/bin/nsetuid
	cp programs/c_args/c_args.elf                     $(INITRD_STAGE)/bin/c_args
	cp programs/c_filedemo/c_filedemo.elf             $(INITRD_STAGE)/bin/c_filedemo
	cp programs/c_alloctest/c_alloctest.elf           $(INITRD_STAGE)/bin/c_alloctest
	cp programs/c_envtest/c_envtest.elf               $(INITRD_STAGE)/bin/c_envtest
	cp programs/c_dynhello/c_dynhello.elf             $(INITRD_STAGE)/bin/c_dynhello
	cp programs/linux-hello/linux-hello.elf           $(INITRD_STAGE)/bin/linux-hello
	cp programs/linux-sigtest/linux-sigtest.elf       $(INITRD_STAGE)/bin/linux-sigtest
	cp programs/linux-mmaptest/linux-mmaptest.elf     $(INITRD_STAGE)/bin/linux-mmaptest
	cp programs/linux-thread/linux-thread.elf         $(INITRD_STAGE)/bin/linux-thread
	cp programs/linux-fb/linux-fb.elf                 $(INITRD_STAGE)/bin/linux-fb
	cp programs/linux-fbwin/linux-fbwin.elf           $(INITRD_STAGE)/bin/linux-fbwin
	cp programs/linux-join/linux-join.elf             $(INITRD_STAGE)/bin/linux-join
	cp programs/linux-poll/linux-poll.elf             $(INITRD_STAGE)/bin/linux-poll
	cp programs/linux-netserver/linux-netserver.elf   $(INITRD_STAGE)/bin/linux-netserver
	cp programs/linux-siginfo/linux-siginfo.elf       $(INITRD_STAGE)/bin/linux-siginfo
	cp programs/linux-httpget/linux-httpget.elf       $(INITRD_STAGE)/bin/linux-httpget
	cp programs/linux-msgsock/linux-msgsock.elf       $(INITRD_STAGE)/bin/linux-msgsock
	cp programs/linux-static-auto/linux-static-auto.elf $(INITRD_STAGE)/bin/linux-static-auto
	cp programs/linux-procfs/linux-procfs.elf         $(INITRD_STAGE)/bin/linux-procfs
	cp programs/linux-futex/linux-futex.elf           $(INITRD_STAGE)/bin/linux-futex
	cp programs/linux-scmsg/linux-scmsg.elf           $(INITRD_STAGE)/bin/linux-scmsg
	cp programs/linux-eventfd/linux-eventfd.elf       $(INITRD_STAGE)/bin/linux-eventfd
	cp programs/linux-mapshare/linux-mapshare.elf     $(INITRD_STAGE)/bin/linux-mapshare
	cp programs/linux-demrace/linux-demrace.elf       $(INITRD_STAGE)/bin/linux-demrace
	cp programs/linux-qfreeze/linux-qfreeze.elf       $(INITRD_STAGE)/bin/linux-qfreeze
	cp programs/linux-drmtest/linux-drmtest.elf       $(INITRD_STAGE)/bin/linux-drmtest
	cp programs/linux-sndtest/linux-sndtest.elf       $(INITRD_STAGE)/bin/linux-sndtest
	cp programs/linux-mlkem/linux-mlkem.elf           $(INITRD_STAGE)/bin/linux-mlkem
	cp programs/linux-xstub/linux-xstub.elf           $(INITRD_STAGE)/bin/linux-xstub
	cp programs/linux-tty/linux-tty.elf               $(INITRD_STAGE)/bin/linux-tty
	cp programs/linux-pty/linux-pty.elf               $(INITRD_STAGE)/bin/linux-pty
	cp programs/linux-procfs2/linux-procfs2.elf       $(INITRD_STAGE)/bin/linux-procfs2
	@# B25 (opt-in): the real glibc static proof is built out-of-tree against a
	@# glibc sysroot (programs/linux-glibc/build.sh) since the in-tree clang is
	@# freestanding. Stage it only when present so normal builds never require it.
	@test -f programs/linux-glibc/linux-glibc.elf \
	  && cp programs/linux-glibc/linux-glibc.elf      $(INITRD_STAGE)/bin/linux-glibc \
	  && echo "[initrd] staged linux-glibc (B25 real glibc proof)" \
	  || echo "[initrd] linux-glibc.elf absent (B25 opt-in) -- skipping"
	cp programs/win-hello/win-hello.exe               $(INITRD_STAGE)/bin/win-hello.exe
	cp programs/win-crt/win-crt.exe                   $(INITRD_STAGE)/bin/win-crt.exe
	cp programs/win-hello3/win-hello3.exe             $(INITRD_STAGE)/bin/win-hello3.exe
	cp programs/win-cpp/win-cpp.exe                   $(INITRD_STAGE)/bin/win-cpp.exe
	cp programs/win-fileio/win-fileio.exe             $(INITRD_STAGE)/bin/win-fileio.exe
	cp programs/win-thread/win-thread.exe             $(INITRD_STAGE)/bin/win-thread.exe
	cp programs/win-tls18b/win-tls18b.exe             $(INITRD_STAGE)/bin/win-tls18b.exe
	cp programs/win-shell18d/win-shell18d.exe         $(INITRD_STAGE)/bin/win-shell18d.exe
	cp programs/win-gui/win-gui.exe                   $(INITRD_STAGE)/bin/win-gui.exe
	cp programs/win-gui8/win-gui8.exe                 $(INITRD_STAGE)/bin/win-gui8.exe
	cp programs/win-c9/win-c9.exe                     $(INITRD_STAGE)/bin/win-c9.exe
	cp programs/win-gui10/win-gui10.exe               $(INITRD_STAGE)/bin/win-gui10.exe
	cp programs/win-c11file/win-c11file.exe           $(INITRD_STAGE)/bin/win-c11file.exe
	cp programs/win-gui11/win-gui11.exe               $(INITRD_STAGE)/bin/win-gui11.exe
	cp programs/win-bmp12/win-bmp12.exe               $(INITRD_STAGE)/bin/win-bmp12.exe
	cp programs/win-ctrl12/win-ctrl12.exe             $(INITRD_STAGE)/bin/win-ctrl12.exe
	cp programs/win-ctrl13/win-ctrl13.exe             $(INITRD_STAGE)/bin/win-ctrl13.exe
	cp programs/win-font13/win-font13.exe             $(INITRD_STAGE)/bin/win-font13.exe
	cp programs/win-ctrl14/win-ctrl14.exe             $(INITRD_STAGE)/bin/win-ctrl14.exe
	cp programs/win-cc25/win-cc25.exe                 $(INITRD_STAGE)/bin/win-cc25.exe
	cp programs/win-dlg14/win-dlg14.exe               $(INITRD_STAGE)/bin/win-dlg14.exe
	cp programs/win-font14b/win-font14b.exe           $(INITRD_STAGE)/bin/win-font14b.exe
	cp programs/win-menu15/win-menu15.exe             $(INITRD_STAGE)/bin/win-menu15.exe
	cp programs/win-reg16/win-reg16.exe               $(INITRD_STAGE)/bin/win-reg16.exe
	cp programs/win-fdlg16/win-fdlg16.exe             $(INITRD_STAGE)/bin/win-fdlg16.exe
	cp programs/win-env16/win-env16.exe               $(INITRD_STAGE)/bin/win-env16.exe
	cp programs/win-tls16/win-tls16.exe               $(INITRD_STAGE)/bin/win-tls16.exe
	cp programs/win-net17/win-net17.exe               $(INITRD_STAGE)/bin/win-net17.exe
	cp programs/win-srv17/win-srv17.exe               $(INITRD_STAGE)/bin/win-srv17.exe
	cp programs/win-udp17/win-udp17.exe               $(INITRD_STAGE)/bin/win-udp17.exe
	cp programs/win-sqlite/win-sqlite.exe             $(INITRD_STAGE)/bin/win-sqlite.exe
	cp programs/win-font18c/win-font18c.exe           $(INITRD_STAGE)/bin/win-font18c.exe
	cp programs/win-lua/win-lua.exe                   $(INITRD_STAGE)/bin/win-lua.exe
	cp programs/win-com19c/win-com19c.exe             $(INITRD_STAGE)/bin/win-com19c.exe
	cp programs/win-math20/win-math20.exe             $(INITRD_STAGE)/bin/win-math20.exe
	cp programs/win-printf21/win-printf21.exe         $(INITRD_STAGE)/bin/win-printf21.exe
	cp programs/win-scan22/win-scan22.exe             $(INITRD_STAGE)/bin/win-scan22.exe
	cp programs/win-wide23/win-wide23.exe             $(INITRD_STAGE)/bin/win-wide23.exe
	cp programs/win-reloc24/win-reloc24.exe           $(INITRD_STAGE)/bin/win-reloc24.exe
	cp programs/win-xprod/win-xprod.exe               $(INITRD_STAGE)/bin/win-xprod.exe
	cp programs/win-xcons/win-xcons.exe               $(INITRD_STAGE)/bin/win-xcons.exe
	cp programs/win-xfilter/win-xfilter.exe           $(INITRD_STAGE)/bin/win-xfilter.exe
	cp initrd/etc/c19b.lua                            $(INITRD_STAGE)/etc/c19b.lua
	cp initrd/etc/c20.lua                             $(INITRD_STAGE)/etc/c20.lua
	cp initrd/etc/c21.lua                             $(INITRD_STAGE)/etc/c21.lua
	cp programs/devlist/devlist.elf                   $(INITRD_STAGE)/bin/devlist
	cp programs/drvtest/drvtest.elf                   $(INITRD_STAGE)/bin/drvtest
	cp programs/usbtest/usbtest.elf                   $(INITRD_STAGE)/bin/usbtest
	cp programs/audiotest/audiotest.elf               $(INITRD_STAGE)/bin/audiotest
	cp programs/audioplay/audioplay.elf               $(INITRD_STAGE)/bin/audioplay
	cp programs/batterytest/batterytest.elf           $(INITRD_STAGE)/bin/batterytest
	cp programs/displayinfo/displayinfo.elf           $(INITRD_STAGE)/bin/displayinfo
	cp programs/drawtest/drawtest.elf                 $(INITRD_STAGE)/bin/drawtest
	cp programs/rendertest/rendertest.elf             $(INITRD_STAGE)/bin/rendertest
	cp programs/fonttest/fonttest.elf                 $(INITRD_STAGE)/bin/fonttest
	cp programs/logview/logview.elf                   $(INITRD_STAGE)/bin/logview
	cp programs/crashinfo/crashinfo.elf               $(INITRD_STAGE)/bin/crashinfo
	cp programs/wdogtest/wdogtest.elf                 $(INITRD_STAGE)/bin/wdogtest
	cp programs/safesh/safesh.elf                     $(INITRD_STAGE)/bin/safesh
	cp programs/fscheck/fscheck.elf                   $(INITRD_STAGE)/bin/fscheck
	cp programs/services/services.elf                 $(INITRD_STAGE)/bin/services
	cp programs/svc_crasher/svc_crasher.elf           $(INITRD_STAGE)/bin/svc_crasher
	cp programs/stabilitytest/stabilitytest.elf       $(INITRD_STAGE)/bin/stabilitytest
	cp programs/hwinfo/hwinfo.elf                     $(INITRD_STAGE)/bin/hwinfo
	cp programs/drvmatch/drvmatch.elf                 $(INITRD_STAGE)/bin/drvmatch
	cp programs/hwcompat/hwcompat.elf                 $(INITRD_STAGE)/bin/hwcompat
	cp programs/hwreport/hwreport.elf                 $(INITRD_STAGE)/bin/hwreport
	cp programs/compattest/compattest.elf             $(INITRD_STAGE)/bin/compattest
	cp programs/sigtest/sigtest.elf                   $(INITRD_STAGE)/bin/sigtest
	cp programs/mctest/mctest.elf                     $(INITRD_STAGE)/bin/mctest
	cp programs/reclaimtest/reclaimtest.elf           $(INITRD_STAGE)/bin/reclaimtest
	cp programs/strace/strace.elf                     $(INITRD_STAGE)/bin/strace
	cp programs/debugger/debugger.elf                 $(INITRD_STAGE)/bin/debugger
	cp programs/pkgmgr/pkgmgr.elf                    $(INITRD_STAGE)/bin/pkgmgr
	cp programs/tobycc/tobycc.elf                     $(INITRD_STAGE)/bin/tobycc
	cp programs/tobybuild/tobybuild.elf             $(INITRD_STAGE)/bin/tobybuild
	cp programs/selfhosttest/selfhosttest.elf       $(INITRD_STAGE)/bin/selfhosttest
	cp programs/user_sh/user_sh.elf                 $(INITRD_STAGE)/bin/sh
# /bin/tsh -- the tobyOS shell language hosted in userspace: src/shell.c
# compiled a second time against programs/tsh/host.c. It replaced
# programs/sh/sh.elf here, which had a parser but almost no expansion layer
# and scored 0/14 against real bash. /bin/sh (programs/user_sh) is untouched.
	cp programs/tsh/tsh.elf                         $(INITRD_STAGE)/bin/tsh
	cp programs/id/id.elf                           $(INITRD_STAGE)/bin/id
	cp programs/newgrp/newgrp.elf                   $(INITRD_STAGE)/bin/newgrp
	cp initrd/etc/passwd                            $(INITRD_STAGE)/etc/passwd
	cp initrd/etc/group                             $(INITRD_STAGE)/etc/group
	cp programs/shparity/shparity.elf               $(INITRD_STAGE)/bin/shparity
	mkdir -p $(INITRD_STAGE)/etc/shparity
	cp initrd/etc/shparity/*.sh                     $(INITRD_STAGE)/etc/shparity/
	cp programs/ttyparity/ttyparity.elf             $(INITRD_STAGE)/bin/ttyparity
	mkdir -p $(INITRD_STAGE)/etc/ttyparity
	cp initrd/etc/ttyparity/*.txt                   $(INITRD_STAGE)/etc/ttyparity/
	cp programs/oilspec/oilspec.elf                 $(INITRD_STAGE)/bin/oilspec
	mkdir -p $(INITRD_STAGE)/etc/oilspec/bin
# 2,776 case files: `cp a/*.sh b/` blows past the command-line limit here, so
# copy the directory rather than the glob.
	cp -r initrd/etc/oilspec/.                      $(INITRD_STAGE)/etc/oilspec/
# One binary, four names. tobyOS has no hard links, so these are copies -- and
# spechelp dispatches on argv[0], so the name IS the interface.
	cp programs/spechelp/spechelp.elf               $(INITRD_STAGE)/etc/oilspec/bin/argv.py
	cp programs/spechelp/spechelp.elf               $(INITRD_STAGE)/etc/oilspec/bin/printenv.py
	cp programs/spechelp/spechelp.elf               $(INITRD_STAGE)/etc/oilspec/bin/stdout_stderr.py
	cp programs/spechelp/spechelp.elf               $(INITRD_STAGE)/etc/oilspec/bin/read_from_fd.py
	cp programs/user_pkg/user_pkg.elf               $(INITRD_STAGE)/bin/pkg
	cp programs/user_svcctl/user_svcctl.elf         $(INITRD_STAGE)/bin/svcctl
	cp programs/user_ps/user_ps.elf                 $(INITRD_STAGE)/bin/ps
	cp programs/user_top/user_top.elf               $(INITRD_STAGE)/bin/top
	cp programs/user_cp/user_cp.elf                 $(INITRD_STAGE)/bin/cp
	cp programs/user_mv/user_mv.elf                 $(INITRD_STAGE)/bin/mv
	cp programs/user_rm/user_rm.elf                 $(INITRD_STAGE)/bin/rm
	cp programs/user_find/user_find.elf             $(INITRD_STAGE)/bin/find
	cp programs/user_sort/user_sort.elf             $(INITRD_STAGE)/bin/sort
	cp programs/user_tee/user_tee.elf               $(INITRD_STAGE)/bin/tee
	cp programs/user_env/user_env.elf               $(INITRD_STAGE)/bin/env
	cp programs/user_chmod/user_chmod.elf           $(INITRD_STAGE)/bin/chmod
	cp programs/user_df/user_df.elf                 $(INITRD_STAGE)/bin/df
	cp programs/user_mkdir/user_mkdir.elf           $(INITRD_STAGE)/bin/mkdir
	cp programs/user_touch/user_touch.elf           $(INITRD_STAGE)/bin/touch
	cp programs/user_kill/user_kill.elf             $(INITRD_STAGE)/bin/kill
	cp programs/user_tail/user_tail.elf             $(INITRD_STAGE)/bin/tail
	cp programs/user_uniq/user_uniq.elf             $(INITRD_STAGE)/bin/uniq
	cp programs/user_date/user_date.elf             $(INITRD_STAGE)/bin/date
	cp programs/user_wget/user_wget.elf             $(INITRD_STAGE)/bin/wget
	cp programs/user_nc/user_nc.elf                 $(INITRD_STAGE)/bin/nc
	cp programs/user_ifconfig/user_ifconfig.elf     $(INITRD_STAGE)/bin/ifconfig
	cp programs/user_nslookup/user_nslookup.elf     $(INITRD_STAGE)/bin/nslookup
	cp programs/user_gui_calc/user_gui_calc.elf     $(INITRD_STAGE)/bin/gui_calc
	cp programs/devmgr/devmgr.elf                   $(INITRD_STAGE)/bin/devmgr
	cp programs/indexer/indexer.elf                 $(INITRD_STAGE)/bin/indexer
	cp programs/updater/updater.elf                 $(INITRD_STAGE)/bin/updater
	cp programs/bluescreen/bluescreen.elf           $(INITRD_STAGE)/bin/bluescreen
	cp programs/lockscreen/lockscreen.elf           $(INITRD_STAGE)/bin/lockscreen
	cp programs/mediaplayer/mediaplayer.elf         $(INITRD_STAGE)/bin/mediaplayer
	cp programs/chromewin/chromewin.elf             $(INITRD_STAGE)/bin/chromewin
	# Headed chrome may still fork xdg-settings despite --no-default-browser-check.
	cp programs/linux-xdg-settings/linux-xdg-settings.elf $(INITRD_STAGE)/bin/xdg-settings
	cp programs/diskmgr/diskmgr.elf                 $(INITRD_STAGE)/bin/diskmgr
	cp programs/eventview/eventview.elf             $(INITRD_STAGE)/bin/eventview
	cp programs/lua/lua.elf                         $(INITRD_STAGE)/bin/lua
	cp programs/make/make.elf                       $(INITRD_STAGE)/bin/make
	cp programs/less/less.elf                       $(INITRD_STAGE)/bin/less
	cp programs/curl/curl.elf                       $(INITRD_STAGE)/bin/curl
	cp programs/tcc/tcc.elf                         $(INITRD_STAGE)/bin/tcc
	cp programs/as/as.elf                           $(INITRD_STAGE)/bin/as
	cp programs/ld/ld.elf                           $(INITRD_STAGE)/bin/ld
	cp programs/wifipicker/wifipicker.elf           $(INITRD_STAGE)/bin/wifipicker
	cp programs/volctrl/volctrl.elf                 $(INITRD_STAGE)/bin/volctrl
	cp programs/appinstall/appinstall.elf           $(INITRD_STAGE)/bin/appinstall
	cp initrd/m36/m36_part2.c                         $(INITRD_STAGE)/m36/m36_part2.c
	cp initrd/m36/m36_sample.c                        $(INITRD_STAGE)/m36/m36_sample.c
	cp initrd/m36/Tobyfile                            $(INITRD_STAGE)/m36/Tobyfile
	cp $(PORTS_SBASE_DIR)/echo.elf                    $(INITRD_STAGE)/bin/p_echo
	cp $(PORTS_SBASE_DIR)/cat.elf                     $(INITRD_STAGE)/bin/p_cat
	cp $(PORTS_SBASE_DIR)/wc.elf                      $(INITRD_STAGE)/bin/p_wc
	cp $(PORTS_SBASE_DIR)/head.elf                    $(INITRD_STAGE)/bin/p_head
	cp $(PORTS_SBASE_DIR)/ls.elf                      $(INITRD_STAGE)/bin/p_ls
	cp $(PORTS_SBASE_DIR)/grep.elf                    $(INITRD_STAGE)/bin/p_grep
# ...and under the names scripts actually use. sbase's grep/wc/head had a
# p_ prefix and no plain entry, so `grep` was simply not a command that
# existed. echo/cat/ls keep the prefix because other programs own those.
	cp $(PORTS_SBASE_DIR)/grep.elf                    $(INITRD_STAGE)/bin/grep
	cp $(PORTS_SBASE_DIR)/wc.elf                      $(INITRD_STAGE)/bin/wc
	cp $(PORTS_SBASE_DIR)/head.elf                    $(INITRD_STAGE)/bin/head
	cp $(PORTS_SBASE_DIR)/grep.elf                    $(INITRD_STAGE)/bin/egrep
	cp $(PORTS_SBASE_DIR)/grep.elf                    $(INITRD_STAGE)/bin/fgrep
	cp $(PORTS_SBASE_DIR)/basename.elf                  $(INITRD_STAGE)/bin/basename
	cp $(PORTS_SBASE_DIR)/dirname.elf                   $(INITRD_STAGE)/bin/dirname
	cp $(PORTS_SBASE_DIR)/sleep.elf                     $(INITRD_STAGE)/bin/sleep
	cp $(PORTS_SBASE_DIR)/tee.elf                       $(INITRD_STAGE)/bin/tee
	cp $(PORTS_SBASE_DIR)/expr.elf                      $(INITRD_STAGE)/bin/expr
	cp $(PORTS_SBASE_DIR)/tr.elf                        $(INITRD_STAGE)/bin/tr
	cp $(PORTS_SBASE_DIR)/cut.elf                       $(INITRD_STAGE)/bin/cut
	cp $(PORTS_SBASE_DIR)/uniq.elf                      $(INITRD_STAGE)/bin/uniq
	cp programs/awk/awk.elf                           $(INITRD_STAGE)/bin/awk
	cp $(PORTS_SBASE_DIR)/sort.elf                      $(INITRD_STAGE)/bin/sort
	cp $(LDTOBY_SO)                                   $(INITRD_STAGE)/lib/ld-toby.so
	cp $(LIBTOBY_SO)                                  $(INITRD_STAGE)/lib/libtoby.so
	cp $(HELLOAPP_TPKG)                               $(INITRD_STAGE)/repo/helloapp.tpkg
	cp $(HELLOAPP_TPKG_V2)                            $(INITRD_STAGE)/repo/helloapp_v2.tpkg
	# M34A fixtures: deliberately-broken cousins of helloapp.tpkg used
	# by the security self-test (and by manual `pkg install` from the
	# kernel shell) to prove the integrity verifier rejects bad inputs.
	cp $(HELLOAPP_TPKG_CORRUPT)                       $(INITRD_STAGE)/repo/helloapp_corrupt.tpkg
	cp $(HELLOAPP_TPKG_BADHASH)                       $(INITRD_STAGE)/repo/helloapp_badhash.tpkg
	cp $(HELLOAPP_TPKG_NOHASH)                        $(INITRD_STAGE)/repo/helloapp_nohash.tpkg
	cp $(HELLOAPP_TPKG_BADFHASH)                      $(INITRD_STAGE)/repo/helloapp_badfhash.tpkg
	cp $(HELLOAPP_TPKG_V2_CORRUPT)                    $(INITRD_STAGE)/repo/helloapp_v2_corrupt.tpkg
	# M34C signing fixtures: trust store consulted at boot, plus three
	# .tpkgs covering trusted-good / unknown-key / tampered-signed.
	cp $(TRUST_DB)                                    $(INITRD_STAGE)/system/keys/trust.db
	cp $(HELLOAPP_TPKG_SIGNED)                        $(INITRD_STAGE)/repo/helloapp_signed.tpkg
	cp $(HELLOAPP_TPKG_SIG_UNKNOWN)                   $(INITRD_STAGE)/repo/helloapp_signed_unknown.tpkg
	cp $(HELLOAPP_TPKG_SIG_TAMPER)                    $(INITRD_STAGE)/repo/helloapp_signed_tampered.tpkg
	# M34D capability fixture: declares "FILE_READ,GUI" so the
	# selftest can read it back from the install record and verify
	# cap_apply_declared narrows correctly.
	cp $(HELLOAPP_TPKG_CAPS_RO)                       $(INITRD_STAGE)/repo/helloapp_caps_ro.tpkg
	# M32 SDK samples ship in /repo/ so a fresh boot can immediately
	# `pkg install /repo/notes_app.tpkg` and see it land on the desktop.
	cp $(SDK_SAMPLE_TPKGS)                            $(INITRD_STAGE)/repo/
	@EXTRA_FILES=""; \
	if [ -n "$(CRASHTEST_FLAG)" ]; then                                   \
	    echo "1" > $(INITRD_STAGE)/etc/crashtest_now;                     \
	    echo "[initrd] M28B: CRASHTEST_FLAG set -- "                      \
	         "/etc/crashtest_now baked into initrd";                      \
	    EXTRA_FILES="$$EXTRA_FILES etc/crashtest_now";                    \
	fi;                                                                   \
	if [ -n "$(WDOGTEST_FLAG)" ]; then                                    \
	    echo "1" > $(INITRD_STAGE)/etc/wdogtest_now;                      \
	    echo "[initrd] M28C: WDOGTEST_FLAG set -- "                       \
	         "/etc/wdogtest_now baked into initrd";                       \
	    EXTRA_FILES="$$EXTRA_FILES etc/wdogtest_now";                     \
	fi;                                                                   \
	if [ -n "$(SAFEMODE_FLAG)" ]; then                                    \
	    echo "1" > $(INITRD_STAGE)/etc/safemode_now;                      \
	    echo "[initrd] M28D: SAFEMODE_FLAG set -- "                       \
	         "/etc/safemode_now baked into initrd";                       \
	    EXTRA_FILES="$$EXTRA_FILES etc/safemode_now";                     \
	fi;                                                                   \
	if [ -n "$(FSCHECK_FLAG)" ]; then                                     \
	    echo "1" > $(INITRD_STAGE)/etc/fscheck_now;                       \
	    echo "[initrd] M28E: FSCHECK_FLAG set -- "                        \
	         "/etc/fscheck_now baked into initrd";                        \
	    EXTRA_FILES="$$EXTRA_FILES etc/fscheck_now";                      \
	fi;                                                                   \
	if [ -n "$(SVCTEST_FLAG)" ]; then                                     \
	    echo "1" > $(INITRD_STAGE)/etc/svctest_now;                       \
	    echo "[initrd] M28F: SVCTEST_FLAG set -- "                        \
	         "/etc/svctest_now baked into initrd";                        \
	    EXTRA_FILES="$$EXTRA_FILES etc/svctest_now";                      \
	fi;                                                                   \
	if [ -n "$(STABTEST_FLAG)" ]; then                                    \
	    echo "1" > $(INITRD_STAGE)/etc/stabtest_now;                      \
	    echo "[initrd] M28G: STABTEST_FLAG set -- "                       \
	         "/etc/stabtest_now baked into initrd";                       \
	    EXTRA_FILES="$$EXTRA_FILES etc/stabtest_now";                     \
	fi;                                                                   \
	if [ -n "$(DRVTEST_FLAG)" ]; then                                     \
	    echo "1" > $(INITRD_STAGE)/etc/drvtest_now;                       \
	    echo "[initrd] M29B: DRVTEST_FLAG set -- "                        \
	         "/etc/drvtest_now baked into initrd";                        \
	    EXTRA_FILES="$$EXTRA_FILES etc/drvtest_now";                      \
	fi;                                                                   \
	if [ -f programs/linux-cred/linux-cred.elf ]; then                    \
	    cp programs/linux-cred/linux-cred.elf                             \
	         $(INITRD_STAGE)/bin/linux-cred;                              \
	    echo "[initrd] staged linux-cred (slice 2 POSIX credentials proof)"; \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-cred";                       \
	fi;                                                                   \
	if [ -f programs/linux-suid/linux-suid.elf ]; then                    \
	    cp programs/linux-suid/linux-suid.elf                             \
	         $(INITRD_STAGE)/bin/linux-suid;                              \
	    echo "[initrd] staged linux-suid (slice 2 setuid-on-exec proof)"; \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-suid";                       \
	fi;                                                                   \
	if [ -f programs/linux-timers/linux-timers.elf ]; then                \
	    cp programs/linux-timers/linux-timers.elf                         \
	         $(INITRD_STAGE)/bin/linux-timers;                            \
	    echo "[initrd] staged linux-timers (slice 3 timerfd/signalfd proof)";\
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-timers";                     \
	fi;                                                                   \
	if [ -f programs/linux-mount/linux-mount.elf ]; then                  \
	    cp programs/linux-mount/linux-mount.elf                           \
	         $(INITRD_STAGE)/bin/linux-mount;                             \
	    echo "[initrd] staged linux-mount (slice 5 mount/chroot proof)";  \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-mount";                      \
	fi;                                                                   \
	if [ -f programs/linux-ns/linux-ns.elf ]; then                        \
	    cp programs/linux-ns/linux-ns.elf                                 \
	         $(INITRD_STAGE)/bin/linux-ns;                                \
	    echo "[initrd] staged linux-ns (slice 8 UTS+IPC namespace proof)";\
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-ns";                         \
	fi;                                                                   \
	if [ -f programs/linux-cgroup/linux-cgroup.elf ]; then                 \
	    cp programs/linux-cgroup/linux-cgroup.elf                          \
	         $(INITRD_STAGE)/bin/linux-cgroup;                             \
	    echo "[initrd] staged linux-cgroup (slice 15 cgroup v2 proof)";    \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-cgroup";                      \
	fi;                                                                   \
	if [ -f programs/linux-clonens/linux-clonens.elf ]; then               \
	    cp programs/linux-clonens/linux-clonens.elf                        \
	         $(INITRD_STAGE)/bin/linux-clonens;                            \
	    echo "[initrd] staged linux-clonens (clone+CLONE_NEW* proof)";     \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-clonens";                     \
	fi;                                                                   \
	if [ -f programs/linux-clonestk/linux-clonestk.elf ]; then             \
	    cp programs/linux-clonestk/linux-clonestk.elf                      \
	         $(INITRD_STAGE)/bin/linux-clonestk;                           \
	    echo "[initrd] staged linux-clonestk (clone child-stack proof)";   \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-clonestk";                    \
	fi;                                                                   \
	if [ -f programs/linux-ocirun/linux-ocirun.elf ]; then                 \
	    cp programs/linux-ocirun/linux-ocirun.elf                          \
	         $(INITRD_STAGE)/bin/linux-ocirun;                             \
	    mkdir -p $(INITRD_STAGE)/oci-bundle;                               \
	    cp programs/linux-ocirun/config.json                               \
	         $(INITRD_STAGE)/oci-bundle/config.json;                       \
	    echo "[initrd] staged linux-ocirun + /oci-bundle (slice 16 OCI)";  \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-ocirun oci-bundle/config.json"; \
	fi;                                                                   \
	if [ -f programs/linux-ctrprobe/linux-ctrprobe.elf ]; then             \
	    cp programs/linux-ctrprobe/linux-ctrprobe.elf                      \
	         $(INITRD_STAGE)/bin/linux-ctrprobe;                           \
	    echo "[initrd] staged linux-ctrprobe (in-container probe)";        \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-ctrprobe";                    \
	fi;                                                                   \
	if [ -f programs/linux-gapfill/linux-gapfill.elf ]; then               \
	    cp programs/linux-gapfill/linux-gapfill.elf                        \
	         $(INITRD_STAGE)/bin/linux-gapfill;                            \
	    echo "[initrd] staged linux-gapfill (fdinfo + new syscalls)";      \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-gapfill";                     \
	fi;                                                                   \
	if [ -f programs/linux-cloexec/linux-cloexec.elf ]; then               \
	    cp programs/linux-cloexec/linux-cloexec.elf                        \
	         $(INITRD_STAGE)/bin/linux-cloexec;                            \
	    echo "[initrd] staged linux-cloexec (close-on-exec + close_range)"; \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-cloexec";                     \
	fi;                                                                   \
	if [ -f programs/linux-flock/linux-flock.elf ]; then                   \
	    cp programs/linux-flock/linux-flock.elf                            \
	         $(INITRD_STAGE)/bin/linux-flock;                              \
	    echo "[initrd] staged linux-flock (record locks + flock)";         \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-flock";                       \
	fi;                                                                   \
	if [ -f programs/linux-sock/linux-sock.elf ]; then                     \
	    cp programs/linux-sock/linux-sock.elf                              \
	         $(INITRD_STAGE)/bin/linux-sock;                               \
	    echo "[initrd] staged linux-sock (socket semantics, peer-less)";   \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-sock";                        \
	fi;                                                                   \
	if [ -f programs/linux-sockserver/linux-sockserver.elf ]; then         \
	    cp programs/linux-sockserver/linux-sockserver.elf                  \
	         $(INITRD_STAGE)/bin/linux-sockserver;                         \
	    echo "[initrd] staged linux-sockserver (host-peer gate half)";     \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-sockserver";                  \
	fi;                                                                   \
	if [ -f programs/linux-nptl/linux-nptl.elf ]; then                     \
	    cp programs/linux-nptl/linux-nptl.elf                              \
	         $(INITRD_STAGE)/bin/linux-nptl;                               \
	    echo "[initrd] staged linux-nptl (thread-group semantics)";        \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-nptl";                        \
	fi;                                                                   \
	if [ -f programs/linux-watch/linux-watch.elf ]; then                   \
	    cp programs/linux-watch/linux-watch.elf                            \
	         $(INITRD_STAGE)/bin/linux-watch;                              \
	    echo "[initrd] staged linux-watch (inotify + epoll honesty)";      \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-watch";                       \
	fi;                                                                   \
	if [ -f programs/linux-misc/linux-misc.elf ]; then                     \
	    cp programs/linux-misc/linux-misc.elf                              \
	         $(INITRD_STAGE)/bin/linux-misc;                               \
	    echo "[initrd] staged linux-misc (syscall-tail batch)";            \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-misc";                        \
	fi;                                                                   \
	if [ -f programs/linux-procdev/linux-procdev.elf ]; then               \
	    cp programs/linux-procdev/linux-procdev.elf                        \
	         $(INITRD_STAGE)/bin/linux-procdev;                            \
	    echo "[initrd] staged linux-procdev (procfs+dev fidelity)";        \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-procdev";                     \
	fi;                                                                   \
	if [ -f programs/linux-loop/linux-loop.elf ]; then                     \
	    cp programs/linux-loop/linux-loop.elf                              \
	         $(INITRD_STAGE)/bin/linux-loop;                               \
	    echo "[initrd] staged linux-loop (loopback datapath)";             \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-loop";                        \
	fi;                                                                   \
	if [ -f programs/linux-proc2/linux-proc2.elf ]; then                   \
	    cp programs/linux-proc2/linux-proc2.elf                            \
	         $(INITRD_STAGE)/bin/linux-proc2;                              \
	    echo "[initrd] staged linux-proc2 (procfs deep fidelity)";         \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-proc2";                       \
	fi;                                                                   \
	if [ -f programs/linux-io/linux-io.elf ]; then                         \
	    cp programs/linux-io/linux-io.elf                                  \
	         $(INITRD_STAGE)/bin/linux-io;                                 \
	    echo "[initrd] staged linux-io (POSIX timers + copy family)";      \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-io";                          \
	fi;                                                                   \
	if [ -f programs/linux-xattr/linux-xattr.elf ]; then                   \
	    cp programs/linux-xattr/linux-xattr.elf                            \
	         $(INITRD_STAGE)/bin/linux-xattr;                              \
	    echo "[initrd] staged linux-xattr (xattr + fd identity + fexecve)"; \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-xattr";                       \
	fi;                                                                   \
	if [ -f programs/linux-thread2/linux-thread2.elf ]; then               \
	    cp programs/linux-thread2/linux-thread2.elf                        \
	         $(INITRD_STAGE)/bin/linux-thread2;                            \
	    echo "[initrd] staged linux-thread2 (de_thread + pshared + robust)"; \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-thread2";                     \
	fi;                                                                   \
	if [ -f programs/linux-tmpfs/linux-tmpfs.elf ]; then                   \
	    cp programs/linux-tmpfs/linux-tmpfs.elf                            \
	         $(INITRD_STAGE)/bin/linux-tmpfs;                              \
	    echo "[initrd] staged linux-tmpfs (in-memory filesystem proof)";   \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-tmpfs";                       \
	fi;                                                                   \
	if [ -f programs/linux-ptrace/linux-ptrace.elf ]; then                 \
	    cp programs/linux-ptrace/linux-ptrace.elf                          \
	         $(INITRD_STAGE)/bin/linux-ptrace;                             \
	    echo "[initrd] staged linux-ptrace (mini-strace proof)";           \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-ptrace";                      \
	fi;                                                                   \
	if [ -f programs/linux-cgroup2/linux-cgroup2.elf ]; then               \
	    cp programs/linux-cgroup2/linux-cgroup2.elf                        \
	         $(INITRD_STAGE)/bin/linux-cgroup2;                            \
	    echo "[initrd] staged linux-cgroup2 (slice 16 memory+io proof)";   \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-cgroup2";                     \
	fi;                                                                   \
	if [ -f programs/linux-seccomp/linux-seccomp.elf ]; then               \
	    cp programs/linux-seccomp/linux-seccomp.elf                        \
	         $(INITRD_STAGE)/bin/linux-seccomp;                            \
	    echo "[initrd] staged linux-seccomp (slice 13 seccomp-bpf proof)"; \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-seccomp";                     \
	fi;                                                                   \
	if [ -f programs/linux-netns/linux-netns.elf ]; then                   \
	    cp programs/linux-netns/linux-netns.elf                            \
	         $(INITRD_STAGE)/bin/linux-netns;                              \
	    echo "[initrd] staged linux-netns (slice 12 net ns proof)";        \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-netns";                       \
	fi;                                                                    \
	if [ -f programs/linux-netlink/linux-netlink.elf ]; then               \
	    cp programs/linux-netlink/linux-netlink.elf                        \
	         $(INITRD_STAGE)/bin/linux-netlink;                            \
	    echo "[initrd] staged linux-netlink (cut 4 control-plane proof)";  \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-netlink";                     \
	fi;                                                                    \
	if [ -f programs/linux-userns/linux-userns.elf ]; then                 \
	    cp programs/linux-userns/linux-userns.elf                          \
	         $(INITRD_STAGE)/bin/linux-userns;                             \
	    echo "[initrd] staged linux-userns (slice 11 user ns proof)";      \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-userns";                      \
	fi;                                                                   \
	if [ -f programs/linux-mntns/linux-mntns.elf ]; then                  \
	    cp programs/linux-mntns/linux-mntns.elf                           \
	         $(INITRD_STAGE)/bin/linux-mntns;                             \
	    echo "[initrd] staged linux-mntns (slice 9 mount namespace proof)"; \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-mntns";                      \
	fi;                                                                   \
	if [ -f programs/linux-pidns/linux-pidns.elf ]; then                  \
	    cp programs/linux-pidns/linux-pidns.elf                           \
	         $(INITRD_STAGE)/bin/linux-pidns;                             \
	    echo "[initrd] staged linux-pidns (slice 10 PID namespace proof)"; \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-pidns";                      \
	fi;                                                                   \
	if [ -f programs/linux-alpinerun/linux-alpinerun.elf ]; then          \
	    cp programs/linux-alpinerun/linux-alpinerun.elf                   \
	         $(INITRD_STAGE)/bin/linux-alpinerun;                         \
	    echo "[initrd] staged linux-alpinerun (slice 7 non-root proof)";  \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-alpinerun";                  \
	fi;                                                                   \
	if [ -f programs/alpine/alpine-minirootfs-3.19.0-x86_64.tar.gz ]; then\
	    cp programs/alpine/alpine-minirootfs-3.19.0-x86_64.tar.gz         \
	         $(INITRD_STAGE)/alpine.tar.gz;                               \
	    echo "[initrd] staged alpine minirootfs (slice 7 distro milestone)";\
	    EXTRA_FILES="$$EXTRA_FILES alpine.tar.gz";                        \
	fi;                                                                   \
	if [ -f programs/linux-glibc-dyn/linux-glibc-dyn.elf ] &&            \
	   [ -f programs/linux-glibc-dyn/lib/libc.so.6 ] &&                  \
	   [ -f programs/linux-glibc-dyn/lib/ld-linux-x86-64.so.2 ]; then    \
	    mkdir -p $(INITRD_STAGE)/lib;                                     \
	    cp programs/linux-glibc-dyn/linux-glibc-dyn.elf                  \
	         $(INITRD_STAGE)/bin/linux-glibc-dyn;                        \
	    cp programs/linux-glibc-dyn/lib/ld-linux-x86-64.so.2             \
	         $(INITRD_STAGE)/lib/ld-linux-x86-64.so.2;                   \
	    cp programs/linux-glibc-dyn/lib/libc.so.6                        \
	         $(INITRD_STAGE)/lib/libc.so.6;                              \
	    cp programs/linux-glibc-dyn/lib/libm.so.6                        \
	         $(INITRD_STAGE)/lib/libm.so.6;                              \
	    echo "[initrd] Track B/B26: bundling DYNAMIC glibc proof +"      \
	         "ld-linux-x86-64.so.2 + libc.so.6/libm.so.6 -- OPT-IN";     \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-glibc-dyn lib/ld-linux-x86-64.so.2 lib/libc.so.6 lib/libm.so.6"; \
	fi;                                                                   \
	if [ -f programs/alsa/libasound.so.2 ] &&                            \
	   [ -d programs/alsa/alsa-share ] &&                                \
	   [ -f programs/linux-alsatest/linux-alsatest.elf ]; then           \
	    mkdir -p $(INITRD_STAGE)/lib $(INITRD_STAGE)/usr/share;          \
	    cp programs/alsa/libasound.so.2 $(INITRD_STAGE)/lib/;            \
	    cp -r programs/alsa/alsa-share $(INITRD_STAGE)/usr/share/alsa;   \
	    cp programs/alsa/asound.conf $(INITRD_STAGE)/etc/asound.conf;    \
	    cp programs/linux-alsatest/linux-alsatest.elf                    \
	         $(INITRD_STAGE)/bin/linux-alsatest;                         \
	    echo "[initrd] audio slice 4: bundling UNMODIFIED alsa-lib +"  \
	         "/usr/share/alsa + linux-alsatest -- OPT-IN";             \
	    EXTRA_FILES="$$EXTRA_FILES lib/libasound.so.2 usr/share/alsa etc/asound.conf bin/linux-alsatest"; \
	fi;                                                                   \
	if [ -f programs/linux-egltest/linux-egltest.elf ]; then              \
	    cp programs/linux-egltest/linux-egltest.elf                       \
	         $(INITRD_STAGE)/bin/linux-egltest;                           \
	    echo "[initrd] tier 3 Phase 1c: bundling linux-egltest (drives"   \
	         "staged Mesa against /dev/dri) -- OPT-IN";                   \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-egltest";                    \
	fi;                                                                   \
	if [ -f programs/linux-fbgui/linux-fbgui.elf ] &&                    \
	   [ -f programs/busybox/ld-musl-x86_64.so.1 ]; then                 \
	    mkdir -p $(INITRD_STAGE)/lib;                                     \
	    cp programs/linux-fbgui/linux-fbgui.elf                          \
	         $(INITRD_STAGE)/bin/linux-fbgui;                            \
	    cp programs/busybox/ld-musl-x86_64.so.1                          \
	         $(INITRD_STAGE)/lib/ld-musl-x86_64.so.1;                    \
	    cp programs/busybox/ld-musl-x86_64.so.1                          \
	         $(INITRD_STAGE)/lib/libc.musl-x86_64.so.1;                  \
	    echo "[initrd] Track B graphics: bundling DYNAMIC musl fbdev app" \
	         "(linux-fbgui) + ld-musl loader -- OPT-IN";                 \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-fbgui lib/ld-musl-x86_64.so.1 lib/libc.musl-x86_64.so.1"; \
	fi;                                                                   \
	if [ -f programs/linux-evdev/linux-evdev.elf ] &&                    \
	   [ -f programs/busybox/ld-musl-x86_64.so.1 ]; then                 \
	    mkdir -p $(INITRD_STAGE)/lib;                                     \
	    cp programs/linux-evdev/linux-evdev.elf                          \
	         $(INITRD_STAGE)/bin/linux-evdev;                            \
	    cp programs/busybox/ld-musl-x86_64.so.1                          \
	         $(INITRD_STAGE)/lib/ld-musl-x86_64.so.1;                    \
	    cp programs/busybox/ld-musl-x86_64.so.1                          \
	         $(INITRD_STAGE)/lib/libc.musl-x86_64.so.1;                  \
	    echo "[initrd] Track B input: bundling DYNAMIC musl evdev reader" \
	         "(linux-evdev) + ld-musl loader -- OPT-IN";                 \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-evdev lib/ld-musl-x86_64.so.1 lib/libc.musl-x86_64.so.1"; \
	fi;                                                                   \
	if [ -f programs/linux-paint/linux-paint.elf ] &&                    \
	   [ -f programs/busybox/ld-musl-x86_64.so.1 ]; then                 \
	    mkdir -p $(INITRD_STAGE)/lib;                                     \
	    cp programs/linux-paint/linux-paint.elf                          \
	         $(INITRD_STAGE)/bin/linux-paint;                            \
	    cp programs/busybox/ld-musl-x86_64.so.1                          \
	         $(INITRD_STAGE)/lib/ld-musl-x86_64.so.1;                    \
	    cp programs/busybox/ld-musl-x86_64.so.1                          \
	         $(INITRD_STAGE)/lib/libc.musl-x86_64.so.1;                  \
	    echo "[initrd] Track B input+graphics: bundling DYNAMIC musl"     \
	         "interactive paint app (linux-paint) -- OPT-IN";            \
	    EXTRA_FILES="$$EXTRA_FILES bin/linux-paint lib/ld-musl-x86_64.so.1 lib/libc.musl-x86_64.so.1"; \
	fi;                                                                   \
	if [ -f programs/fbsplash/splash.ppm ]; then                         \
	    mkdir -p $(INITRD_STAGE)/etc;                                     \
	    cp programs/fbsplash/splash.ppm $(INITRD_STAGE)/etc/splash.ppm;   \
	    echo "[initrd] Track B graphics: bundling splash.ppm for the"     \
	         "off-the-shelf busybox fbsplash demo -- OPT-IN";            \
	    EXTRA_FILES="$$EXTRA_FILES etc/splash.ppm";                       \
	fi;                                                                   \
	if [ -f programs/busybox/busybox ]; then                             \
	    cp programs/busybox/busybox $(INITRD_STAGE)/bin/busybox;          \
	    printf '\003' | dd of=$(INITRD_STAGE)/bin/busybox bs=1 seek=7     \
	         count=1 conv=notrunc 2>/dev/null;                           \
	    echo "[initrd] Track B: bundling musl-static busybox (brandelf"   \
	         "EI_OSABI=Linux) -- Linux ABI demo, OPT-IN (not committed)"; \
	    EXTRA_FILES="$$EXTRA_FILES bin/busybox";                          \
	fi;                                                                   \
	if [ -f programs/busybox/busybox-dyn ] &&                            \
	   [ -f programs/busybox/ld-musl-x86_64.so.1 ]; then                 \
	    cp programs/busybox/busybox-dyn $(INITRD_STAGE)/bin/busybox-dyn;  \
	    printf '\003' | dd of=$(INITRD_STAGE)/bin/busybox-dyn bs=1 seek=7 \
	         count=1 conv=notrunc 2>/dev/null;                           \
	    mkdir -p $(INITRD_STAGE)/lib;                                     \
	    cp programs/busybox/ld-musl-x86_64.so.1                          \
	         $(INITRD_STAGE)/lib/ld-musl-x86_64.so.1;                    \
	    cp programs/busybox/ld-musl-x86_64.so.1                          \
	         $(INITRD_STAGE)/lib/libc.musl-x86_64.so.1;                  \
	    echo "[initrd] Track B/B5: bundling DYNAMIC musl busybox +"       \
	         "ld-musl loader -- OPT-IN (not committed)";                 \
	    EXTRA_FILES="$$EXTRA_FILES bin/busybox-dyn lib/ld-musl-x86_64.so.1 lib/libc.musl-x86_64.so.1"; \
	    cp programs/busybox/busybox-dyn $(INITRD_STAGE)/bin/busybox-auto; \
	    printf '\000' | dd of=$(INITRD_STAGE)/bin/busybox-auto bs=1       \
	         seek=7 count=1 conv=notrunc 2>/dev/null;                    \
	    echo "[initrd] Track B/B10: bundling UNBRANDED (EI_OSABI=0) musl" \
	         "busybox -- proves PT_INTERP auto-detect, OPT-IN";          \
	    EXTRA_FILES="$$EXTRA_FILES bin/busybox-auto";                     \
	    if [ -f programs/busybox/file ] &&                               \
	       [ -f programs/busybox/libmagic.so.1 ]; then                   \
	        cp programs/busybox/file $(INITRD_STAGE)/bin/file;            \
	        printf '\003' | dd of=$(INITRD_STAGE)/bin/file bs=1 seek=7    \
	             count=1 conv=notrunc 2>/dev/null;                       \
	        cp programs/busybox/libmagic.so.1                            \
	             $(INITRD_STAGE)/lib/libmagic.so.1;                      \
	        echo "[initrd] Track B/B6: bundling multi-DSO 'file' +"       \
	             "libmagic.so.1 (file-backed mmap of a separate .so)";   \
	        EXTRA_FILES="$$EXTRA_FILES bin/file lib/libmagic.so.1";       \
	    fi;                                                              \
	fi;                                                                   \
	if [ -f programs/realtool/readelf ] &&                               \
	   [ -f programs/realtool/lib64/ld-linux-x86-64.so.2 ] &&            \
	   [ -f programs/realtool/lib64/libc.so.6 ]; then                    \
	    mkdir -p $(INITRD_STAGE)/lib64;                                   \
	    cp programs/realtool/readelf $(INITRD_STAGE)/bin/readelf;         \
	    cp programs/realtool/lib64/ld-linux-x86-64.so.2                  \
	         $(INITRD_STAGE)/lib64/ld-linux-x86-64.so.2;                 \
	    cp programs/realtool/lib64/libc.so.6                             \
	         $(INITRD_STAGE)/lib64/libc.so.6;                            \
	    echo "[initrd] realtool: bundling UNMODIFIED GNU binutils"       \
	         "readelf + glibc runtime under /lib64 -- OPT-IN";           \
	    EXTRA_FILES="$$EXTRA_FILES bin/readelf lib64/ld-linux-x86-64.so.2 lib64/libc.so.6"; \
	fi;                                                                   \
	if [ -f programs/realtools/objdump ] &&                              \
	   [ -f programs/realtools/lib64/ld-linux-x86-64.so.2 ] &&           \
	   [ -f programs/realtools/lib64/libc.so.6 ] &&                      \
	   [ -f programs/realtools/lib64/libdl.so.2 ]; then                  \
	    mkdir -p $(INITRD_STAGE)/lib64;                                   \
	    cp programs/realtools/objdump $(INITRD_STAGE)/bin/objdump;        \
	    cp programs/realtools/nm      $(INITRD_STAGE)/bin/nm;             \
	    cp programs/realtools/size    $(INITRD_STAGE)/bin/size;           \
	    cp programs/realtools/strings $(INITRD_STAGE)/bin/strings;        \
	    cp programs/realtools/cppfilt $(INITRD_STAGE)/bin/cppfilt;        \
	    cp programs/realtools/lib64/ld-linux-x86-64.so.2                 \
	         $(INITRD_STAGE)/lib64/ld-linux-x86-64.so.2;                 \
	    cp programs/realtools/lib64/libc.so.6                            \
	         $(INITRD_STAGE)/lib64/libc.so.6;                            \
	    cp programs/realtools/lib64/libdl.so.2                           \
	         $(INITRD_STAGE)/lib64/libdl.so.2;                           \
	    echo "[initrd] realtools: bundling UNMODIFIED GNU Binutils"      \
	         "battery (objdump/nm/size/strings/c++filt) + glibc -- OPT-IN"; \
	    EXTRA_FILES="$$EXTRA_FILES bin/objdump bin/nm bin/size bin/strings bin/cppfilt lib64/ld-linux-x86-64.so.2 lib64/libc.so.6 lib64/libdl.so.2"; \
	fi;                                                                   \
	if [ -f programs/realbash/bash ]; then                               \
	    cp programs/realbash/bash $(INITRD_STAGE)/bin/bash;               \
	    printf '\003' | dd of=$(INITRD_STAGE)/bin/bash bs=1 seek=7        \
	         count=1 conv=notrunc 2>/dev/null;                           \
	    echo "[initrd] realbash: bundling UNMODIFIED GNU bash 5.2"        \
	         "(static musl, brandelf EI_OSABI=Linux) -- OPT-IN";         \
	    EXTRA_FILES="$$EXTRA_FILES bin/bash";                             \
	fi;                                                                   \
	if [ -f programs/realpython/python3 ] &&                             \
	   [ -f programs/realpython/python310.zip ] &&                       \
	   [ -f programs/busybox/ld-musl-x86_64.so.1 ]; then                 \
	    mkdir -p $(INITRD_STAGE)/lib;                                     \
	    cp programs/realpython/python3 $(INITRD_STAGE)/bin/python3;       \
	    printf '\003' | dd of=$(INITRD_STAGE)/bin/python3 bs=1 seek=7     \
	         count=1 conv=notrunc 2>/dev/null;                           \
	    cp programs/realpython/python310.zip                             \
	         $(INITRD_STAGE)/lib/python310.zip;                          \
	    cp programs/realpython/hello.py $(INITRD_STAGE)/hello.py;         \
	    cp programs/busybox/ld-musl-x86_64.so.1                          \
	         $(INITRD_STAGE)/lib/ld-musl-x86_64.so.1;                    \
	    cp programs/busybox/ld-musl-x86_64.so.1                          \
	         $(INITRD_STAGE)/lib/libc.musl-x86_64.so.1;                  \
	    echo "[initrd] realpython: bundling UNMODIFIED CPython 3.10"     \
	         "(dynamic musl, brandelf EI_OSABI=Linux) + zipimport stdlib -- OPT-IN"; \
	    EXTRA_FILES="$$EXTRA_FILES bin/python3 lib/python310.zip hello.py lib/ld-musl-x86_64.so.1 lib/libc.musl-x86_64.so.1"; \
	fi;                                                                   \
	if [ -f programs/realcc/tcc ] &&                                     \
	   [ -f programs/realcc/libtcc.so ] &&                               \
	   [ -f programs/realcc/tcclib/libtcc1.a ] &&                        \
	   [ -f programs/busybox/ld-musl-x86_64.so.1 ]; then                 \
	    mkdir -p $(INITRD_STAGE)/lib $(INITRD_STAGE)/usr/lib/tcc;         \
	    cp programs/realcc/tcc $(INITRD_STAGE)/bin/realcc;                \
	    printf '\003' | dd of=$(INITRD_STAGE)/bin/realcc bs=1 seek=7      \
	         count=1 conv=notrunc 2>/dev/null;                           \
	    cp programs/realcc/libtcc.so $(INITRD_STAGE)/usr/lib/libtcc.so;   \
	    cp programs/realcc/tcclib/libtcc1.a                              \
	         $(INITRD_STAGE)/usr/lib/tcc/libtcc1.a;                      \
	    cp programs/realcc/hello_cc.c $(INITRD_STAGE)/hello_cc.c;         \
	    cp programs/busybox/ld-musl-x86_64.so.1                          \
	         $(INITRD_STAGE)/lib/ld-musl-x86_64.so.1;                    \
	    cp programs/busybox/ld-musl-x86_64.so.1                          \
	         $(INITRD_STAGE)/lib/libc.musl-x86_64.so.1;                  \
	    echo "[initrd] realcc: bundling UNMODIFIED TinyCC 0.9.27"        \
	         "(dynamic musl + libtcc.so) for in-VM self-hosting -- OPT-IN"; \
	    EXTRA_FILES="$$EXTRA_FILES bin/realcc usr/lib/libtcc.so usr/lib/tcc/libtcc1.a hello_cc.c lib/ld-musl-x86_64.so.1 lib/libc.musl-x86_64.so.1"; \
	fi;                                                                   \
	if [ -f programs/realtui/nano ] &&                                   \
	   [ -f programs/realtui/libncursesw.so.6 ] &&                        \
	   [ -d programs/realtui/terminfo ] &&                                \
	   [ -f programs/busybox/ld-musl-x86_64.so.1 ]; then                 \
	    mkdir -p $(INITRD_STAGE)/lib $(INITRD_STAGE)/usr/lib              \
	             $(INITRD_STAGE)/usr/share;                              \
	    cp programs/realtui/nano $(INITRD_STAGE)/bin/nano;                \
	    printf '\003' | dd of=$(INITRD_STAGE)/bin/nano bs=1 seek=7        \
	         count=1 conv=notrunc 2>/dev/null;                           \
	    cp programs/realtui/libncursesw.so.6                             \
	         $(INITRD_STAGE)/usr/lib/libncursesw.so.6;                   \
	    rm -rf $(INITRD_STAGE)/usr/share/terminfo;                       \
	    cp -r programs/realtui/terminfo                                  \
	         $(INITRD_STAGE)/usr/share/terminfo;                         \
	    cp programs/busybox/ld-musl-x86_64.so.1                          \
	         $(INITRD_STAGE)/lib/ld-musl-x86_64.so.1;                    \
	    cp programs/busybox/ld-musl-x86_64.so.1                          \
	         $(INITRD_STAGE)/lib/libc.musl-x86_64.so.1;                  \
	    echo "[initrd] realtui: bundling UNMODIFIED GNU nano + libncursesw" \
	         "+ terminfo (dynamic musl) for the VT100 console -- OPT-IN"; \
	    EXTRA_FILES="$$EXTRA_FILES bin/nano usr/lib/libncursesw.so.6 usr/share/terminfo lib/ld-musl-x86_64.so.1 lib/libc.musl-x86_64.so.1"; \
	fi;                                                                   \
	if [ -f programs/realcurl/curl ]; then                               \
	    cp programs/realcurl/curl $(INITRD_STAGE)/bin/curl-real;          \
	    printf '\003' | dd of=$(INITRD_STAGE)/bin/curl-real bs=1 seek=7   \
	         count=1 conv=notrunc 2>/dev/null;                           \
	    echo "[initrd] realcurl: bundling UNMODIFIED static curl 8.20.0"  \
	         "(static musl, brandelf EI_OSABI=Linux) -- OPT-IN";         \
	    EXTRA_FILES="$$EXTRA_FILES bin/curl-real";                        \
	fi;                                                                   \
	if [ "$(CHROME_FULL)" = "1" ] && [ -f programs/chromium/chrome-linux64/chrome ]; then \
	    mkdir -p $(INITRD_STAGE)/opt; \
	    cp -r programs/chromium/chrome-linux64 $(INITRD_STAGE)/opt/chrome; \
	    if [ -d programs/chromium/sysroot ]; then \
	        cp -r programs/chromium/sysroot $(INITRD_STAGE)/opt/chrome/sysroot; \
	    fi; \
	    printf '\003' | dd of=$(INITRD_STAGE)/opt/chrome/chrome bs=1 seek=7 count=1 conv=notrunc 2>/dev/null; \
	    cp programs/linux-crashpad/linux-crashpad.elf \
	        $(INITRD_STAGE)/opt/chrome/chrome_crashpad_handler; \
	    printf '\003' | dd of=$(INITRD_STAGE)/opt/chrome/chrome_crashpad_handler bs=1 seek=7 count=1 conv=notrunc 2>/dev/null; \
	    rm -f $(INITRD_STAGE)/opt/chrome/vk_swiftshader_icd.json; \
	    rm -f $(INITRD_STAGE)/opt/chrome/libvk_swiftshader.so; \
	    echo "[initrd] chromium: staging FULL chrome (+sysroot) under /opt/chrome -- CHROME_FULL=1 (Route B: headed/Ozone arc)"; \
	    EXTRA_FILES="$$EXTRA_FILES opt/chrome"; \
	elif [ -f programs/chromium/chrome-headless-shell-linux64/chrome-headless-shell ]; then \
	    mkdir -p $(INITRD_STAGE)/opt; \
	    cp -r programs/chromium/chrome-headless-shell-linux64 $(INITRD_STAGE)/opt/chrome; \
	    if [ -d programs/chromium/sysroot ]; then \
	        cp -r programs/chromium/sysroot $(INITRD_STAGE)/opt/chrome/sysroot; \
	    fi; \
	    printf '\003' | dd of=$(INITRD_STAGE)/opt/chrome/chrome-headless-shell bs=1 seek=7 count=1 conv=notrunc 2>/dev/null; \
	    echo "[initrd] chromium: staging chrome-headless-shell +sysroot under /opt/chrome -- OPT-IN M0"; \
	    EXTRA_FILES="$$EXTRA_FILES opt/chrome"; \
	    for pd in etc/opt/chrome/policies/managed etc/chromium/policies/managed etc/opt/chrome_for_testing/policies/managed; do \
	        mkdir -p $(INITRD_STAGE)/$$pd; \
	        printf '{ "PostQuantumKeyAgreementEnabled": false }\n' > $(INITRD_STAGE)/$$pd/tobyos.json; \
	        EXTRA_FILES="$$EXTRA_FILES $$pd/tobyos.json"; \
	    done; \
	    echo "[initrd] chromium: staged managed policy PostQuantumKeyAgreementEnabled=false (disable X25519MLKEM768 PQ KEX -- tobyOS ML-KEM wall)"; \
	fi; \
	cd $(INITRD_STAGE) && tar --format=ustar --mode=0755 \
	    --owner=0 --group=0 --numeric-owner -cf ../initrd.tar             \
	    bin/hello bin/bad bin/echo bin/cat bin/long_task bin/netecho      \
	    bin/gui_demo bin/x2gui bin/gui_clock bin/gui_widgets bin/gui_about          \
	    bin/cpptest bin/oh264test bin/bsdroute bin/mp4play bin/stltest bin/av1test bin/aactest bin/wasmtest \
	    bin/gui_term bin/gui_files bin/gui_viewer                         \
	    bin/gui_calc bin/gui_taskmgr bin/gui_edit bin/gui_browser         \
	    bin/login bin/gui_settings bin/abi_test                           \
	    bin/volctrl bin/wifipicker bin/appinstall bin/updater bin/lockscreen \
	    bin/devmgr bin/diskmgr bin/eventview bin/mediaplayer bin/chromewin \
	    bin/xdg-settings \
	    bin/c_hello bin/c_args bin/c_filedemo bin/c_alloctest bin/c_envtest \
	    bin/nsetuid \
	    bin/c_dynhello bin/linux-hello bin/linux-sigtest                  \
	    bin/linux-mmaptest bin/linux-thread bin/linux-fb bin/linux-fbwin bin/linux-join bin/linux-poll \
	    bin/linux-netserver bin/linux-siginfo bin/linux-httpget bin/linux-msgsock \
	    bin/linux-static-auto bin/linux-procfs bin/linux-tty bin/linux-pty bin/linux-procfs2 \
	    bin/linux-scmsg bin/linux-futex bin/linux-eventfd bin/linux-mapshare bin/linux-demrace bin/linux-mlkem \
	    bin/linux-qfreeze bin/linux-drmtest bin/linux-sndtest \
	    bin/linux-xstub \
	    bin/basename bin/dirname bin/sleep bin/tee bin/expr \
	    bin/tr bin/cut bin/uniq bin/sort bin/awk \
	    bin/linux-glibc \
	    bin/win-hello.exe \
	    bin/win-crt.exe bin/win-hello3.exe bin/win-cpp.exe              \
	    bin/win-fileio.exe bin/win-thread.exe bin/win-tls18b.exe       \
	    bin/win-shell18d.exe                                           \
	    bin/win-gui.exe                                                \
	    bin/win-gui8.exe bin/win-c9.exe bin/win-gui10.exe              \
	    bin/win-c11file.exe bin/win-gui11.exe                          \
	    bin/win-bmp12.exe bin/win-ctrl12.exe                           \
	    bin/win-ctrl13.exe bin/win-font13.exe                          \
	    bin/win-ctrl14.exe bin/win-dlg14.exe bin/win-font14b.exe       \
	    bin/win-cc25.exe                                               \
	    bin/win-menu15.exe bin/win-reg16.exe bin/win-fdlg16.exe        \
	    bin/win-env16.exe bin/win-tls16.exe bin/win-net17.exe         \
	    bin/win-srv17.exe bin/win-udp17.exe bin/win-sqlite.exe        \
	    bin/win-font18c.exe bin/win-lua.exe bin/win-com19c.exe        \
	    bin/win-math20.exe bin/win-printf21.exe bin/win-scan22.exe    \
	    bin/win-wide23.exe bin/win-reloc24.exe                        \
	    bin/win-xprod.exe bin/win-xcons.exe bin/win-xfilter.exe       \
	    bin/devlist bin/drvtest bin/usbtest bin/audiotest bin/audioplay bin/batterytest \
	    bin/displayinfo bin/drawtest bin/rendertest bin/fonttest          \
	    bin/logview bin/crashinfo bin/wdogtest bin/safesh bin/fscheck     \
	    bin/services bin/svc_crasher bin/stabilitytest                    \
	    bin/hwinfo bin/drvmatch bin/hwcompat bin/hwreport bin/compattest  \
	    bin/sigtest bin/mctest bin/reclaimtest bin/tobycc bin/tobybuild bin/selfhosttest \
    bin/lua bin/make bin/less bin/curl bin/tcc bin/as \
	    bin/p_echo bin/p_cat bin/p_wc bin/p_head bin/p_ls bin/p_grep      \
	    bin/grep bin/wc bin/head bin/egrep bin/fgrep                      \
	    bin/mkdir bin/rm bin/cp bin/mv bin/touch bin/chmod bin/date       \
	    bin/kill bin/id bin/newgrp                                        \
	    bin/env bin/find bin/tail bin/df bin/ps bin/top bin/sh            \
	    bin/nc bin/wget bin/nslookup bin/ifconfig bin/pkg bin/svcctl      \
	    bin/strace bin/debugger bin/indexer bin/pkgmgr bin/ld             \
	    bin/bluescreen                                                    \
	    bin/tsh bin/shparity bin/oilspec bin/ttyparity                    \
	    lib/ld-toby.so lib/libtoby.so                                     \
	    $$EXTRA_FILES                                                     \
	  && tar --format=ustar --mode=0644 \
	    --owner=0 --group=0 --numeric-owner -rf ../initrd.tar             \
	    etc/c19b.lua etc/c20.lua etc/c21.lua                          \
	    etc/Lato-Regular.ttf etc/Lato-Bold.ttf etc/Lato-Italic.ttf   \
	    etc/Lato-BoldItalic.ttf etc/fallback.otf etc/emoji.ttf        \
	    etc/DejaVuSerif.ttf etc/DejaVuSerif-Bold.ttf                  \
	    etc/DejaVuSansMono.ttf etc/DejaVuSansMono-Bold.ttf            \
	    repo/helloapp.tpkg repo/helloapp_v2.tpkg                          \
	    repo/helloapp_corrupt.tpkg repo/helloapp_badhash.tpkg             \
	    repo/helloapp_nohash.tpkg  repo/helloapp_badfhash.tpkg            \
	    repo/helloapp_v2_corrupt.tpkg                                     \
	    repo/helloapp_signed.tpkg repo/helloapp_signed_unknown.tpkg       \
	    repo/helloapp_signed_tampered.tpkg                                \
	    repo/helloapp_caps_ro.tpkg                                        \
	    system/keys/trust.db                                              \
	    repo/hello_cli.tpkg repo/hello_gui.tpkg repo/notes_app.tpkg       \
	    etc/motd etc/anim.html etc/webgl.html etc/input.html etc/input2.html etc/fonttest.html etc/audiotone.html etc/start.html etc/egl_vendor.json etc/drvmatch.conf etc/drvmatch.conf.test etc/lua_selftest.lua \
	    etc/resolv.conf etc/fonts/fonts.conf \
	    etc/passwd etc/group \
	    m36/m36_part2.c m36/m36_sample.c m36/Tobyfile \
	    etc/shparity/*.sh \
	    etc/ttyparity/*.txt \
	    readme.txt
# The 2,776 corpus scripts are DATA (run as `bash FILE`), so 0644 is right.
# Tarred as a TREE, not a glob: 2,776 names is 75 KB of command line. The
# helper binaries are excluded here and added below with the executable bit,
# because staging them 0644 made every case that calls argv.py fail to run
# it -- 327 cases reporting a shell divergence that was a permission bit.
# No './' prefix: gate 0 matches exact tar paths and './etc/...' is not
# the same name as 'etc/...'.
	cd $(INITRD_STAGE) && tar --format=ustar --mode=0644 \
	    --owner=0 --group=0 --numeric-owner \
	    --exclude='etc/oilspec/bin' --exclude='etc/oilspec/bin/*' \
	    -rf ../initrd.tar etc/oilspec
	cd $(INITRD_STAGE) && tar --format=ustar --mode=0755 \
	    --owner=0 --group=0 --numeric-owner -rf ../initrd.tar \
	    etc/oilspec/bin

# ---------- Build ISO ----------
#
# Milestone 20 introduces a two-stage ISO build:
#
#   build/base.iso   : self-contained bootable image -- kernel +
#                      initrd only, no installer module. This is the
#                      image that lands on the installed disk.
#
#   build/install.img: base.iso padded to INSTALL_BOOT_BYTES. The
#                      installer flashes this onto the front of the
#                      target disk.
#
#   tobyOS.iso       : the LIVE ISO. Ships the same kernel + initrd as
#                      base.iso, PLUS an extra Limine module
#                      "install.img" so the running OS can install
#                      itself onto a disk.
#
# There is no circular dependency: install.img is derived from the
# standalone base.iso (which knows nothing about the installer), not
# from the final tobyOS.iso.

# Hybrid ISO build helper: every ISO we ship (base.iso + tobyOS.iso)
# is a single image that boots all four ways the real world cares
# about:
#
#   1. legacy BIOS  -- via Limine's CD-ROM stage (limine-bios-cd.bin)
#                      from a CD/DVD or a USB stick presented to the
#                      firmware as a CD device
#   2. legacy BIOS  -- via Limine's HDD MBR stage1 (installed by
#                      `limine bios-install`) when the firmware
#                      treats the USB stick as a hard drive
#   3. UEFI x86_64  -- via Limine's UEFI loader (limine-uefi-cd.bin)
#                      from El Torito, AND via the EFI System
#                      Partition that xorriso embeds when given
#                      --efi-boot-part. We drop BOOTX64.EFI into
#                      EFI/BOOT/ so removable-media UEFI fallback
#                      finds it
#   4. QEMU         -- both `-cdrom $(ISO)` and `-boot c` against the
#                      same image
#
# The `dd ... seek=446 count=16` after xorriso zeros out *only* the
# first MBR partition entry (the protective-ISO one whose start LBA
# is < 63 and would make `limine bios-install` abort with "A
# partition's start sector is less than 63"). The EFI partition
# entry that xorriso writes lives in MBR slot 2 (offset 462) and is
# preserved, so UEFI firmware that scans the MBR can still see the
# ESP. The GPT copy of the EFI partition (also written by xorriso)
# is untouched and is what real UEFI implementations actually use.
#
# We DO NOT pass --force to limine bios-install here -- the slot-1
# zero is enough to satisfy the safety check, and we want to know
# loudly if a future xorriso change breaks the assumption.

$(BASE_ISO): $(KERNEL) limine_base.conf $(LIMINE_FILES) $(INITRD_TAR)
	rm -rf build/iso_base
	mkdir -p build/iso_base/boot/limine
	mkdir -p build/iso_base/EFI/BOOT
	cp $(KERNEL)                           build/iso_base/boot/
	cp limine_base.conf                    build/iso_base/boot/limine/limine.conf
	cp $(LIMINE_DIR)/limine-bios.sys       build/iso_base/boot/limine/
	cp $(LIMINE_DIR)/limine-bios-cd.bin    build/iso_base/boot/limine/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin    build/iso_base/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI           build/iso_base/EFI/BOOT/
	cp $(INITRD_TAR)                       build/iso_base/boot/initrd.tar
	xorriso -as mkisofs -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot \
		-boot-load-size 4 \
		-boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image \
		--protective-msdos-label \
		build/iso_base -o $(BASE_ISO)
	@echo "[base.iso] zeroing protective-ISO MBR slot before limine bios-install"
	dd if=/dev/zero of=$(BASE_ISO) bs=1 count=16 seek=446 conv=notrunc status=none
	$(LIMINE_DIR)/limine bios-install $(BASE_ISO)

# install.img = base.iso, padded to INSTALL_BOOT_BYTES. base.iso is
# already a fully hybrid (BIOS+UEFI) bootable image with Limine's
# MBR stage1 stamped on it, so all install.img has to do is hold
# that exact byte stream at a known size that the kernel and the
# installer both agree on. The padding size must match
# INSTALLER_BOOT_BYTES in the kernel header so the boot region the
# installer writes lines up exactly with what the kernel then
# expects to find at LBA 0 and (INSTALL_BOOT_BYTES/512) on the
# target disk.
$(INSTALL_IMG): $(BASE_ISO)
	cp $(BASE_ISO) $(INSTALL_IMG)
	truncate -s $(INSTALL_BOOT_BYTES) $(INSTALL_IMG)

$(ISO): $(KERNEL) limine.conf $(LIMINE_FILES) $(INITRD_TAR) $(INSTALL_IMG)
	rm -rf iso
	mkdir -p iso/boot/limine
	mkdir -p iso/EFI/BOOT
	cp $(KERNEL)                           iso/boot/
	cp limine.conf                         iso/boot/limine/
	cp $(LIMINE_DIR)/limine-bios.sys       iso/boot/limine/
	cp $(LIMINE_DIR)/limine-bios-cd.bin    iso/boot/limine/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin    iso/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI           iso/EFI/BOOT/
	cp $(INITRD_TAR)                       iso/boot/initrd.tar
	cp $(INSTALL_IMG)                      iso/boot/install.img
	xorriso -as mkisofs -R -r -J \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot \
		-boot-load-size 4 \
		-boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image \
		--protective-msdos-label \
		iso -o $(ISO)
	@echo "[tobyOS.iso] zeroing protective-ISO MBR slot before limine bios-install"
	dd if=/dev/zero of=$(ISO) bs=1 count=16 seek=446 conv=notrunc status=none
	$(LIMINE_DIR)/limine bios-install $(ISO)

# ---------- Build host mkfs tool + format disk ----------

$(MKFS_BIN): $(MKFS_SRC)
	mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $<

# Only formats if disk.img doesn't already exist -- protects user data
# across normal `make` invocations. Use `make wipe-disk` to force.
#
# Milestone 20: the disk is sized to $(DISK_IMG_BYTES) (16 MiB by
# default). mkfs_tobyfs only stamps tobyfs metadata in the first 4 MiB
# (TFS_TOTAL_BLOCKS * 4 KiB); the trailing bytes are left as zeros
# until the installer either (a) overwrites them with a fresh tobyfs
# partition (installed layout), or (b) nothing at all (live layout).
$(DISK_IMG): $(MKFS_BIN)
	@if [ ! -f $(DISK_IMG) ]; then \
	    echo "[mkfs] formatting fresh $(DISK_IMG) ($(DISK_IMG_BYTES) bytes)..."; \
	    $(MKFS_BIN) $(DISK_IMG); \
	    truncate -s $(DISK_IMG_BYTES) $(DISK_IMG); \
	else \
	    echo "[mkfs] $(DISK_IMG) already exists -- preserving (run 'make wipe-disk' to recreate)"; \
	fi

mkfs: $(MKFS_BIN)
disk: $(DISK_IMG)

wipe-disk: $(MKFS_BIN)
	rm -f $(DISK_IMG)
	$(MKFS_BIN) $(DISK_IMG)
	truncate -s $(DISK_IMG_BYTES) $(DISK_IMG)

# Milestone 20: a FRESH, blank 16 MiB disk that the installer can
# write its full layout to. Not formatted beforehand -- the installer
# itself writes Limine to the first 4 MiB + a fresh tobyfs region
# after that, so pre-formatting would be discarded. Use this when
# you want to prove an install completes from a known-clean target.
install-target: $(MKFS_BIN)
	rm -f install-target.img
	truncate -s $(DISK_IMG_BYTES) install-target.img
	@echo "[install-target] 16 MiB blank target ready at install-target.img"

# ---------- M23A: GPT-formatted demo disk ----------
#
# build/disk_gpt.img is a 32 MiB raw image with a protective MBR + a
# real GPT carrying three partitions:
#
#   slot 1: BIOS Boot            (1 MiB,    LBA 34..2047)
#   slot 2: tobyOS-data (tobyfs) (4 MiB,    LBA 2048..10239)
#   slot 3: MS Basic Data        (2 MiB,    LBA 10240..14335)
#
# The kernel mounts slot 2 at /data via partition_find_by_type
# (GPT_TYPE_TOBYOS_DATA) -- no LBA-0 fallback needed. The other two
# partitions exist purely to demonstrate multi-partition discovery in
# `blkdump -v`.
$(MKDISK_GPT_BIN): $(MKDISK_GPT_SRC)
	mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $<

mkdisk-gpt: $(MKDISK_GPT_BIN)

# Always rebuild the GPT image -- it's cheap (32 MiB, deterministic
# layout) and it ships under build/ so 'make clean' wipes it. We
# regenerate every invocation so the image always matches the
# checked-in tool source.
$(GPT_DISK_IMG): $(MKDISK_GPT_BIN)
	$(MKDISK_GPT_BIN) $(GPT_DISK_IMG)

gpt-disk: $(GPT_DISK_IMG)

# ---------- Build host mkpkg tool + demo package (milestone 16) ----

$(MKPKG_BIN): $(MKPKG_SRC)
	mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $<

mkpkg: $(MKPKG_BIN)

# Suppress MSYS2 / Cygwin runtime path translation for *every* mkpkg
# invocation in this Makefile. Without it, /data/apps/helloapp.txt
# (which mkpkg expects to consume verbatim because the package format
# is full of guest-VFS absolute paths) gets rewritten to e.g.
# C:/msys64/data/... before the binary even sees it. The result is
# that --file dest paths and --bad-fhash key strings stop matching
# each other, and the badfhash fixture turns out IDENTICAL to the
# good package -- a particularly painful silent miscompile of test
# data. Setting MSYS2_ARG_CONV_EXCL=* (or NOGLOB on Cygwin) in the
# environment used by every mkpkg recipe disables the conversion for
# that one process.
#
# Defined as a `MKPKG :=` shim so the rule body reads the same on Linux
# and macOS hosts where the env var is harmless.
MKPKG := MSYS2_ARG_CONV_EXCL='*' $(MKPKG_BIN)

# Small README shipped inside the package so `cat
# /data/apps/helloapp.txt` after install works. Rebuilt on every
# package build so it stays in sync with the mkpkg invocation.
$(HELLOAPP_README):
	mkdir -p build
	printf "%s\n%s\n%s\n" \
	    "tobyOS helloapp -- installed via \`pkg install helloapp\`." \
	    "Launcher entry: 'Hello Pkg' -> /data/apps/helloapp.elf" \
	    "Remove with: pkg remove helloapp" > $@

$(HELLOAPP_TPKG): $(MKPKG_BIN) \
                  programs/user_gui_helloapp/user_gui_helloapp.elf \
                  $(HELLOAPP_README)
	$(MKPKG) -o $@ \
	    --name helloapp \
	    --version 1.0.0 \
	    --desc "Packaged hello-world GUI app (milestone 16 demo)" \
	    --publisher "tobyOS-builtin" \
	    --app "Hello Pkg|/data/apps/helloapp.elf" \
	    --file /data/apps/helloapp.elf:programs/user_gui_helloapp/user_gui_helloapp.elf \
	    --file /data/apps/helloapp.txt:$(HELLOAPP_README)

helloapp-tpkg: $(HELLOAPP_TPKG)

# Milestone-17 v1.1 build of the SAME helloapp package. The ELF is
# identical (we don't have two separate sources to maintain), but the
# DESC + README change so the user can SEE the upgrade has landed
# (`cat /data/apps/helloapp.txt` after upgrade prints the v1.1 text).
$(HELLOAPP_README_V2):
	mkdir -p build
	printf "%s\n%s\n%s\n%s\n" \
	    "tobyOS helloapp v1.1.0 -- upgraded via \`pkg upgrade helloapp\`." \
	    "Launcher entry: 'Hello Pkg' -> /data/apps/helloapp.elf" \
	    "Roll back to v1.0.0 with: pkg rollback helloapp" \
	    "Remove with: pkg remove helloapp" > $@

$(HELLOAPP_TPKG_V2): $(MKPKG_BIN) \
                     programs/user_gui_helloapp/user_gui_helloapp.elf \
                     $(HELLOAPP_README_V2)
	$(MKPKG) -o $@ \
	    --name helloapp \
	    --version 1.1.0 \
	    --desc "Packaged hello-world GUI app (milestone 17 upgrade)" \
	    --publisher "tobyOS-builtin" \
	    --app "Hello Pkg|/data/apps/helloapp.elf" \
	    --file /data/apps/helloapp.elf:programs/user_gui_helloapp/user_gui_helloapp.elf \
	    --file /data/apps/helloapp.txt:$(HELLOAPP_README_V2)

helloapp-tpkg-v2: $(HELLOAPP_TPKG_V2)

# ---------- Milestone 34: package security fixtures ---------------
#
# Each helloapp_*.tpkg below is a deliberately-broken cousin of
# helloapp.tpkg, used by the M34 self-test (and by manual `pkg install`
# from the shell) to prove the verifier rejects the bad ones cleanly.
#
#   helloapp_corrupt.tpkg  body byte 8 has been XORed with 0x01 AFTER
#                          HASH was computed -- declared HASH no longer
#                          matches the body. Exercises HASH verification.
#   helloapp_badhash.tpkg  declared HASH is wrong from the start (body
#                          byte 0 was flipped before computing HASH).
#                          Exercises the HASH-vs-body comparison path
#                          when the body is intact but the metadata lies.
#   helloapp_nohash.tpkg   no HASH/FHASH at all -- legacy pre-M34 layout,
#                          must still install under WARN policy.
#   helloapp_badfhash.tpkg per-file FHASH for /data/apps/helloapp.txt is
#                          wrong; the package-wide HASH is correct, so
#                          this exercises the per-file FHASH check
#                          rejecting before any FILE is laid down.
#
# The fixture variable defs live up near HELLOAPP_TPKG so $(INITRD_TAR)'s
# prereq list can reference them at parse time.

$(HELLOAPP_TPKG_CORRUPT): $(MKPKG_BIN) \
                          programs/user_gui_helloapp/user_gui_helloapp.elf \
                          $(HELLOAPP_README)
	$(MKPKG) -o $@ \
	    --name helloapp \
	    --version 1.0.0 \
	    --desc "Corrupted helloapp fixture (M34A negative test)" \
	    --publisher "tobyOS-builtin" \
	    --app "Hello Pkg|/data/apps/helloapp.elf" \
	    --file /data/apps/helloapp.elf:programs/user_gui_helloapp/user_gui_helloapp.elf \
	    --file /data/apps/helloapp.txt:$(HELLOAPP_README) \
	    --corrupt-body 8

$(HELLOAPP_TPKG_BADHASH): $(MKPKG_BIN) \
                          programs/user_gui_helloapp/user_gui_helloapp.elf \
                          $(HELLOAPP_README)
	$(MKPKG) -o $@ \
	    --name helloapp \
	    --version 1.0.0 \
	    --desc "Wrong-HASH helloapp fixture (M34A negative test)" \
	    --publisher "tobyOS-builtin" \
	    --app "Hello Pkg|/data/apps/helloapp.elf" \
	    --file /data/apps/helloapp.elf:programs/user_gui_helloapp/user_gui_helloapp.elf \
	    --file /data/apps/helloapp.txt:$(HELLOAPP_README) \
	    --bad-hash

$(HELLOAPP_TPKG_NOHASH): $(MKPKG_BIN) \
                         programs/user_gui_helloapp/user_gui_helloapp.elf \
                         $(HELLOAPP_README)
	$(MKPKG) -o $@ \
	    --name helloapp \
	    --version 1.0.0 \
	    --desc "Legacy unhashed helloapp fixture (M34A WARN policy)" \
	    --publisher "tobyOS-builtin" \
	    --app "Hello Pkg|/data/apps/helloapp.elf" \
	    --file /data/apps/helloapp.elf:programs/user_gui_helloapp/user_gui_helloapp.elf \
	    --file /data/apps/helloapp.txt:$(HELLOAPP_README) \
	    --no-hash

$(HELLOAPP_TPKG_BADFHASH): $(MKPKG_BIN) \
                           programs/user_gui_helloapp/user_gui_helloapp.elf \
                           $(HELLOAPP_README)
	$(MKPKG) -o $@ \
	    --name helloapp \
	    --version 1.0.0 \
	    --desc "Wrong-FHASH helloapp fixture (M34A negative test)" \
	    --publisher "tobyOS-builtin" \
	    --app "Hello Pkg|/data/apps/helloapp.elf" \
	    --file /data/apps/helloapp.elf:programs/user_gui_helloapp/user_gui_helloapp.elf \
	    --file /data/apps/helloapp.txt:$(HELLOAPP_README) \
	    --bad-fhash /data/apps/helloapp.txt

# M34B: corrupted-update fixture. We pick VERSION=1.0.0 deliberately
# (matches the live-install starting version) so it isn't auto-selected
# by repo_find_latest -- the M34B self-test forces it in via the new
# pkg_upgrade_path() entrypoint.
$(HELLOAPP_TPKG_V2_CORRUPT): $(MKPKG_BIN) \
                             programs/user_gui_helloapp/user_gui_helloapp.elf \
                             $(HELLOAPP_README_V2)
	$(MKPKG) -o $@ \
	    --name helloapp \
	    --version 1.0.0 \
	    --desc "Corrupted helloapp v2 fixture (M34B negative test)" \
	    --publisher "tobyOS-builtin" \
	    --app "Hello Pkg|/data/apps/helloapp.elf" \
	    --file /data/apps/helloapp.elf:programs/user_gui_helloapp/user_gui_helloapp.elf \
	    --file /data/apps/helloapp.txt:$(HELLOAPP_README_V2) \
	    --corrupt-body 16

# ---------- M34C: signing key + trust store + signed fixtures -----
#
# We commit ZERO key material to the source tree. The 32-byte HMAC key
# is materialised here as a deterministic byte sequence (0x01..0x20) so
# rebuilds are reproducible AND so trust.db's pinned hex literal stays
# in sync with the file mkpkg signs against. If you change the byte
# sequence here, regenerate the hex in $(TRUST_DB) below.
$(TEST_KEY_BIN):
	@mkdir -p $(dir $@)
	printf '\1\2\3\4\5\6\7\10\11\12\13\14\15\16\17\20\21\22\23\24\25\26\27\30\31\32\33\34\35\36\37\40' > $@

# Trust store consumed by sig_trust_store_init() at boot. Lives in the
# initrd at /system/keys/trust.db (read-only, ramfs-backed -- exactly
# the right shape for a security boundary like this on tobyOS today).
# Format: KEY <id> HMAC-SHA256 <hex(key)>. Hex below MUST match the
# bytes printf'd into $(TEST_KEY_BIN) above.
$(TRUST_DB):
	@mkdir -p $(dir $@)
	@printf '# tobyOS trust store -- auto-generated by Makefile.\n' > $@
	@printf '# Edit Makefile target $$(TRUST_DB) to change keys.\n' >> $@
	@printf 'KEY $(TEST_KEY_ID) HMAC-SHA256 0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20\n' >> $@

# Cleanly signed package: HASH + FHASH + SIG all valid; key id is a
# match for the trust store entry above. Positive-path fixture.
$(HELLOAPP_TPKG_SIGNED): $(MKPKG_BIN) $(TEST_KEY_BIN) \
                         programs/user_gui_helloapp/user_gui_helloapp.elf \
                         $(HELLOAPP_README)
	$(MKPKG) -o $@ \
	    --name helloapp \
	    --version 1.0.0 \
	    --desc "Signed helloapp fixture (M34C positive test)" \
	    --publisher "tobyOS-builtin" \
	    --app "Hello Pkg|/data/apps/helloapp.elf" \
	    --file /data/apps/helloapp.elf:programs/user_gui_helloapp/user_gui_helloapp.elf \
	    --file /data/apps/helloapp.txt:$(HELLOAPP_README) \
	    --sign-key-file $(TEST_KEY_BIN) \
	    --sign-key-id $(TEST_KEY_ID)

# Same key BYTES, wrong key ID -- the verifier looks up the id in the
# trust store and bails before even computing the MAC, so we don't need
# a separate "real other key". Tests the unknown-key reject path.
$(HELLOAPP_TPKG_SIG_UNKNOWN): $(MKPKG_BIN) $(TEST_KEY_BIN) \
                              programs/user_gui_helloapp/user_gui_helloapp.elf \
                              $(HELLOAPP_README)
	$(MKPKG) -o $@ \
	    --name helloapp \
	    --version 1.0.0 \
	    --desc "Signed-by-unknown helloapp (M34C negative test)" \
	    --publisher "tobyOS-builtin" \
	    --app "Hello Pkg|/data/apps/helloapp.elf" \
	    --file /data/apps/helloapp.elf:programs/user_gui_helloapp/user_gui_helloapp.elf \
	    --file /data/apps/helloapp.txt:$(HELLOAPP_README) \
	    --sign-key-file $(TEST_KEY_BIN) \
	    --sign-key-id rogue

# Tampered signed package. We use --no-hash so the in-band SHA-256
# can't catch the body flip first; that forces the SIG check to be
# the gate that detects the tamper. (HASH-then-SIG ordering is also
# tested implicitly by the M34A/B body-corruption fixtures.)
$(HELLOAPP_TPKG_SIG_TAMPER): $(MKPKG_BIN) $(TEST_KEY_BIN) \
                             programs/user_gui_helloapp/user_gui_helloapp.elf \
                             $(HELLOAPP_README)
	$(MKPKG) -o $@ \
	    --name helloapp \
	    --version 1.0.0 \
	    --desc "Signed-and-tampered helloapp (M34C negative test)" \
	    --publisher "tobyOS-builtin" \
	    --app "Hello Pkg|/data/apps/helloapp.elf" \
	    --file /data/apps/helloapp.elf:programs/user_gui_helloapp/user_gui_helloapp.elf \
	    --file /data/apps/helloapp.txt:$(HELLOAPP_README) \
	    --no-hash \
	    --sign-key-file $(TEST_KEY_BIN) \
	    --sign-key-id $(TEST_KEY_ID) \
	    --corrupt-body 24

# M34D fixture: helloapp built with --caps "FILE_READ,GUI". The kernel
# self-test reads the install record back, parses CAPS, and verifies
# cap_apply_declared narrows a synthetic proc to those exact bits.
$(HELLOAPP_TPKG_CAPS_RO): $(MKPKG_BIN) \
                          programs/user_gui_helloapp/user_gui_helloapp.elf \
                          $(HELLOAPP_README)
	$(MKPKG) -o $@ \
	    --name helloapp \
	    --version 1.0.0 \
	    --desc "Read-only-caps helloapp fixture (M34D)" \
	    --publisher "tobyOS-builtin" \
	    --caps "FILE_READ,GUI" \
	    --app "Hello Pkg|/data/apps/helloapp.elf" \
	    --file /data/apps/helloapp.elf:programs/user_gui_helloapp/user_gui_helloapp.elf \
	    --file /data/apps/helloapp.txt:$(HELLOAPP_README)

.PHONY: m34-fixtures
m34-fixtures: $(HELLOAPP_TPKG_CORRUPT) $(HELLOAPP_TPKG_BADHASH) \
              $(HELLOAPP_TPKG_NOHASH)  $(HELLOAPP_TPKG_BADFHASH) \
              $(HELLOAPP_TPKG_V2_CORRUPT) \
              $(TRUST_DB) \
              $(HELLOAPP_TPKG_SIGNED) \
              $(HELLOAPP_TPKG_SIG_UNKNOWN) \
              $(HELLOAPP_TPKG_SIG_TAMPER) \
              $(HELLOAPP_TPKG_CAPS_RO)

# ---------- Milestone 32: Developer SDK ---------------------------
#
# Builds an out-of-tree-friendly distribution under build/sdk-<ver>/
# containing the public ABI header, the libtoby static archive (libc-shape
# helpers + the TobyTK widget toolkit), the canonical linker script, the
# three samples, the three templates, and the API docs. The committed
# sdk/ tree provides the SDK-authored sources (linker script, toby/
# headers, samples, templates, docs); `make sdk` mirrors the kernel-
# tree-owned pieces (libtoby headers, abi.h, libtoby.a, crt0.o) on
# top of that to produce the assembled SDK ready for tarball.
#
# Targets:
#   pkgbuild     -- host binary (build/pkgbuild)
#   sdk          -- assemble build/sdk-<ver>/
#   sdk-samples  -- build the three samples USING the staged SDK,
#                   producing build/samples/<name>.tpkg
#   sdk-pack     -- gz-tar the staged SDK into build/tobyOS-sdk-<ver>.tar.gz
#
# The sample .tpkg files are also baked into the initrd at /repo/ so a
# fresh boot can immediately `pkg install /repo/notes_app.tpkg`.
#
# The SDK_VERSION / SDK_STAGE / SDK_TARBALL / PKGBUILD_BIN / SDK_SAMPLES
# / SDK_SAMPLE_TPKGS variables live up near the HELLOAPP_TPKG block --
# they MUST be defined before $(INITRD_TAR) references them in its
# prereq list, otherwise the initrd would be assembled without the
# sample .tpkgs and the cp recipe would fail at "No such file".

# Host pkgbuild tool. Same shape as MKPKG_BIN -- one TU, one host
# compile, no link-time deps beyond libc.
$(PKGBUILD_BIN): $(PKGBUILD_SRC)
	mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $<

pkgbuild: $(PKGBUILD_BIN)

# GUI for SDK apps is TobyTK (<toby/tk.h>), which lives inside libtoby.a (tk.c is
# in LIBTOBY_C_SRCS). There is no separate GUI archive: an SDK GUI app links the
# same crt0.o + libtoby.a as a CLI app and the linker drops the widget code it
# doesn't call. The header is mirrored from libtoby/include/toby/tk.h in
# sdk-headers (one source of truth). The old libtoby_gui.a / <toby/gui.h>
# toolkit (programs/common/toby_gui.c) was retired in the unify-on-TobyTK work.

# sdk-tree: stamp the COMMITTED sdk/ contents into the staging area.
# This wipes the staging area first so re-staging is reproducible.
.PHONY: sdk sdk-tree sdk-headers sdk-libs sdk-tools sdk-samples sdk-pack pkgbuild
sdk-tree:
	@rm -rf $(SDK_STAGE)
	@mkdir -p $(SDK_STAGE)/include/toby $(SDK_STAGE)/include/tobyos/abi
	@mkdir -p $(SDK_STAGE)/include/sys
	@mkdir -p $(SDK_STAGE)/lib $(SDK_STAGE)/tools $(SDK_STAGE)/docs
	@mkdir -p $(SDK_STAGE)/templates $(SDK_STAGE)/samples
	cp sdk/VERSION    $(SDK_STAGE)/
	cp sdk/README.md  $(SDK_STAGE)/
	cp -R sdk/docs/.       $(SDK_STAGE)/docs/
	cp -R sdk/templates/.  $(SDK_STAGE)/templates/
	cp -R sdk/samples/.    $(SDK_STAGE)/samples/
	cp sdk/include/toby/*.h   $(SDK_STAGE)/include/toby/
	cp sdk/include/toby/*.hpp $(SDK_STAGE)/include/toby/
	cp sdk/lib/program.ld     $(SDK_STAGE)/lib/

# Mirror the libtoby public headers + the frozen ABI header into the
# SDK tree. These are NOT committed into sdk/ so we have one source
# of truth (libtoby/include/) and a generated copy in the SDK.
sdk-headers: sdk-tree
	cp $(LIBTOBY_INC)/*.h         $(SDK_STAGE)/include/
	cp $(LIBTOBY_INC)/sys/*.h     $(SDK_STAGE)/include/sys/
	cp $(LIBTOBY_INC)/toby/tk.h   $(SDK_STAGE)/include/toby/
	cp include/tobyos/abi/abi.h   $(SDK_STAGE)/include/tobyos/abi/

# Drop the prebuilt libtoby.a and crt0.o into the SDK lib dir. GUI (TobyTK) is
# inside libtoby.a -- no separate GUI archive.
sdk-libs: sdk-tree $(LIBTOBY_AR) $(LIBTOBY_CRT0)
	cp $(LIBTOBY_AR)   $(SDK_STAGE)/lib/
	cp $(LIBTOBY_CRT0) $(SDK_STAGE)/lib/

# Drop the host pkgbuild binary into the SDK tools dir.
sdk-tools: sdk-tree $(PKGBUILD_BIN)
	cp $(PKGBUILD_BIN) $(SDK_STAGE)/tools/

# Umbrella: assemble the full SDK.
sdk: sdk-headers sdk-libs sdk-tools
	@echo "[sdk] assembled $(SDK_STAGE)/"

# Build a sample app using the staged SDK and produce its .tpkg.
#
# We use a STATIC PATTERN RULE (`$(LIST): pattern: prereqs`) instead of
# an implicit pattern rule. Implicit pattern rules are rejected by Make
# when their non-phony prereqs (e.g. `$(SDK_STAGE)/samples/%/tobyapp.toml`)
# don't exist on disk, which is exactly the case after `make clean` --
# the manifest is stamped out by `sdk-tree`, but Make can't match a
# pattern rule against a not-yet-existing file produced by a phony
# target. Static pattern rules don't apply that rejection logic, so we
# can list `sdk` (phony) as the only prereq and rely on it to materialise
# the staging tree before the recipe runs.
#
# Each sample's Makefile honours TOBYOS_SDK; we point it at the absolute
# SDK_STAGE path so relative -I/-T flags resolve.
#
# On MSYS2/Windows hosts `$(abspath ...)` returns a Cygwin-style path
# like /c/CustomOS/.../sdk-1.0.0 which the native clang.exe in the
# sample build CANNOT recognise (it silently treats `/c/...` as a
# nonexistent dir, then "stdio.h not found"). We resolve it through
# `cygpath -m` to emit a mixed Win/POSIX path (C:/CustomOS/.../sdk-1.0.0)
# that BOTH the MSYS2 shell utilities and native clang understand. On
# Linux/macOS hosts cygpath is absent, so the recipe falls back to the
# raw $(abspath) value -- which already works there because there's no
# path translation layer to fight.
SDK_STAGE_ABS := $(shell cygpath -m '$(abspath $(SDK_STAGE))' 2>/dev/null || echo '$(abspath $(SDK_STAGE))')
$(SDK_SAMPLE_TPKGS): build/samples/%.tpkg: sdk $(PKGBUILD_BIN)
	@mkdir -p $(dir $@)
	$(MAKE) -C $(SDK_STAGE)/samples/$* TOBYOS_SDK='$(SDK_STAGE_ABS)' all
	OUT="$$(cygpath -m '$(abspath $@)' 2>/dev/null || echo '$(abspath $@)')"; \
	$(PKGBUILD_BIN) $(SDK_STAGE)/samples/$*/tobyapp.toml -o "$$OUT" -v

sdk-samples: $(SDK_SAMPLE_TPKGS)
	@echo "[sdk] sample tpkgs: $(SDK_SAMPLE_TPKGS)"

sdk-pack: sdk sdk-samples
	cd build && tar -czf $(notdir $(SDK_TARBALL)) sdk-$(SDK_VERSION)
	@echo "[sdk] packed $(SDK_TARBALL)"

# Same as default `make all` today.
.PHONY: fastboot
fastboot:
	rm -f src/kernel.o src/net.o
	$(MAKE) all

# Full boot-time network smoke (example.com DNS+TCP+HTTP) and 3 s DHCP
# budget — use for CI / regression, not daily dev on real hardware.
.PHONY: fullboot
fullboot:
	rm -f src/kernel.o src/net.o
	$(MAKE) all EXTRA_CFLAGS=

# Live ISO rebuild with minimal kmain harness work (faster USB boots).
.PHONY: quickiso
quickiso:
	$(MAKE) clean
	$(MAKE) EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT"

# Milestone-17 self-test build. Wipes intermediate artifacts and
# rebuilds the kernel with -DPKG_M17_SELFTEST defined so the boot
# sequence runs the install/update/upgrade/rollback demo and writes
# clearly-tagged "[m17-selftest] step N" lines to serial.log.
# Use the test_pkg_upgrade.ps1 driver to boot + verify.
m17test:
	rm -f src/pkg.o src/kernel.o $(KERNEL) $(ISO)
	$(MAKE) all CFLAGS="$(CFLAGS) -DPKG_M17_SELFTEST"

# Milestone-20 boot self-test build. Wipes intermediate artifacts
# and rebuilds the kernel with -DINSTALL_M20_SELFTEST defined so the
# boot sequence runs installer_run against the primary IDE disk and
# writes "[m20-selftest] step N" lines to serial.log. Use with a
# fresh 16 MiB disk.img so the install has a valid target:
#
#     make wipe-disk
#     make m20test
#     make run             # wait for "[m20-selftest] SUCCESS"
#     make run-installed   # boot installed disk with no CD-ROM
m20test:
	rm -f src/installer.o src/kernel.o $(KERNEL) $(ISO) $(BASE_ISO) $(INSTALL_IMG)
	$(MAKE) all CFLAGS="$(CFLAGS) -DINSTALL_M20_SELFTEST"

# Milestone-22 ACPI shutdown self-test build. Wipes intermediate
# artifacts and rebuilds the kernel with -DACPI_M22_SELFTEST defined
# so kmain arms a deadline that fires acpi_shutdown() ~12s after
# boot.  Combined with the Makefile change that drops -no-shutdown
# from every run-* target, QEMU exits cleanly when the trigger fires
# -- which is the simplest end-to-end demonstration that the FADT
# parse + PM1a_CNT write actually works on this platform.
#
# Usage:
#     make m22shutdowntest
#     make run             # waits ~12s, then QEMU exits with code 0
#                          # (look for "[m22-selftest] deadline reached"
#                          #  on serial.log just before the exit)
m22shutdowntest:
	rm -f src/acpi.o src/kernel.o $(KERNEL) $(ISO)
	$(MAKE) all CFLAGS="$(CFLAGS) -DACPI_M22_SELFTEST"

# Milestone-34 security self-test build. Wipes intermediate
# artifacts and rebuilds with -DPKG_M34_SELFTEST so kmain calls
# pkg_m34_selftest() at boot. The harness drives:
#
#   M34A : valid install, then reject of /repo/helloapp_corrupt.tpkg,
#          /repo/helloapp_badhash.tpkg, /repo/helloapp_badfhash.tpkg,
#          and a sanity install of /repo/helloapp_nohash.tpkg under
#          the default WARN policy.
#   (M34B/C/D/E are appended phase by phase as they land.)
#
# Each step prints "[m34a-selftest] step N: PASS|FAIL" and the
# wrapper "[m34-selftest] milestone 34 end (failures=N) -- PASS|FAIL"
# so an external PowerShell driver can grep verdicts off serial.log
# without driving the kernel shell over QMP.
m34test:
	rm -f src/pkg.o src/sec.o src/kernel.o $(KERNEL) $(ISO)
	$(MAKE) all EXTRA_CFLAGS="-DPKG_M34_SELFTEST"

# Milestone 35: hardware-compatibility selftest. Wipes the kernel
# objects that compile in the M35 selftest hooks, then rebuilds with
# -DM35_SELFTEST so kmain calls m35X_selftest at the right boot
# stage. The PowerShell driver test_m35.ps1 boots the resulting
# kernel headless and greps serial.log for "[m35X-selftest] ... PASS".
m35test:
	rm -f src/m35selftest.o src/drvconf.o src/drvdb.o src/hwdb.o \
	      src/drvmatch.o src/usbreg.o src/xhci.o src/syscall.o \
	      src/safemode.o src/hwinfo.o src/kernel.o $(KERNEL) $(ISO)
	$(MAKE) all EXTRA_CFLAGS="-DM35_SELFTEST"

# Milestone 36: spawns /bin/selfhosttest at boot for QEMU validation.
m36test:
	rm -f src/kernel.o $(KERNEL) $(ISO)
	$(MAKE) all EXTRA_CFLAGS="-DM36_SELFTEST"

# Milestone 34G integrated security validation suite. Wipes the
# kernel + sectest object so the autorun hook gets compiled in, then
# rebuilds with -DSECTEST_AUTORUN. The PowerShell driver is
# test_m34g.ps1 -- it boots tobyOS headless, waits for the
# "[securitytest] OVERALL: PASS" marker, and exits with the same
# verdict. Unlike m34test this does NOT depend on -DPKG_M34_SELFTEST,
# so a default-build kernel is what's actually exercised.
sectest:
	rm -f src/sectest.o src/kernel.o src/shell.o $(KERNEL) $(ISO)
	$(MAKE) all EXTRA_CFLAGS="-DSECTEST_AUTORUN"

# POSIX shell compatibility smoke build. Keeps QUICK_BOOT so the boot
# log is dominated by the shell-language sentinels the driver checks.
posixshtest:
	rm -f src/kernel.o src/shell.o src/printk.o $(KERNEL) $(ISO)
	$(MAKE) all EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DPOSIX_SHELL_SELFTEST"

# Milestone-24D HTTP-pkg self-test build. Wipes intermediate
# artifacts and rebuilds with -DHTTP_M24D_SELFTEST so kmain's
# http_m24d_selftest() drives the full
#     httpget /m24d_smoke.txt
#  -> pkg install http://.../helloapp.tpkg
#  -> pkg info helloapp
#  -> pkg remove helloapp
# cycle against an HTTP server expected at 10.0.2.2:8000. Use the
# tests/test_m24d.ps1 driver which stages the test directory, starts
# `py -m http.server 8000` in the background, runs `make run`, and
# greps serial.log for "[m24d-selftest] SUCCESS".
m24dtest:
	rm -f src/http.o src/kernel.o $(KERNEL) $(ISO)
	$(MAKE) all CFLAGS="$(CFLAGS) -DHTTP_M24D_SELFTEST"

# ---------- Run ----------

run: $(ISO) $(DISK_IMG)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log $(LOGDIR)/net.pcap
	$(QEMU) -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -boot d \
	        -smp 4 \
	        -netdev user,id=net0,hostfwd=udp::5555-:5555 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -object filter-dump,id=fd0,netdev=net0,file=$(LOGDIR)/net.pcap \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== debugcon (port 0xE9) ==="; \
	cat $(LOGDIR)/debug.log 2>/dev/null || echo "(empty)"; \
	echo ""; echo "=== serial (COM1) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"; \
	echo ""; echo "=== qemu.log -- non-timer entries ==="; \
	grep -v "INT=0x08" $(LOGDIR)/qemu.log | head -n 200 2>/dev/null || echo "(empty)"

run-ssh: $(ISO) $(DISK_IMG)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log $(LOGDIR)/net.pcap
	@echo "SSH will be available at localhost:2222 once the guest finishes booting."
	$(QEMU) -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -boot d \
	        -smp 4 \
	        -object rng-builtin,id=rng0 \
	        -device virtio-rng-pci,rng=rng0,disable-legacy=on \
	        -netdev user,id=net0,hostfwd=tcp::2222-:22,hostfwd=udp::5555-:5555 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -object filter-dump,id=fd0,netdev=net0,file=$(LOGDIR)/net.pcap \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== debugcon (port 0xE9) ==="; \
	cat $(LOGDIR)/debug.log 2>/dev/null || echo "(empty)"; \
	echo ""; echo "=== serial (COM1, SSH) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

# ---------- Milestone 20: boot the INSTALLED disk ----------
#
# Boots straight off the target disk image -- no -cdrom. Limine's
# hybrid MBR (written into LBA 0 by the installer when it flashes
# install.img) jumps to limine-bios.sys embedded in the first 4 MiB
# of the disk, which loads the kernel + initrd. The kernel then
# mounts /data from the tobyfs region at LBA $(INSTALL_BOOT_BYTES)/512.
#
# DISK defaults to disk.img (the same primary-master image the live
# ISO writes to when you run `install --yes` from the shell).
# Override with `make run-installed DISK=some_other.img` to boot a
# different installed target.
DISK ?= $(DISK_IMG)
run-installed:
	@if [ ! -f $(DISK) ]; then \
	    echo "error: $(DISK) does not exist."; \
	    echo "Typical install flow:"; \
	    echo "  1. make                          # build kernel + ISO"; \
	    echo "  2. make run                      # boot live ISO"; \
	    echo "  3. (in the shell) install --yes  # flash disk.img"; \
	    echo "  4. (close QEMU)"; \
	    echo "  5. make run-installed            # boot from installed disk"; \
	    exit 1; \
	fi
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log $(LOGDIR)/net.pcap
	$(QEMU) -drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
	        -boot c \
	        -smp 4 \
	        -netdev user,id=net0,hostfwd=udp::5555-:5555 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -object filter-dump,id=fd0,netdev=net0,file=$(LOGDIR)/net.pcap \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== debugcon (port 0xE9) ==="; \
	cat $(LOGDIR)/debug.log 2>/dev/null || echo "(empty)"; \
	echo ""; echo "=== serial (COM1) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

# ---------- Milestone 21 step 1: AHCI smoke targets ----------
#
# `make run-ahci` boots the live ISO under QEMU's q35 chipset with an
# explicit ICH9 AHCI controller (vid 8086, did 2922), attached via
# `-device ahci,id=ahci0` (the `ahci` device is an alias for ich9-ahci
# in modern QEMU and is NOT auto-instantiated by -machine q35). The
# disk hangs off port 0 of that controller as a SATA drive instead of
# the legacy if=ide shortcut.
#
# Same disk image, same network setup as `make run`. blk_ata's PCI
# probe finds nothing (no PIIX3 IDE on q35), and blk_ahci picks up the
# ICH9 controller. /data still mounts because kernel.c calls
# blk_get_first(), which now returns the AHCI disk.
#
# Useful for catching driver bugs that only surface on real hardware
# without breaking the existing make run / test_install.ps1 flow.
run-ahci: $(ISO) $(DISK_IMG)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log $(LOGDIR)/net.pcap
	$(QEMU) -machine q35 \
	        -cdrom $(ISO) \
	        -device ahci,id=ahci0 \
	        -drive file=$(DISK_IMG),format=raw,if=none,id=hd0 \
	        -device ide-hd,drive=hd0,bus=ahci0.0 \
	        -boot d \
	        -smp 4 \
	        -netdev user,id=net0,hostfwd=udp::5555-:5555 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -object filter-dump,id=fd0,netdev=net0,file=$(LOGDIR)/net.pcap \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== debugcon (port 0xE9) ==="; \
	cat $(LOGDIR)/debug.log 2>/dev/null || echo "(empty)"; \
	echo ""; echo "=== serial (COM1) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

run-ahci-uefi: $(ISO) $(DISK_IMG) $(OVMF_CODE) $(OVMF_VARS)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log $(LOGDIR)/net.pcap
	$(QEMU) -machine q35 \
	        -drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF_CODE) \
	        -drive if=pflash,unit=1,format=raw,file=$(OVMF_VARS) \
	        -cdrom $(ISO) \
	        -device ahci,id=ahci0 \
	        -drive file=$(DISK_IMG),format=raw,if=none,id=hd0 \
	        -device ide-hd,drive=hd0,bus=ahci0.0 \
	        -smp 4 \
	        -netdev user,id=net0,hostfwd=udp::5555-:5555 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -object filter-dump,id=fd0,netdev=net0,file=$(LOGDIR)/net.pcap \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== serial (UEFI/q35/AHCI) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

# ---------- Milestone 21 step 2: NVMe smoke targets ----------
#
# `make run-nvme` boots the live ISO under QEMU with an emulated NVMe
# controller (vid 1b36, did 0010 -- "QEMU NVMe Ctrl") and the data disk
# attached as namespace 1. We stay on i440fx (the same chipset as
# `make run`) so the only thing that changes vs the baseline is the
# storage controller -- this exercises the NVMe driver in isolation.
#
# Boot order:
#   - blk_ata sees the i440fx PIIX3 IDE controller at 00:01.1 but
#     ata_identify returns -2 (the IDE bus has no drives wired up
#     because we used if=none on the only disk). It declines cleanly.
#   - blk_ahci finds nothing (no AHCI on i440fx).
#   - blk_nvme binds to the NVMe HBA, runs IDENTIFY CONTROLLER +
#     IDENTIFY NS 1, registers nvme0:n1.
#   - blk_get_first() returns nvme0:n1, /data mounts via NVMe.
#
# Useful for catching driver bugs without breaking the existing make
# run / run-ahci / test_install.ps1 flows.
run-nvme: $(ISO) $(DISK_IMG)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log $(LOGDIR)/net.pcap
	$(QEMU) -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=none,id=hd0 \
	        -device nvme,drive=hd0,serial=tobyOSnvme0 \
	        -boot d \
	        -smp 4 \
	        -netdev user,id=net0,hostfwd=udp::5555-:5555 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -object filter-dump,id=fd0,netdev=net0,file=$(LOGDIR)/net.pcap \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== debugcon (port 0xE9) ==="; \
	cat $(LOGDIR)/debug.log 2>/dev/null || echo "(empty)"; \
	echo ""; echo "=== serial (COM1) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

run-nvme-uefi: $(ISO) $(DISK_IMG) $(OVMF_CODE) $(OVMF_VARS)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log $(LOGDIR)/net.pcap
	$(QEMU) -drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF_CODE) \
	        -drive if=pflash,unit=1,format=raw,file=$(OVMF_VARS) \
	        -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=none,id=hd0 \
	        -device nvme,drive=hd0,serial=tobyOSnvme0 \
	        -smp 4 \
	        -netdev user,id=net0,hostfwd=udp::5555-:5555 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -object filter-dump,id=fd0,netdev=net0,file=$(LOGDIR)/net.pcap \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== serial (UEFI/NVMe) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

# ---------- virtio-net (modern transport) ----------
#
# Same disk path as `make run` (i440fx + IDE), but the e1000 NIC is
# replaced with a virtio-net-pci device. We pass `disable-legacy=on`
# so QEMU exposes the modern (V1) transport with device id 0x1041 and
# does NOT also emit the legacy I/O port BAR0; this matches what real
# vhost-net + Linux kernels expect and ensures our driver exercises
# the modern PCI cap walk + memory-mapped doorbells path.
#
# Expected boot behaviour:
#   - blk_ata binds to IDE primary master (same as `make run`).
#   - blk_ahci / blk_nvme stay silent (no AHCI / NVMe in i440fx).
#   - e1000 stays silent (no Intel 8254x present).
#   - virtio_net binds to 1AF4:1041, walks the cap list, finds
#     COMMON_CFG / NOTIFY_CFG / DEVICE_CFG, negotiates VERSION_1 +
#     NET_F_MAC, brings up two virtqueues, registers virtio-net:bb:ss.f.
#   - net_init() picks up virtio-net as net_default(), reads MAC,
#     ARPs the SLIRP gateway, and the UDP echo socket comes up on
#     :5555 just like with e1000.
#
# To verify a round trip: from the host (after the login screen),
# run:  echo hello | nc -u 127.0.0.1 5555
# (or use the user_netecho program from inside the OS).
run-virtio-net: $(ISO) $(DISK_IMG)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log $(LOGDIR)/net.pcap
	$(QEMU) -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -boot d \
	        -smp 4 \
	        -netdev user,id=net0,hostfwd=udp::5555-:5555 \
	        -device virtio-net-pci,netdev=net0,disable-legacy=on,mac=52:54:00:12:34:56 \
	        -object filter-dump,id=fd0,netdev=net0,file=$(LOGDIR)/net.pcap \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== debugcon (port 0xE9) ==="; \
	cat $(LOGDIR)/debug.log 2>/dev/null || echo "(empty)"; \
	echo ""; echo "=== serial (COM1, virtio-net) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

# ---------- M35B: virtio-blk smoke target ----------
#
# Adds a SECOND disk image (build/vblk_test.img) attached as a modern
# virtio-blk-pci device. The boot disk stays on IDE so /data and the
# rest of the boot path are untouched -- the new disk shows up as a
# separate blk_dev "vblk0" that the M35B selftest reads from.
#
# disable-legacy=on / disable-modern=off pins the device to the modern
# transport our driver actually implements.
build/vblk_test.img:
	@mkdir -p build
	@if [ ! -f build/vblk_test.img ]; then \
	    truncate -s 4194304 build/vblk_test.img; \
	    printf 'TOBYM35B-VBLK0\x00' | dd of=build/vblk_test.img bs=1 \
	            count=16 conv=notrunc status=none; \
	    echo "[m35b] created build/vblk_test.img (4 MiB, sentinel @LBA0)"; \
	fi

run-virtio-blk: $(ISO) $(DISK_IMG) build/vblk_test.img
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log
	$(QEMU) -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -drive file=build/vblk_test.img,format=raw,if=none,id=vblk0 \
	        -device virtio-blk-pci,drive=vblk0,disable-legacy=on,disable-modern=off \
	        -boot d \
	        -smp 4 \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot -no-shutdown -display none

run-virtio-net-uefi: $(ISO) $(DISK_IMG) $(OVMF_CODE) $(OVMF_VARS)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log $(LOGDIR)/net.pcap
	$(QEMU) -drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF_CODE) \
	        -drive if=pflash,unit=1,format=raw,file=$(OVMF_VARS) \
	        -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -smp 4 \
	        -netdev user,id=net0,hostfwd=udp::5555-:5555 \
	        -device virtio-net-pci,netdev=net0,disable-legacy=on,mac=52:54:00:12:34:56 \
	        -object filter-dump,id=fd0,netdev=net0,file=$(LOGDIR)/net.pcap \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== serial (UEFI/virtio-net) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

# ---------- e1000e (Intel 82574L) ----------
#
# Same disk path as `make run`, but the e1000 NIC is replaced with
# QEMU's `-device e1000e` (which models exactly the 82574L silicon
# our src/e1000e.c targets, PCI id 8086:10D3). The point of this
# target is to prove that the PCI driver registry correctly picks
# the e1000e probe over the e1000 probe based on device id, and
# that net_init() comes up the same way regardless of which NIC is
# present.
#
# Expected boot behaviour:
#   - blk_ata binds to IDE primary master (same as `make run`).
#   - e1000 stays silent (no 82540EM present).
#   - e1000e binds to 8086:10D3, RX/TX rings come up, MAC read out
#     of RAL0/RAH0, registered as e1000e:bb:ss.f.
#   - net_init() picks up e1000e as net_default(), ARPs the SLIRP
#     gateway, UDP echo socket binds on :5555.
run-e1000e: $(ISO) $(DISK_IMG)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log $(LOGDIR)/net.pcap
	$(QEMU) -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -boot d \
	        -smp 4 \
	        -netdev user,id=net0,hostfwd=udp::5555-:5555 \
	        -device e1000e,netdev=net0,mac=52:54:00:12:34:56 \
	        -object filter-dump,id=fd0,netdev=net0,file=$(LOGDIR)/net.pcap \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== debugcon (port 0xE9) ==="; \
	cat $(LOGDIR)/debug.log 2>/dev/null || echo "(empty)"; \
	echo ""; echo "=== serial (COM1, e1000e) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

run-e1000e-uefi: $(ISO) $(DISK_IMG) $(OVMF_CODE) $(OVMF_VARS)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log $(LOGDIR)/net.pcap
	$(QEMU) -drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF_CODE) \
	        -drive if=pflash,unit=1,format=raw,file=$(OVMF_VARS) \
	        -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -smp 4 \
	        -netdev user,id=net0,hostfwd=udp::5555-:5555 \
	        -device e1000e,netdev=net0,mac=52:54:00:12:34:56 \
	        -object filter-dump,id=fd0,netdev=net0,file=$(LOGDIR)/net.pcap \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== serial (UEFI/e1000e) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

# ---------- rtl8139 (sanity check that rtl8169 driver IGNORES it) ----------
#
# QEMU does NOT emulate the gigabit Realtek family (RTL8168/8111),
# only the older RTL8139 (Fast-Ethernet, vendor 0x10EC, device
# 0x8139). Our src/rtl8169.c match table covers 0x10EC:8168/8169/8161
# and DELIBERATELY EXCLUDES 0x8139 (different MAC layer).
#
# This target boots the OS with `-device rtl8139` AND `-device e1000`
# present on the bus. Expected behaviour:
#   - PCI enumeration finds 10EC:8139 (the Realtek FE) AND 8086:100E
#     (the Intel e1000) as separate functions.
#   - rtl8169 driver is registered, walks the bus, but its match
#     table sees only 0x8139 from Realtek -- declines the match
#     and stays silent. No crash, no spurious bind, "0 of N bound"
#     for the Realtek device.
#   - e1000 driver binds to 8086:100E and the network stack comes
#     up via the Intel NIC.
#   - test traffic flows over e1000 (so the system is still usable
#     for ICMP/UDP echo in the QEMU SLIRP).
#
# This proves the registry's vendor+device match logic correctly
# distinguishes sibling Realtek chips. Real-hardware verification of
# the gigabit path itself happens out-of-band on physical 8168 silicon.
run-rtl8139: $(ISO) $(DISK_IMG)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log $(LOGDIR)/net.pcap
	$(QEMU) -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -boot d \
	        -smp 4 \
	        -netdev user,id=net0,hostfwd=udp::5555-:5555 \
	        -device rtl8139,netdev=net0,mac=52:54:00:12:34:78 \
	        -netdev user,id=net1 \
	        -device e1000,netdev=net1,mac=52:54:00:12:34:56 \
	        -object filter-dump,id=fd0,netdev=net0,file=$(LOGDIR)/net.pcap \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== debugcon (port 0xE9) ==="; \
	cat $(LOGDIR)/debug.log 2>/dev/null || echo "(empty)"; \
	echo ""; echo "=== serial (COM1, rtl8139+e1000) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

run-rtl8139-uefi: $(ISO) $(DISK_IMG) $(OVMF_CODE) $(OVMF_VARS)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log $(LOGDIR)/net.pcap
	$(QEMU) -drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF_CODE) \
	        -drive if=pflash,unit=1,format=raw,file=$(OVMF_VARS) \
	        -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -smp 4 \
	        -netdev user,id=net0,hostfwd=udp::5555-:5555 \
	        -device rtl8139,netdev=net0,mac=52:54:00:12:34:78 \
	        -netdev user,id=net1 \
	        -device e1000,netdev=net1,mac=52:54:00:12:34:56 \
	        -object filter-dump,id=fd0,netdev=net0,file=$(LOGDIR)/net.pcap \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== serial (UEFI/rtl8139+e1000) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

# ---------- xHCI (USB-HID keyboard + mouse) ----------
#
# QEMU's qemu-xhci device exposes a USB 3.0 controller (PCI vendor
# 0x1b36 / device 0x000d, matched by our class triple Serial Bus /
# USB / xHCI prog-if). usb-kbd + usb-mouse pre-attach a Boot HID
# keyboard + mouse to its root-hub ports 1 and 2.
#
# Expected boot behaviour (visible in serial.log):
#   - PCI enumeration finds 1b36:000d at some bus:slot.fn
#   - xhci driver binds via the class match; logs HCIVERSION,
#     CSZ, MaxSlots, MaxPorts, doorbell + runtime offsets
#   - port enumeration: ports 1 + 2 report CCS=1, devices reset,
#     Address Device assigns slot ids, GET_DESCRIPTOR succeeds
#   - usb-hid claims both interfaces, SET_PROTOCOL(BOOT) + SET_IDLE(0)
#     succeed, first interrupt-IN transfer submitted on each ring
#   - login GUI appears as usual; both PS/2 (i440fx default) and USB
#     paths can deliver keystrokes to the focused window
#
# We KEEP the i440fx default PS/2 keyboard + mouse on the bus too --
# they coexist with USB HID, and the rest of the milestone-21
# regression suite still sees identical input behaviour.
run-xhci: $(ISO) $(DISK_IMG)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log
	$(QEMU) -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -boot d \
	        -smp 4 \
	        -netdev user,id=net0 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -device qemu-xhci,id=usb0 \
	        -device usb-kbd,bus=usb0.0 \
	        -device usb-mouse,bus=usb0.0 \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== debugcon (port 0xE9) ==="; \
	cat $(LOGDIR)/debug.log 2>/dev/null || echo "(empty)"; \
	echo ""; echo "=== serial (COM1, xhci+usb-kbd+usb-mouse) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

run-xhci-uefi: $(ISO) $(DISK_IMG) $(OVMF_CODE) $(OVMF_VARS)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log
	$(QEMU) -drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF_CODE) \
	        -drive if=pflash,unit=1,format=raw,file=$(OVMF_VARS) \
	        -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -smp 4 \
	        -netdev user,id=net0 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -device qemu-xhci,id=usb0 \
	        -device usb-kbd,bus=usb0.0 \
	        -device usb-mouse,bus=usb0.0 \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== serial (UEFI/xhci+usb-kbd+usb-mouse) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

# ---------- Milestone 23C: xHCI + USB Mass Storage ----------
#
# Boots the live ISO with a USB 3 controller (qemu-xhci) AND a 16 MiB
# usb-storage device backed by a host-side raw FAT32 image. The kernel
# should:
#
#   1. xhci_register + bind the qemu-xhci controller as usual.
#   2. Discover the usb-storage device on root-hub port 3 (kbd + mouse
#      are on ports 1+2). Speed=HS for QEMU's usb-storage today.
#   3. usb_msc_probe runs Get-Max-LUN, INQUIRY, READ_CAPACITY (10) and
#      registers a "usb0" blk_dev with sector_count = 32768 (16 MiB / 512).
#   4. The boot path's /usb mount loop probes usb0 for a FAT32 BPB,
#      mounts it at /usb, then runs the [usb-msc-test] smoke suite.
#
# The same FAT32 image survives reboots because qemu-storage writes
# back through the file. Run twice to verify the round-trip test
# unlinked USBTEST.LOG cleanly.
USB_STICK_IMG := build/usbstick.img
USB_STICK_BYTES := $(shell echo $$((16 * 1024 * 1024)))

usb-stick-img: $(USB_STICK_IMG)
$(USB_STICK_IMG): $(MKDISK_GPT_BIN)
	@echo "  GEN     $@  (16 MiB raw FAT32 USB stick)"
	@mkdir -p $(dir $@)
	@$(MKDISK_GPT_BIN) --fat32-only $@ $(USB_STICK_BYTES)

run-xhci-usb: $(ISO) $(DISK_IMG) $(USB_STICK_IMG)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log
	$(QEMU) -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -drive if=none,id=usbstick,format=raw,file=$(USB_STICK_IMG) \
	        -boot d \
	        -smp 4 \
	        -netdev user,id=net0 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -device qemu-xhci,id=usb0 \
	        -device usb-kbd,bus=usb0.0 \
	        -device usb-mouse,bus=usb0.0 \
	        -device usb-storage,bus=usb0.0,drive=usbstick \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== debugcon (port 0xE9) ==="; \
	cat $(LOGDIR)/debug.log 2>/dev/null || echo "(empty)"; \
	echo ""; echo "=== serial (COM1, xhci+usb-storage) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

# ---------- M26B: USB hub-aware boot ----------
#
# Boots the live ISO with:
#   - qemu-xhci controller on root-hub bus usb0.0
#   - usb-hub at root-hub port 1 -> exposes its own bus usb0.1
#   - usb-kbd, usb-mouse, usb-storage attached BEHIND the hub
#
# The kernel must:
#   1. Address the hub on slot N and set Slot Context Hub=1.
#   2. Walk hub.NumberOfPorts via class GET_PORT_STATUS.
#   3. For each connected port, allocate a fresh slot, address it
#      with route_string set + RootHubPortNumber pointing at port 1.
#   4. Run the same HID / MSC class probes as the root-hub path.
#
# After boot:
#   `devlist hub`  shows hub1-1 (slot=N nports=8 ...)
#   `devlist usb`  shows usb1-1.K records (depth>0) for each leaf
#   `usbtest hub`  exits 0 with "PASS" line
run-xhci-hub: $(ISO) $(DISK_IMG) $(USB_STICK_IMG)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log
	$(QEMU) -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -drive if=none,id=usbstick,format=raw,file=$(USB_STICK_IMG) \
	        -boot d \
	        -smp 4 \
	        -netdev user,id=net0 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -device qemu-xhci,id=usb0 \
	        -device usb-hub,bus=usb0.0,port=1,id=hub1 \
	        -device usb-kbd,bus=usb0.0,port=1.1 \
	        -device usb-mouse,bus=usb0.0,port=1.2 \
	        -device usb-storage,bus=usb0.0,port=1.3,drive=usbstick \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== debugcon (port 0xE9) ==="; \
	cat $(LOGDIR)/debug.log 2>/dev/null || echo "(empty)"; \
	echo ""; echo "=== serial (COM1, xhci+hub+kbd+mouse+storage) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

# M23D: combined boot exercising EVERY storage path at once --
#   - GPT-partitioned IDE disk (-> /data + /fat + /ext)
#   - raw FAT32 USB stick      (-> /usb)
# Useful as a single end-to-end "all four mounts coexist" smoke test.
run-gpt-usb: $(ISO) $(GPT_DISK_IMG) $(USB_STICK_IMG)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log
	$(QEMU) -cdrom $(ISO) \
	        -drive file=$(GPT_DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -drive if=none,id=usbstick,format=raw,file=$(USB_STICK_IMG) \
	        -boot d \
	        -smp 4 \
	        -netdev user,id=net0 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -device qemu-xhci,id=usb0 \
	        -device usb-kbd,bus=usb0.0 \
	        -device usb-mouse,bus=usb0.0 \
	        -device usb-storage,bus=usb0.0,drive=usbstick \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== serial (GPT+USB combined) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

run-xhci-usb-uefi: $(ISO) $(DISK_IMG) $(USB_STICK_IMG) $(OVMF_CODE) $(OVMF_VARS)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log
	$(QEMU) -drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF_CODE) \
	        -drive if=pflash,unit=1,format=raw,file=$(OVMF_VARS) \
	        -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -drive if=none,id=usbstick,format=raw,file=$(USB_STICK_IMG) \
	        -smp 4 \
	        -netdev user,id=net0 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -device qemu-xhci,id=usb0 \
	        -device usb-kbd,bus=usb0.0 \
	        -device usb-mouse,bus=usb0.0 \
	        -device usb-storage,bus=usb0.0,drive=usbstick \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== serial (UEFI/xhci+usb-storage) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

# ---------- Milestone 21 step 7: virtio-gpu smoke targets ----------
#
# `make run-virtio-gpu` boots the live ISO under QEMU's i440fx default
# chipset with `-vga none -device virtio-gpu-pci` (vid 1AF4, did 1050,
# modern non-transitional). The vgabios-virtio.bin option ROM that QEMU
# auto-attaches gives Limine a VBE framebuffer to set up at boot, so
# gfx_init still gets a real (vgabios-emulated) FB. Then our
# src/virtio_gpu.c driver binds, runs GET_DISPLAY_INFO + RESOURCE_CREATE_2D
# + RESOURCE_ATTACH_BACKING + SET_SCANOUT, and installs its gfx_backend
# so subsequent gfx_flip()s push pixels via TRANSFER_TO_HOST_2D +
# RESOURCE_FLUSH instead of the Limine memcpy fallback. Everything else
# (IDE disk, e1000 NIC) is identical to `make run` so the GUI login
# flow looks the same on serial.log.
#
# Expected serial.log highlights:
#   - "[virtio-gpu] probing 00:bb.f  (vid:did 1af4:1050)"
#   - "[virtio-gpu] scanout 0: 1024x768 (preferred)"
#   - "[virtio-gpu] backing: 3072 KiB at phys=..."
#   - "[virtio-gpu] backend installed -- gfx_flip now uses TRANSFER+FLUSH"
#   - the usual "[gui] login window opened" / "[trace] flip" cadence
#
# We KEEP the i440fx default PS/2 keyboard + mouse for input; only the
# DISPLAY changes vs. `make run`. `-vga none` removes the default Cirrus.
run-virtio-gpu: $(ISO) $(DISK_IMG)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log
	$(QEMU) -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -boot d \
	        -smp 4 \
	        -netdev user,id=net0 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -vga none \
	        -device virtio-gpu-pci \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== debugcon (port 0xE9) ==="; \
	cat $(LOGDIR)/debug.log 2>/dev/null || echo "(empty)"; \
	echo ""; echo "=== serial (COM1, virtio-gpu) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

# `make run-virtio-gpu-gl` boots with the GL-accelerated virtio-gpu variant
# (`virtio-gpu-gl-pci`) plus a host OpenGL context via `-display egl-headless`.
# On a host with a working virglrenderer + GL stack this makes the device
# advertise VIRTIO_GPU_F_VIRGL, so the C24/VIRGL path (CTX_CREATE/SUBMIT_3D)
# goes live. NOTE: the GL device REQUIRES a gl=on display backend -- `-display
# none` is rejected, and on QEMU-for-Windows the only headless GL backend
# (egl-headless) currently crashes, so this target is for Linux/virglrenderer
# hosts. Build the ISO with EXTRA_CFLAGS+=-DVIRGL_BOOT to run the proof harness.
run-virtio-gpu-gl: $(ISO) $(DISK_IMG)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log
	$(QEMU) -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -boot d \
	        -smp 4 \
	        -vga none \
	        -device virtio-gpu-gl-pci \
	        -display egl-headless \
	        -serial file:$(LOGDIR)/serial.log \
	        -no-reboot; \
	echo ""; echo "=== serial (COM1, virtio-gpu-gl) ==="; \
	grep -aE 'virtio-gpu|VIRGL|virgl' $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

run-virtio-gpu-uefi: $(ISO) $(DISK_IMG) $(OVMF_CODE) $(OVMF_VARS)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log
	$(QEMU) -drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF_CODE) \
	        -drive if=pflash,unit=1,format=raw,file=$(OVMF_VARS) \
	        -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -smp 4 \
	        -netdev user,id=net0 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -vga none \
	        -device virtio-gpu-pci \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== serial (UEFI/virtio-gpu) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

# ---------- Real-hardware boot helpers (UEFI + USB-image) ----------
#
# The same tobyOS.iso is now hybrid: it boots BIOS-CDROM, BIOS-HDD,
# UEFI-CDROM, and UEFI-USB. These targets exercise the UEFI paths
# under QEMU using the OVMF firmware blob shipped with QEMU-for-
# Windows (see $(OVMF) at the top of this file). If your distro
# ships a different OVMF path, override OVMF on the command line:
#
#   make run-uefi OVMF=/usr/share/OVMF/OVMF_CODE.fd
#
# Note we use `-bios $(OVMF)` (read-only) instead of the two-pflash
# split (CODE.fd + VARS.fd). For our use that's fine because we
# don't depend on UEFI NVRAM persistence -- removable-media boot
# (EFI/BOOT/BOOTX64.EFI) works regardless. If you want NVMV vars
# persistence later, switch to two -drive if=pflash entries.

# Stage a writable copy of OVMF (CODE + VARS) under build/. CODE is
# copied straight from $(OVMF_SRC). VARS is created blank if
# missing; QEMU/OVMF will populate it on first boot. We never delete
# it on `make clean` of the kernel artifacts -- it's regenerated as
# needed and is harmless to keep around.
# Note: $(OVMF_SRC) is NOT listed as a prerequisite -- on Windows the
# default path "/c/Program Files/qemu/..." contains a space, and make
# would split it on whitespace and try to build "/c/Program" as a
# separate target ("No rule to make target '/c/Program'"). We instead
# do an explicit existence check inside the recipe; OVMF rarely changes
# anyway, so once we've staged a copy under build/ we keep it.
$(OVMF_CODE):
	mkdir -p build
	@if [ ! -f "$(OVMF_SRC)" ]; then \
	    echo "error: OVMF_SRC='$(OVMF_SRC)' not found."; \
	    echo "  install QEMU's UEFI firmware or override OVMF_SRC=..."; \
	    exit 1; \
	fi
	cp "$(OVMF_SRC)" $(OVMF_CODE)

$(OVMF_VARS):
	mkdir -p build
	@echo "[ovmf] creating blank NVRAM vars store ($(OVMF_VARS_SZ) bytes)"
	dd if=/dev/zero of=$(OVMF_VARS) bs=1 count=$(OVMF_VARS_SZ) status=none

run-uefi: $(ISO) $(DISK_IMG) $(OVMF_CODE) $(OVMF_VARS)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log $(LOGDIR)/net.pcap
	$(QEMU) -drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF_CODE) \
	        -drive if=pflash,unit=1,format=raw,file=$(OVMF_VARS) \
	        -cdrom $(ISO) \
	        -drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -smp 4 \
	        -netdev user,id=net0,hostfwd=udp::5555-:5555 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -object filter-dump,id=fd0,netdev=net0,file=$(LOGDIR)/net.pcap \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== serial (UEFI) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

run-installed-uefi: $(OVMF_CODE) $(OVMF_VARS)
	@if [ ! -f $(DISK) ]; then \
	    echo "error: $(DISK) does not exist (run 'make run' + 'install --yes' first)"; \
	    exit 1; \
	fi
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log $(LOGDIR)/net.pcap
	$(QEMU) -drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF_CODE) \
	        -drive if=pflash,unit=1,format=raw,file=$(OVMF_VARS) \
	        -drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
	        -smp 4 \
	        -netdev user,id=net0,hostfwd=udp::5555-:5555 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== serial (installed, UEFI) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

# ---------- M23A: GPT-disk smoke targets ----------
#
# `make run-gpt`        - boot the live ISO with build/disk_gpt.img as
#                         the IDE primary master. The kernel scans GPT,
#                         discovers three partitions (BIOS Boot / tobyOS
#                         data / MS Basic Data), and mounts /data via
#                         the tobyOS-data partition (NOT the legacy
#                         LBA-0 path).
#
# `make run-gpt-uefi`   - same, but boots under OVMF.
# `make run-gpt-nvme`   - same, but the disk is attached via NVMe so
#                         we exercise blk_nvme + GPT in tandem.
#
# Useful evidence in serial.log:
#   [part] ide0:master: GPT detected -- entries=128 ...
#   [part] ide0:master: slot 1 -> ide0:master.p1, LBA 34..2047 ...
#   [part] ide0:master: slot 2 -> ide0:master.p2, LBA 2048..10239 ...
#   [boot] mounted /data via GPT partition 'ide0:master.p2' ...
run-gpt: $(ISO) $(GPT_DISK_IMG)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log $(LOGDIR)/net.pcap
	$(QEMU) -cdrom $(ISO) \
	        -drive file=$(GPT_DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -boot d \
	        -smp 4 \
	        -netdev user,id=net0,hostfwd=udp::5555-:5555 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -object filter-dump,id=fd0,netdev=net0,file=$(LOGDIR)/net.pcap \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== serial (GPT/IDE) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

run-gpt-uefi: $(ISO) $(GPT_DISK_IMG) $(OVMF_CODE) $(OVMF_VARS)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log $(LOGDIR)/net.pcap
	$(QEMU) -drive if=pflash,unit=0,format=raw,readonly=on,file=$(OVMF_CODE) \
	        -drive if=pflash,unit=1,format=raw,file=$(OVMF_VARS) \
	        -cdrom $(ISO) \
	        -drive file=$(GPT_DISK_IMG),format=raw,if=ide,index=0,media=disk \
	        -smp 4 \
	        -netdev user,id=net0,hostfwd=udp::5555-:5555 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -object filter-dump,id=fd0,netdev=net0,file=$(LOGDIR)/net.pcap \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== serial (GPT/UEFI/IDE) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

run-gpt-nvme: $(ISO) $(GPT_DISK_IMG)
	@mkdir -p $(LOGDIR) && rm -f $(LOGDIR)/serial.log $(LOGDIR)/debug.log $(LOGDIR)/qemu.log $(LOGDIR)/net.pcap
	$(QEMU) -cdrom $(ISO) \
	        -drive file=$(GPT_DISK_IMG),format=raw,if=none,id=hd0 \
	        -device nvme,drive=hd0,serial=tobyOSnvme0 \
	        -boot d \
	        -smp 4 \
	        -netdev user,id=net0,hostfwd=udp::5555-:5555 \
	        -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
	        -object filter-dump,id=fd0,netdev=net0,file=$(LOGDIR)/net.pcap \
	        -serial file:$(LOGDIR)/serial.log \
	        -debugcon file:$(LOGDIR)/debug.log \
	        -d int,cpu_reset,guest_errors -D $(LOGDIR)/qemu.log \
	        -no-reboot; \
	echo ""; echo "=== serial (GPT/NVMe) ==="; \
	cat $(LOGDIR)/serial.log 2>/dev/null || echo "(empty)"

# Convenience copy for "I want to flash this with Rufus / dd". Just
# tobyOS.iso renamed -- we already produce a hybrid image. Use the
# DD ("write in DD image mode") option in Rufus, NOT the ISO mode,
# so the byte-exact MBR + GPT + Limine stage1 layout is preserved.
# On Linux/macOS:
#     sudo dd if=tobyOS.iso of=/dev/sdX bs=4M status=progress conv=fsync
USB_IMG := tobyOS-usb.img
usb-image: $(ISO)
	cp $(ISO) $(USB_IMG)
	@echo ""
	@echo "USB image ready: $(USB_IMG)"
	@echo "  - Rufus  : pick this file, choose 'DD Image' write mode"
	@echo "  - dd     : sudo dd if=$(USB_IMG) of=/dev/sdX bs=4M status=progress conv=fsync"
	@echo "  - QEMU   : make run-uefi   (UEFI)   |   make run   (legacy BIOS)"

# ---------- Inspect ELF ----------

inspect: $(KERNEL)
	readelf -h $(KERNEL) 2>/dev/null || objdump -f $(KERNEL) 2>/dev/null || echo "no readelf/objdump found"
	@echo "--- Program headers ---"
	readelf -l $(KERNEL) 2>/dev/null || objdump -p $(KERNEL) 2>/dev/null || echo "no readelf/objdump found"

# ---------- Clean ----------

clean:
	@# Delete the boot images first. On Windows these are commonly held
	@# open by a still-running QEMU window (`make run`), which makes
	@# rm fail with "Device or resource busy". A locked image is not a
	@# real build error -- warn the user and keep going so the rest of
	@# the tree still gets cleaned. The next successful `make` will
	@# overwrite whatever stale image is on disk anyway.
	@rm -f $(ISO) $(USB_IMG) $(BASE_ISO) $(INSTALL_IMG) install-target.img 2>/dev/null || true
	@for f in $(ISO) $(USB_IMG) $(BASE_ISO) $(INSTALL_IMG) install-target.img; do \
	    if [ -e "$$f" ]; then \
	        echo "warning: could not remove '$$f' (likely held by a running QEMU)"; \
	        echo "         close QEMU and re-run 'make clean' if you need a fresh image."; \
	    fi; \
	done
	rm -rf $(OBJS) $(KERNEL) iso build \
	       programs/hello/hello.o programs/hello/hello.elf \
	       programs/user_hello/user_hello.o programs/user_hello/user_hello.elf \
	       programs/user_bad/user_bad.o programs/user_bad/user_bad.elf \
	       programs/user_echo/user_echo.o programs/user_echo/user_echo.elf \
	       programs/user_cat/user_cat.o programs/user_cat/user_cat.elf \
	       programs/user_long/user_long.o programs/user_long/user_long.elf \
	       programs/user_netecho/user_netecho.o programs/user_netecho/user_netecho.elf \
	       programs/user_gui_demo/user_gui_demo.o programs/user_gui_demo/user_gui_demo.elf \
	       programs/x2gui/x2gui.o programs/x2gui/x2gui.elf \
	       programs/user_gui_clock/user_gui_clock.o programs/user_gui_clock/user_gui_clock.elf \
	       programs/user_gui_widgets/user_gui_widgets.o programs/user_gui_widgets/user_gui_widgets.elf \
	       programs/user_gui_about/user_gui_about.o programs/user_gui_about/user_gui_about.elf \
	       programs/user_gui_term/user_gui_term.o programs/user_gui_term/user_gui_term.elf \
	       programs/user_gui_files/user_gui_files.o programs/user_gui_files/user_gui_files.elf \
	       programs/user_gui_viewer/user_gui_viewer.o programs/user_gui_viewer/user_gui_viewer.elf \
	       programs/user_login/user_login.o programs/user_login/user_login.elf \
	       programs/user_gui_settings/user_gui_settings.o programs/user_gui_settings/user_gui_settings.elf \
	       programs/user_gui_helloapp/user_gui_helloapp.o programs/user_gui_helloapp/user_gui_helloapp.elf \
	       programs/user_gui_taskmgr/user_gui_taskmgr.o programs/user_gui_taskmgr/user_gui_taskmgr.elf \
	       programs/user_gui_edit/user_gui_edit.o programs/user_gui_edit/user_gui_edit.elf \
	       programs/user_abi_test/user_abi_test.o programs/user_abi_test/user_abi_test.elf \
	       programs/c_hello/c_hello.o programs/c_hello/c_hello.elf \
	       programs/c_args/c_args.o programs/c_args/c_args.elf \
	       programs/c_filedemo/c_filedemo.o programs/c_filedemo/c_filedemo.elf \
	       programs/c_alloctest/c_alloctest.o programs/c_alloctest/c_alloctest.elf \
	       programs/c_envtest/c_envtest.o programs/c_envtest/c_envtest.elf \
	       programs/c_dynhello/c_dynhello.o programs/c_dynhello/c_dynhello.elf \
	       programs/devlist/devlist.o programs/devlist/devlist.elf \
	       programs/drvtest/drvtest.o programs/drvtest/drvtest.elf \
	       programs/usbtest/usbtest.o programs/usbtest/usbtest.elf \
	       programs/audiotest/audiotest.o programs/audiotest/audiotest.elf \
	       programs/audioplay/audioplay.o programs/audioplay/audioplay.elf \
	       programs/batterytest/batterytest.o programs/batterytest/batterytest.elf \
	       programs/displayinfo/displayinfo.o programs/displayinfo/displayinfo.elf \
	       programs/drawtest/drawtest.o programs/drawtest/drawtest.elf \
	       programs/rendertest/rendertest.o programs/rendertest/rendertest.elf \
	       programs/fonttest/fonttest.o programs/fonttest/fonttest.elf \
	       programs/logview/logview.o programs/logview/logview.elf \
	       programs/crashinfo/crashinfo.o programs/crashinfo/crashinfo.elf \
	       programs/wdogtest/wdogtest.o programs/wdogtest/wdogtest.elf \
	       programs/safesh/safesh.o programs/safesh/safesh.elf \
	       programs/shparity/shparity.o programs/shparity/shparity.elf \
	       programs/fscheck/fscheck.o programs/fscheck/fscheck.elf \
	       programs/services/services.o programs/services/services.elf \
	       programs/svc_crasher/svc_crasher.o programs/svc_crasher/svc_crasher.elf \
	       programs/stabilitytest/stabilitytest.o programs/stabilitytest/stabilitytest.elf \
	       programs/hwinfo/hwinfo.o programs/hwinfo/hwinfo.elf \
	       programs/drvmatch/drvmatch.o programs/drvmatch/drvmatch.elf \
	       programs/hwcompat/hwcompat.o programs/hwcompat/hwcompat.elf \
	       programs/hwreport/hwreport.o programs/hwreport/hwreport.elf \
	       programs/compattest/compattest.o programs/compattest/compattest.elf \
	       programs/strace/strace.o programs/strace/strace.elf \
	       programs/debugger/debugger.o programs/debugger/debugger.elf \
	       programs/pkgmgr/pkgmgr.o programs/pkgmgr/pkgmgr.elf \
	       programs/tobycc/tobycc.o programs/tobycc/m36_elf.o programs/tobycc/tobycc.elf \
	       programs/tobybuild/tobybuild.o programs/tobybuild/tobybuild.elf \
	       programs/selfhosttest/selfhosttest.o programs/selfhosttest/selfhosttest.elf \
	       programs/user_sh/user_sh.o programs/user_sh/user_sh.elf \
	       programs/user_pkg/user_pkg.o programs/user_pkg/user_pkg.elf \
	       programs/user_svcctl/user_svcctl.o programs/user_svcctl/user_svcctl.elf \
	       programs/user_ps/user_ps.o programs/user_ps/user_ps.elf \
	       programs/user_top/user_top.o programs/user_top/user_top.elf \
	       programs/user_cp/user_cp.o programs/user_cp/user_cp.elf \
	       programs/user_mv/user_mv.o programs/user_mv/user_mv.elf \
	       programs/user_rm/user_rm.o programs/user_rm/user_rm.elf \
	       programs/user_find/user_find.o programs/user_find/user_find.elf \
	       programs/user_sort/user_sort.o programs/user_sort/user_sort.elf \
	       programs/user_tee/user_tee.o programs/user_tee/user_tee.elf \
	       programs/user_env/user_env.o programs/user_env/user_env.elf \
	       programs/user_chmod/user_chmod.o programs/user_chmod/user_chmod.elf \
	       programs/user_df/user_df.o programs/user_df/user_df.elf \
	       programs/user_mkdir/user_mkdir.o programs/user_mkdir/user_mkdir.elf \
	       programs/user_touch/user_touch.o programs/user_touch/user_touch.elf \
	       programs/user_kill/user_kill.o programs/user_kill/user_kill.elf \
	       programs/user_tail/user_tail.o programs/user_tail/user_tail.elf \
	       programs/user_uniq/user_uniq.o programs/user_uniq/user_uniq.elf \
	       programs/user_date/user_date.o programs/user_date/user_date.elf \
	       programs/user_wget/user_wget.o programs/user_wget/user_wget.elf \
	       programs/user_nc/user_nc.o programs/user_nc/user_nc.elf \
	       programs/user_ifconfig/user_ifconfig.o programs/user_ifconfig/user_ifconfig.elf \
	       programs/user_nslookup/user_nslookup.o programs/user_nslookup/user_nslookup.elf \
	       programs/user_gui_calc/user_gui_calc.o programs/user_gui_calc/user_gui_calc.elf \
	       $(PORTS_SBASE_OBJS) $(PORTS_SBASE_ELFS) $(PORTS_SBASE_UTIL_O) \
	       libtoby/libtoby.a libtoby/crt0.o $(LIBTOBY_OBJS) \
	       libtoby/libtoby.so libtoby/Scrt0.o $(LDTOBY_OBJS) $(LDTOBY_SO) \
	       programs/common/start.o $(LOGDIR)/net.pcap
	@rm -f $(ALL_DEPS)
	@echo "(disk.img preserved -- 'make wipe-disk' to reformat the persistent FS)"

# ---------- Header dependency tracking ----------
#
# Every object is compiled with $(DEPFLAGS) (-MMD -MP, see the top of this
# file), which drops a `.d` next to it naming the headers it really included.
# Pull them all in so editing a header rebuilds exactly the objects that read
# it -- including `include/tobyos/abi/abi.h` and everything under libtoby/ and
# programs/, none of which had header prerequisites before.
#
# $(wildcard) is deliberate: it expands to only the .d files that EXIST, so a
# clean tree (or a brand-new object with no .d yet) is not an error. The
# per-header phony targets from -MP cover the delete-a-header case.
ALL_DEPS := $(wildcard \
    src/*.d \
    $(LIBTOBY_SRC)/*.d \
    programs/*/*.d \
    third_party/*/*.d \
    third_party/*/src/*.d \
    third_party/*/src/*/*.d)

-include $(ALL_DEPS)
