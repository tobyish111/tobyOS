# tobyOS

**tobyOS** is a from-scratch, hobby operating system focused on learning, experimentation, and building a modern desktop OS from the ground up.

The project explores the full stack of operating system development—from low-level kernel architecture to a graphical desktop environment—with a long-term goal of creating a usable, extensible, and self-hosting OS.

---

## 🚀 Current Status

tobyOS has progressed significantly and now includes:

### Core OS

* Custom kernel with:

  * memory management
  * interrupts and scheduling
  * process model and syscalls
* Virtual filesystem (VFS) with persistent storage
* User accounts and permissions
* Basic security model with sandboxing

### Hardware & System Support

* Driver model with device discovery
* Storage support (partitions, filesystems)
* Networking stack (DHCP, DNS, TCP, HTTP)
* Expanded hardware compatibility (QEMU-focused)

### Desktop Environment

* Window manager and GUI system
* Taskbar, launcher, and desktop shell
* Notification system
* Cyberpunk-inspired UI direction (in progress)

### Application Ecosystem

* Built-in apps:

  * file manager
  * terminal
  * text editor
  * settings app
* Package manager (CLI + GUI “App Store”)
* App manifest system
* Install/remove apps dynamically

### Developer Platform

* SDK for building apps
* In-OS build workflow
* Minimal compiler/toolchain integration
* Partial self-hosting:

  * build and run programs inside tobyOS

### System Quality

* Logging and diagnostics framework
* Crash handling and panic reporting
* Safe-mode boot options
* Service supervision and restart
* Hardware compatibility reporting

---

## 🎯 Project Goals

* Learn and explore OS development end-to-end
* Build a modern desktop OS with a clean architecture
* Support a growing ecosystem of applications
* Achieve partial → full self-hosting over time
* Create a visually modern UI inspired by:

  * Windows 10 usability
  * Cyberpunk/technical aesthetic

---

## 🧠 Design Philosophy

* **From scratch**: no reliance on existing OS kernels
* **Incremental milestones**: each system built step-by-step
* **Clarity over complexity**: simple, understandable implementations
* **Modularity**: clean separation between subsystems
* **Stability-first**: features must not break the system

---

## 🛠️ Development Approach

tobyOS is built using structured milestones, including:

* Kernel & core systems
* Filesystems & storage
* Networking
* GUI & desktop environment
* Package management
* Security & sandboxing
* Self-hosting toolchain
* Hardware compatibility

Development and testing are primarily done using QEMU, with plans for real hardware validation.

---

## 🧪 Running tobyOS

tobyOS currently runs in a virtual machine environment (QEMU).

Example (may vary based on build setup):

```bash
qemu-system-x86_64 -m 2048 -cdrom tobyOS.iso -boot d
```

---

## ⚠️ Disclaimer

tobyOS is an experimental hobby project:

* Not production-ready
* Limited hardware support
* Features are still evolving
* Expect bugs and incomplete functionality

---

## 🔮 Roadmap (High-Level)

* Expand hardware compatibility
* Improve desktop polish and UX
* Increase POSIX compatibility
* Advance toward full self-hosting
* Real hardware testing and release candidate

---

## 🤝 Contributing

This project is currently experimental and evolving quickly, but contributions, ideas, and feedback are welcome.

---

## 📌 Summary

tobyOS is a deep dive into operating system engineering—from bootloader to desktop apps—with the ambition of becoming a fully self-sustaining, modern OS.

---

**Status:** Actively in development 🚧
