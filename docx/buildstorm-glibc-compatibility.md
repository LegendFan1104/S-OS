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
  page contents and R/W/X permissions even when the address belongs to an
  existing reservation, as required by ld.so for BSS and TLS mappings.
- `mprotect` validates and replaces permissions, then flushes the RISC-V TLB.
- Removed the unaligned `0x10000036e` dynamic-linker workaround and its
  corresponding `uvmcopy` special case. It was not an ELF mapping and exposed
  a stale, uninitialised page at a page-rounded address.

## Investigation Result

The RISC-V ELF headers for the image's `rustup` were checked directly. Its two
`PT_LOAD` ranges and its `PT_TLS` image agree with the current page-rounded
load calculation. A bounded diagnostic boot also showed that `rustup` loads
`libgcc_s`, `libpthread`, `libm`, `libdl`, and `libc`, completes relocation
protection changes, and reaches the actual toolchain-selection path.

The latest observed result still has a stack-protector abort in a dynamically
linked Rust executable, so the 20-point BuildStorm environment check is not
yet passed. The remaining investigation should focus on the dynamic loader's
TLS/runtime state after relocation, not on shell paths or the disk image.

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
