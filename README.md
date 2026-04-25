# tobyOS

A modern, from-scratch operating system built with performance, control, and a cyberpunk-inspired user experience in mind.

---

## 🚀 Overview

**tobyOS** is a hobbyist operating system developed from the ground up to explore low-level systems design, hardware interaction, and OS architecture.

The long-term vision is to create a **modern, visually rich OS** combining:

* The usability of Windows 10
* The polish of macOS
* A **cyberpunk-inspired UI aesthetic**

---

## ✨ Features (Current)

* ✅ Custom boot process using Limine
* ✅ 64-bit kernel (x86_64)
* ✅ Framebuffer graphics output
* ✅ Basic terminal / console
* ✅ Memory management:

  * Physical Memory Manager (PMM)
  * Kernel heap (`kmalloc` / `kfree`)
* ✅ Interrupt handling:

  * GDT (Global Descriptor Table)
  * IDT (Interrupt Descriptor Table)
  * Exception handling
* ✅ Hardware timers (PIT)
* ✅ Keyboard input (PS/2)
* ✅ Basic shell

---

## 🛣️ Roadmap

Planned milestones include:

### Core Systems

* [ ] Virtual Memory (Paging)
* [ ] Process & Thread Scheduler
* [ ] User Mode Support
* [ ] System Call Interface

### Storage

* [ ] GPT partition support
* [ ] FAT32 read/write
* [ ] Custom filesystem (`tobyfs`)

### Networking

* [ ] TCP/IP stack
* [ ] DHCP & DNS
* [ ] Basic HTTP client

### Userland

* [ ] Dynamic linker
* [ ] libc implementation
* [ ] Ported utilities (shell, text editor)

### UI / Graphics

* [ ] Windowing system
* [ ] Hardware acceleration (virtio-gpu / GPU drivers)
* [ ] Modern UI framework (Slint / LVGL)
* [ ] Cyberpunk-themed desktop environment

---

## 🧰 Tech Stack

* **Languages:** C (primary), Assembly (boot/low-level)
* **Bootloader:** Limine
* **Emulation:** QEMU
* **Build Tools:** GCC / Clang, Make
* **Target Architecture:** x86_64

---

## 🖥️ Building & Running

### Requirements

* GCC or Clang (cross-compiler recommended)
* QEMU
* Make
* NASM

### Build

```bash
make
```

### Run in QEMU

```bash
make run
```

---

## 🧪 Testing

Current testing is done via:

* QEMU-based boot validation
* Incremental milestone verification

Planned:

* Unit tests for kernel subsystems
* Integration tests for drivers
* Benchmarking with tools like Phoronix Test Suite
* Real hardware validation across multiple platforms

---

## 📸 Screenshots

*(Coming soon)*

---

## 🤝 Contributing

This project is currently in active development and primarily maintained by a single developer.

If you’re interested in contributing:

* Open an issue to discuss ideas or bugs
* Fork the repo and submit a PR

---

## ⚠️ Disclaimer

tobyOS is an experimental project and is **not production-ready**. Expect bugs, crashes, and incomplete features.

---

## 📜 License

*(Add your license here — MIT, GPL, etc.)*

---

## 👤 Author

Developed by Toby Buckmaster

---

## 🌌 Vision

tobyOS aims to push beyond traditional hobby OS projects by combining:

* Low-level systems engineering
* Modern UI/UX design
* A cohesive aesthetic inspired by futuristic computing environments

This is not just an OS — it’s a platform for experimentation, performance, and design.

---
