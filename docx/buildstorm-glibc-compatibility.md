# BuildStorm glibc Compatibility Record

## Scope

This record covers the RISC-V BuildStorm environment check only:

- `rustc --version` and `cargo --version`;
- `cargo new`, `cargo build`, and running the generated hello-world program;
- one guest CPU (`-smp 1`).

It does not claim support for the timed multi-core ArceOS build.

## Initial Symptom

The submit entrypoint runs cagent and then BuildStorm through
`run_submit()`. cagent completed, but every dynamically linked toolchain
program terminated with:

```
*** stack smashing detected ***: terminated
Aborted
BUILDSTORM_TOOLCHAIN fail
```

The same failure was reproduced with `/usr/bin/rm`, so it was not specific to
the Rust path or cargo cache.

## Implemented Changes

### Dynamic-program environment

- `user/riscv/user.c` now calls cagent before BuildStorm from `run_submit()`.
- BuildStorm is launched with the Debian RISC-V library directory first in
  `LD_LIBRARY_PATH`; it no longer copies the legacy `/glibc/lib` libraries
  into `/usr/lib`.
- `sys_execve` preserves the caller-provided environment rather than replacing
  `LD_LIBRARY_PATH` with a hard-coded value.
- The RISC-V ELF interpreter is resolved as
  `/usr/lib/riscv64-linux-gnu/ld-linux-riscv64-lp64d.so.1`.

### Process and virtual memory correctness

- `exec` clears all user general-purpose registers before entering a new image.
  Carrying the previous BusyBox register and TLS state across `execve` is not a
  valid RISC-V ABI transition.
- `growproc` now maps the complete requested `brk` expansion instead of only
  the first 64 KiB while advancing the logical size by the full amount.
- `mmap` eagerly creates and zeroes anonymous pages. `MAP_FIXED` now replaces
  an existing mapping through a precise VMA split/remove operation before
  installing replacement pages. `munmap` uses the same range operation and
  accepts unmapped holes as Linux does. This prevents a later unmap from
  freeing an adjacent loader, BSS, or TLS page because of stale VMA metadata.
- `mprotect` validates and replaces permissions, then flushes the RISC-V TLB.
- RISC-V leaf PTE creation now sets the accessed bit explicitly and sets the
  dirty bit for writable pages, rather than relying on the optional Svadu
  hardware extension to update A/D state.
- Added a conservative `riscv_hwprobe` implementation that reports no
  optional ISA extensions, so recent glibc can select its baseline rv64gc
  implementation without an unsupported-syscall path.
- The RISC-V `struct stat` ABI now has its required 128-byte tail. `fstat`
  also supplies valid metadata for terminal, pipe, and virtual-file handles
  rather than failing for standard streams.
- Removed the unaligned `0x10000036e` dynamic-linker workaround and its
  corresponding `uvmcopy` special case. It was not an ELF mapping and exposed
  a stale, uninitialised page at a page-rounded address.

## Investigation Result

The RISC-V ELF headers for the image's `rustup` were checked directly. Its two
`PT_LOAD` ranges and its `PT_TLS` image agree with the current page-rounded
load calculation. A bounded diagnostic boot also showed that `rustup` loads
`libgcc_s`, `libpthread`, `libm`, `libdl`, and `libc`, completes relocation
protection changes, and reaches the actual toolchain-selection path.

The latest observed result still has a stack-protector abort in dynamically
linked `/usr/bin/rm` and Rust executables after their dynamic libraries finish
mapping and `brk` grows. cagent passes in the same boot. Therefore the
20-point BuildStorm environment check is not yet passed. The remaining focus
is the glibc runtime state after relocation, especially high-address mapping
and TLS ownership across the loader's final transitions; it is not a shell
path or disk-image lookup failure. The same failure remains after the
`riscv_hwprobe` implementation, so that syscall is not the root cause.

## Reproduction

All builds run inside the provided `sos2026` Docker container:

```sh
docker exec sos2026 bash -lc 'make clean'
docker exec sos2026 bash -lc 'make all'
```

Run the RISC-V test with:

```sh
docker exec sos2026 bash -lc 'qemu-system-riscv64 -machine virt -kernel kernel-rv -m 1G -nographic -smp 1 -bios default -drive file=sdcard-rv.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 -no-reboot -device virtio-net-device,netdev=net -netdev user,id=net -rtc base=utc'
```

## AI Use

AI-assisted work was limited to source audit, targeted code edits, and
reproduction with the commands above. No guest clock, `/proc/uptime`, test
script, or disk-image result marker was modified.
