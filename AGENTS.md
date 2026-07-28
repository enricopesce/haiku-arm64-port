# AI-assisted Haiku ARM64 port

This repository is an experimental Haiku ARM64/QEMU port maintained through
**AI-first development**. The active, extensive use of AI is a project
requirement, not merely an allowed convenience. Use AI throughout the entire
project lifecycle: architecture and code analysis, implementation, debugging,
test design, build recovery, performance work, documentation, review,
refactoring, release preparation, and ongoing maintenance.

Agents must work proactively. Do not treat the use of AI as a reason to avoid
modifying source code, investigating a failure, or proposing an improvement:
make well-scoped changes, verify them, report the evidence, and continue with
the next safe step. Human direction sets goals and boundaries; AI should
accelerate the work inside those boundaries rather than waiting for routine
approval.

## Project goals

- Make Haiku ARM64 boot and run reliably on QEMU `virt` with native HVF on
  Apple Silicon.
- Stabilize ARM64 memory management, SMP, debugger support, and the VirtIO
  PCI/block/GPU stack.
- Produce reproducible ARM64 images and a shareable package repository.
- Keep changes reviewable and suitable for eventual upstream discussion.

## Repository layout

- This repository: kernel, drivers, userland, image definitions, and QEMU
  smoke tests.
- `https://github.com/enricopesce/haiku-arm64-port`: public fork for the Haiku
  changes. The full tested branch is `arm64-qemu-port-full`.
- `https://github.com/enricopesce/haikuports-arm64`: public fork for HaikuPorts
  recipes and bootstrap fixes. Its working branch is `arm64-bootstrap-macos`.

Keep Haiku source changes and HaikuPorts recipe changes in their respective
repositories. Do not mix package recipes into Haiku commits.

## Build environment

The macOS checkout at the repository root may be case-insensitive. Do not build
Haiku there. Use the case-sensitive checkout:

```sh
cd /Volumes/HaikuCase/haiku
```

Important paths:

```text
Haiku source:       /Volumes/HaikuCase/haiku
HaikuPorts source:  /Volumes/HaikuCase/haikuports
Jam:                /Volumes/HaikuCase/bin/jam
ARM64 build:        /Volumes/HaikuCase/haiku/generated.arm64
Bootstrap build:    /Volumes/HaikuCase/haiku/generated.arm64-bootstrap
```

Always pass `-sHAIKU_REVISION=hrev0` to Jam in this environment.

## Fast ARM64/QEMU loop

Use all host CPUs for normal Haiku compilation:

```sh
cd /Volumes/HaikuCase/haiku/generated.arm64
PATH=/Volumes/HaikuCase/bin:$PATH \
  jam -q -j8 -sHAIKU_REVISION=hrev0 -sHAIKU_IMAGE_SIZE=1024 @minimum-mmc
```

Run the resulting image natively on Apple Silicon:

```sh
/opt/homebrew/bin/qemu-system-aarch64 \
  -machine virt,accel=hvf,acpi=off -cpu host -smp 4 -m 2048 \
  -bios /opt/homebrew/share/qemu/edk2-aarch64-code.fd \
  -drive if=none,file=/Volumes/HaikuCase/haiku/generated.arm64/haiku-mmc.image,format=raw,id=drive0 \
  -device virtio-blk-pci,drive=drive0,disable-legacy=on,vectors=0 \
  -device virtio-gpu-pci,disable-legacy=on,vectors=0,xres=1920,yres=1080 \
  -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 \
  -device usb-tablet,bus=xhci.0 -display cocoa
```

There is no x86 emulation in this configuration. Do not add artificial display
refresh-rate options: VirtIO scanouts expose a nominal 60 Hz mode per
resolution.

Use `src/tests/arm64-qemu-smoke-test` for non-interactive boot/SMP/VirtIO
validation. It must run with the regular VM stopped because it uses QEMU
snapshot mode.

## Bootstrap and packages

The bootstrap is intentionally separate from the normal QEMU image. It creates
the ARM64 development/bootstrap environment and source packages; it does not
need to be rebuilt for each kernel edit.

### Image roles

- `@minimum-mmc` is the supported interactive ARM64/QEMU desktop image. Use it
  for GUI, network, VirtIO and performance validation, and for published
  bootable artifacts.
- `@bootstrap-mmc` is a native package-build environment only. HaikuPorter's
  bootstrap dependencies are activated in `/boot/system/packages` during first
  boot, so it must not be presented as equivalent to the minimal desktop.
- Keep optional development HPKGs in `/boot/home/config/packages` when testing
  them on a desktop image. Do not fold the HaikuPortsCross repository into the
  system package set of a user-facing image.

Before publishing, boot the exact MMC file with the regular QEMU command and
verify serial output contains no `indirect_desc` error and publishes
`net/virtio/0`. The known absence of the ARM64 media-server package is not a
VirtIO or packagefs boot failure.

```sh
cd /Volumes/HaikuCase/haiku/generated.arm64-bootstrap
PATH=/Volumes/HaikuCase/bin:/opt/homebrew/opt/ncurses/bin:/opt/homebrew/opt/m4/bin:/opt/homebrew/bin:$PATH \
  jam -q -sHAIKU_REVISION=hrev0 -sHAIKU_IMAGE_SIZE=4096 \
  -sHAIKU_PORTER_CONCURRENT_JOBS=8 @bootstrap-mmc
```

Do not add `-j8` to this Jam command: HaikuPorter metadata is shared. The
per-package parallelism is already set to eight jobs.

When a recipe fails on macOS, identify the exact wrapper-script failure before
editing. Prefer portable recipe changes, such as selecting `gsed` when the
recipe depends on GNU `sed`. Validate source URLs and checksums before adding a
mirror. Commit each recipe fix separately in the HaikuPorts fork.

## Engineering priorities

1. Correctness before performance; use explicit memory barriers for VirtIO
   rings and MMIO notifications on ARM64.
2. Keep the VirtIO block boot path stable; it is the normal QEMU boot disk.
3. Keep VirtIO GPU mode changes deterministic and compatible with Screen.
4. Preserve serial logging and ARM64 debugger state: crashes must remain
   diagnosable.
5. Treat optional unavailable ARM64 packages as information, not a failure,
   unless they are required by the selected image target.

## Git and sharing

- Work in focused commits with imperative messages.
- Push tested Haiku changes to `arm64-qemu-port-full` (or a clearly named
  follow-up branch) and recipe changes to `arm64-bootstrap-macos`.
- Do not commit host-specific paths, local disk images, ISO files, logs, or
  credentials.
- Publish bootable images as GitHub Release assets with SHA-256 checksums, not
  as Git objects.
- Before publishing an image, stop QEMU so the disk image is consistent.

## Agent workflow

AI is expected to be the default engineering collaborator for every task,
including tasks not explicitly listed in this file. Prefer evidence from source,
build output, tests, and runtime logs over guesses. Use AI to turn that evidence
into a concrete diagnosis, a minimal implementation, and a reproducible
verification.

1. Inspect the relevant diff, build status, and available disk space.
2. Make the smallest change that addresses the observed failure.
3. Build and test proportionally: compile, create/update the image, then boot
   or run the smoke test when the change affects boot/runtime behavior.
4. If a build fails, continue from the checkpoint, diagnose the first real
   error, fix it, and relaunch. Do not wait for confirmation between routine
   recovery steps.
5. Keep the user informed with concise progress updates and surface only real
   blockers or decisions that require their input.
