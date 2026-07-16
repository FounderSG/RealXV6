# RealXV6

A faithful port of the UNIX Version 6 (V6) kernel to the 8086 PC in real mode.

## Overview

RealXV6 ports the classic UNIX V6 kernel — the version analyzed in the *Lions' Commentary on UNIX 6th Edition* — to the **Intel 8086/8088**, targeting the original IBM PC and its emulators. The userspace is a hybrid of programs from the original V6 distribution and a few from the `xv6` project.

## Philosophy: A Port, Not a Rewrite

Unlike `xv6`, which is a modern rewrite *inspired by* V6, RealXV6 aims to be a faithful **port**: it preserves the original structure, algorithms, and spirit of the V6 kernel as closely as possible, making it a resource for studying the original design on an accessible, widely emulated platform.

Two examples of retained original design:

*   **Scheduler & swapping.** The logic at the famous `/* You are not expected to understand this */` comment keeps its original meaning, and with it the original V6 process-swapping mechanism.
*   **Process switching.** The PDP-11 `savu`/`retu` primitives are still used, but — since the 8086 has no MMU — they now only switch the user area (`u` area), implemented as a `memcpy`. The actual register/stack context switch is done by the more portable `save`/`resume` from UNIX V7.

## Architecture

V6 was written for the 16-bit PDP-11, so targeting the natively 16-bit 8086 minimizes the changes needed and keeps the code close to the original. While understanding the PDP-11 architecture is key to studying the original V6 source, the 8086 architecture is more widely understood, and the availability of mature 8086 PC emulators makes the platform highly accessible for development and study.

Executables use the simple `.COM` format (single segment, position-independent). Because `.COM` images are relocatable by design, processes are easy to move in memory and to swap to disk — which is how V6-style swapping works here without an MMU.

## The `vmm-experiment` Branch

`main` is the pure real-mode port described above. The **`vmm-experiment`** branch explores giving the MMU-less kernel real hardware memory management, *without* modernizing the kernel itself.

It adds a small V86 monitor under `vmm/`: the boot sector loads it first, it switches the CPU into 32-bit protected mode with paging and VME (Virtual-8086 Mode Extensions), and then runs the entire `unix.com` V6 kernel as a Virtual-8086 guest. The monitor gives the kernel a **paravirtualized MMU** — per-process address isolation and hardware memory protection — while the guest still sees a plain 8086.

The kernel talks to the monitor through hypercalls on `int 80h` (kept separate from the `int 81h` syscall path). This makes possible things the bare port cannot do, such as per-process `u`-area switching by remapping a single page (instead of `memcpy`), shared read-only program text, sparse/demand-paged memory, and surfacing wild-pointer faults to the process as `SIGSEG` rather than corrupting the kernel.

This branch requires a 386-class CPU (or emulator); it is experimental and a work in progress.

## Building

The project is built with the **Open Watcom** C compiler and toolchain (`wcc`, `wasm`, `wlink`, `wmake`), which runs on modern Windows, Linux, and macOS while targeting 16-bit 8086 code. Build the kernel with the top-level `Makefile`; the userspace (`usr/`) and the boot image (`boot/`) have their own Makefiles.

## Code Structure

*   `/ken` — machine-independent kernel (named for Ken Thompson): system calls, process management, file system, scheduler.
*   `/dmr` — machine-dependent code (named for Dennis M. Ritchie): low-level entry/context switch and PC drivers (IDE, keyboard, TTY, serial).
*   `/h` — kernel headers defining the core data structures and constants.
*   `/usr` — userspace programs and the small libc they link against.
*   `/boot` — boot sector, filesystem prototype, and emulator/image build.
*   `/tools` — host-side utilities, including the filesystem-image builder (`mkfs`).
*   `/vmm` — the V86 monitor (**`vmm-experiment` branch only**).
