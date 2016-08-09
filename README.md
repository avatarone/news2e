[S²E: A Platform for In-Vivo Multi-Path Software Analysis](https://s2e.epfl.ch)
===============================================================================

__WARNING__: This repository contains work-in-progress code for a version of
S2E with a current Qemu version.  The code might compile and link, but is for
sure not functional. Lots of stuff is still missing, other stuff is stubbed.
Use only as a study target.





S2E is a platform for writing tools that analyze the properties and behavior of
software systems. So far, S2E has been used to develop a comprehensive
performance profiler, a reverse engineering tool for proprietary software, and
a bug finding tool for both kernel-mode and user-mode binaries. Building these
tools on top of S2E took less than 770 LOC and 40 person-hours each.

S2E’s novelty consists of its ability to scale to large real systems, such as a
full Windows stack. S2E is based on two new ideas:

1. Selective symbolic execution, a way to automatically
   minimize the amount of code that has to be executed
   symbolically given a target analysis; and

2. Relaxed execution consistency models, a way to make
   principled performance/accuracy trade-offs in complex
   analyses.

These techniques give S2E three key abilities:

1. to simultaneously analyze entire families of execution
   paths, instead of just one execution at a time;
    
2. to perform the analyses in-vivo within a real software
   stack—user programs, libraries, kernel, drivers, etc.—
   instead of using abstract models of these layers; and
   to operate directly on binaries, thus being able to analyze
   even proprietary software.

Conceptually, S2E is an automated path explorer with modular path analyzers:
the explorer drives the target system down all execution paths of interest,
while analyzers check properties of each such path (e.g., to look for bugs) or
simply collect information (e.g., count page faults). Desired paths can be
specified in multiple ways, and S2E users can either combine existing
analyzers to build a custom analysis tool, or write new analyzers using the
S2E API.

S2E helps make analyses based on symbolic execution practical for large
software that runs in real environments, without requiring explicit modeling of
these environments.

S2E is built upon the [KLEE symbolic execution engine](http://klee.llvm.org)
and the [QEMU virtual machine emulator](http://qemu.org).

Documentation 
-------------

Setup instruction and user documentation can be found in the /docs folder, both
in RST and HTML format.

Building and running S2E
-----------

There are three different build modes for S2E available:
- release: An optimized build. Debug information will be stripped.
- debug: A debug build. Compiler optimizations are disabled (-O0, -O1 is used)
         and debug information is preserved.
- asan: Like the debug build, but also enables clang's address sanitizer. This
       is useful to debug memory issues without such heavy tooling as valgrind.

You can build a specific mode by, e.g., issuing 

    make -f $(S2E_SRC_DIR)/Makefile stamps/qemu-debug-make
    
to build the debug configuration.
Binaries will be produced in the
$(S2E_BIN_DIR)/qemu-<mode>/<arch>-s2e-softmmu/qemu-system-arm, where <mode> is
the build mode and <arch> is the guest architecture S2E is built for (i386,
x86_64, arm, armeb). Scripts expect the environment variable S2E_DIR to be set
to the build directory of your current build mode, so do, e.g.,
 
    export S2E_DIR=$(S2E_BIN_DIR)/qemu-asan 
    
to choose the ASAN build mode before starting S2E.
The ASAN build mode will require some more work for starting, as libraries need
to be built as shared libraries. To tell the dynamic linker where it can find
these libraries, do 

    export LD_LIBRARY_PATH=$(S2E_BIN_DIR)/stp-asan/lib:$(S2E_BIN_DIR)/minisat-asan
    
before starting S2E.



