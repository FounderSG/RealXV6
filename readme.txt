     ==========================================================================
                                    RealXV6
     ==========================================================================
        A Port of the UNIX Version 6 (V6) Kernel to the 8086 PC in Real Mode.
       
       
     Overview
     --------

     This project is a port of the classic UNIX Version 6 (V6) kernel to the
     Intel 8086/8088 architecture, targeting the original IBM PC and its
     compatibles. The kernel code is based on the version famously analyzed in
     the Lions' Commentary on UNIX 6th Edition.

     The userspace is a hybrid, containing programs from the original V6
     distribution as well as some from the xv6 project.


     Philosophy: A Port, Not a Rewrite
     ---------------------------------

     Unlike projects like xv6 which are modern rewrites inspired by V6, RealXV6
     aims to be a faithful port. The goal is to preserve the original
     structure, algorithms, and spirit of the V6 kernel as much as possible.

     This makes it an excellent resource for studying the original design on a
     more accessible platform for modern study, typically through emulation.

     Key aspects of the original design are intentionally retained:

       - Original Scheduler Logic: The logic at the famous
         `/* You are not expected to understand this */`
         comment in the scheduler retains its original meaning. As a direct
         result, the original V6 process swapping mechanism is also fully
         preserved.

       - Process Switching: The original PDP-11 `savu`/`retu` assembly
         primitives relied on the C compiler saving the environment on every
         function call. In this port, the `savu`/`retu` routines are still
         used, but their role is now limited to managing the switch of the user
         area (`u` area). Since the 8086 architecture lacks an MMU, this
         switch is handled by `memcpy` within these routines. The actual
         process context switch is handled by the more portable `save`/`resume`
         functions from UNIX V7.


     Architecture
     ------------


     The original V6 was designed for the 16-bit PDP-11. By targeting the
     native 16-bit architecture of the 8086, this port minimizes the
     architectural changes required, keeping the code as close to the original
     as possible.

     While understanding the PDP-11 architecture is key to studying the
     original V6 source, the 8086 architecture is more widely understood.
     Furthermore, the widespread availability of mature 8086 PC emulators makes
     this platform highly accessible for development and study. For developers,
     knowledge of how a typical PC bootloader operates is sufficient to
     understand the low-level aspects of RealXV6.

     The project uses the simple `.COM` executable format. This choice
     simplifies development as `.COM` files are memory-relocatable by design,
     which makes it easier to manage processes in memory and to implement
     swapping them to disk.


     Building the System 
     -------------------

     The project is built using the Open Watcom C compiler and toolchain.
     Open Watcom is a powerful cross-platform suite that can run on modern
     operating systems like Windows, Linux, and macOS, while still targeting
     the 8086's 16-bit architecture.

     To build the kernel, please refer to the included Makefile.


     Code Structure
     --------------

     The source code is organized into several key directories:

       - /ken: Named for Ken Thompson, this directory contains the main,
         machine-independent kernel code (e.g., system calls, process
         management, file system).

       - /dmr: Named for Dennis M. Ritchie, this directory contains machine-
         dependent drivers and low-level code for the PC architecture (e.g.,
         keyboard, IDE, TTY).

       - /h: Contains all kernel header files, defining the core data
         structures and constants.
