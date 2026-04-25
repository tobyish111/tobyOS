# tobyOS

A modern, from-scratch operating system focused on low-level control, performance, and a futuristic cyberpunk-inspired user experience.

---

## 🚀 Overview

**tobyOS** is a custom-built x86_64 operating system designed to explore systems programming from the ground up — from bootloader to kernel to eventual userland and UI.

The long-term vision is to create a **modern OS with a visually rich interface**, blending:

* The usability of Windows
* The polish of macOS
* A cyberpunk-inspired aesthetic

---

## ✨ Current Features

### Boot & Core

* Custom boot process using Limine
* 64-bit kernel (x86_64)

### CPU & Interrupts

* Global Descriptor Table (GDT)
* Interrupt Descriptor Table (IDT)
* Exception handling
* PIC (interrupt controller)
* PIT (hardware timer)

### Memory Management

* Physical Memory Manager (PMM)
* Kernel heap (`kmalloc` / `kfree`)

### Hardware Input

* PS/2 keyboard driver

### Graphics & Output

* Framebuffer rendering
* Custom console with cursor

### System Layer

* Basic shell environment

---

## 🛣️ Roadmap

### Kernel Evolution

* [ ] Virtual memory (paging)
* [ ] Process & thread scheduler
* [ ] User mode support
* [ ] System call interface

### Storage

* [ ] Disk drivers (AHCI / NVMe)
* [ ] FAT32 filesystem support
* [ ] Custom filesystem (`tobyfs`)

### Networking

* [ ] TCP/IP stack
* [ ] DHCP / DNS
* [ ] Basic HTTP support

### Userland

* [ ] ELF executable loader
* [ ] libc implementation
* [ ] Core utilities (shell tools, editor)

### Graphics & UI

* [ ] Windowing system
* [ ] Hardware acceleration (GPU support)
* [ ] UI framework (Slint / LVGL)
* [ ] Cyberpunk-themed desktop environment

---

## 🧰 Tech Stack

* **Languages:** C, Assembly
* **Bootloader:** Limine
* **Emulator:** QEMU
* **Architecture:** x86_64

---

## 🖥️ Build & Run

```bash
make
make run
```

---

## 🧪 Testing

* QEMU-based validation
* Incremental milestone testing

Planned:

* Unit testing for kernel subsystems
* Benchmarking (Phoronix Test Suite)
* Real hardware validation

---

## ⚠️ Disclaimer

This project is experimental and not production-ready.

---

## 👤 Author

Toby Buckmaster

---

## 🌌 Vision

tobyOS aims to bridge low-level systems engineering with modern UI/UX design — creating an OS that is both technically deep and visually distinct.
