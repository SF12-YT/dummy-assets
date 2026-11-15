# Exynos 9820 JSQZ driver — boot-image analysis

**Target:** Samsung Galaxy S10e SM-G970F, Exynos 9820  
**Kernel:** `4.14.113-Cruel Kernel M SebastianSNRL v1.0 N` (15 Sep 2023 build)  
**Boot payload SHA-256:** `838e2ac15c5270201eeafe67f6a603df25048783301f21e2617e5be68d142d5b`  
**Boot image:** Android boot header v1; page size 2048; kernel payload 44,849,160 bytes; ramdisk size 0  
**Driver:** built-in `CONFIG_VIDEO_EXYNOS_JPEG_SQZ=y`; device-tree compatible `samsung,exynos-jsqz`.

## Executive finding

JSQZ is a **JPEG quantisation-table generator driven by raw YUV image data**. The driver maps the supplied YUV buffer for device DMA, starts the JSQZ IP, and retrieves two 64-byte quantisation matrices (luma and chroma). It returns those 128 bytes to userspace as 32 little-endian 32-bit words.

It does **not** receive a JPEG bitstream and does **not** return an encoded JPEG bitstream. JPEG encoding is a later stage which can use the resulting tables.

This conclusion follows from the driver's DMA direction and buffer-size calculations, the addresses programmed into the Y/U/V input registers, and the fact that the only output read by the driver is the pair of 16-word quantisation-register banks.

## What was recovered from the actual boot image

The kernel image was extracted from the uploaded boot partition and converted to an ELF with recovered symbols. The JSQZ driver symbols are present at these kernel virtual addresses:

| Function | Kernel address |
|---|---:|
| `jsqz_suspend` | `ffffff8008b08830` |
| `jsqz_sysmmu_fault_handler` | `ffffff8008b08860` |
| `jsqz_resume` | `ffffff8008b08a20` |
| `jsqz_runtime_suspend` | `ffffff8008b08a50` |
| `jsqz_open` | `ffffff8008b08b00` |
| `jsqz_probe` | `ffffff8008b08bc0` |
| `jsqz_irq_handler` | `ffffff8008b08fb0` |
| `jsqz_runtime_resume` | `ffffff8008b09130` |
| `jsqz_remove` | `ffffff8008b09190` |
| `jsqz_release` | `ffffff8008b09250` |
| `jsqz_process` | `ffffff8008b092d0` |
| `jsqz_compat_ioctl32` | `ffffff8008b09a80` |
| `jsqz_ioctl` | `ffffff8008b09d40` |
| `jsqz_task_schedule` | `ffffff8008b09f80` |
| `jsqz_task_schedule_work` | `ffffff8008b0a5d0` |
| `jsqz_map_dma_attachment` | `ffffff8008b0a5f0` |
| `jsqz_unmap_dma_attachment` | `ffffff8008b0a660` |
| `jsqz_dma_addr_map` | `ffffff8008b0a690` |
| `jsqz_dma_addr_unmap` | `ffffff8008b0a760` |
| `jsqz_driver_init` | `ffffff8009a9aa00` |

The actual machine code confirms that `jsqz_run_sqz` is folded into the task-scheduling path in this build; the string exists, but it is not a separate recovered symbol.

Full disassembly: [`jsqz-driver-disassembly.txt`](jsqz-driver-disassembly.txt)  
Recovered symbol list: [`jsqz-symbols.txt`](jsqz-symbols.txt)  
Kernel string offsets: [`jsqz-string-offsets.txt`](jsqz-string-offsets.txt)

## Userspace ABI — the 32-bit/64-bit distinction is resolved

The Samsung HAL library `libhwjsqz.so` is 32-bit ARM. It submits ioctl request:

```
0xC1344D00
```

This is **the 32-bit compat ioctl**, not a malformed native ioctl. The actual kernel's `jsqz_compat_ioctl32` compares against `0xC1344D00`, copies a `0x134`-byte (308-byte) compat task from userspace, translates its fields into the native task structure, calls `jsqz_process`, and copies the 32 returned table words back.

The 64-bit native handler is separate:

| Handler | Request | Structure size |
|---|---:|---:|
| `jsqz_compat_ioctl32` | `0xC1344D00` | 308 bytes (`0x134`) |
| `jsqz_ioctl` | `0xC1504D00` | 336 bytes (`0x150`) |

The 32-bit task has a 16-byte image-info block, two 12-byte buffer descriptors, an 8-byte config block, two 128-byte arrays and a 4-byte buffer count. The 64-bit descriptors are larger because the pointer/length fields are 64-bit.

**Important:** this is direct confirmation from your kernel's recovered `jsqz_compat_ioctl32` and `jsqz_ioctl` disassembly, not merely an inference from the userspace library.

## End-to-end hardware path

```text
HAL opens /dev/jsqz
    |
    v
jsqz_open
    creates a per-file context and mutex
    |
    v
ioctl 0xC1344D00
    jsqz_compat_ioctl32
    copies 308-byte task from userspace
    |
    v
jsqz_process
    checks image alignment
    prepares and validates the YUV image buffer
    attaches/maps DMA buffer through Exynos IOVMM + System MMU
    |
    v
ordered workqueue -> jsqz_task_schedule
    runtime-PM resume / clock enable
    set current task
    software reset
    write Y/U/V IOVA addresses and image-format word
    enable interrupt, clear pending status, start JSQZ IP
    |
    v
hardware processes the image and raises IRQ
    jsqz_irq_handler
    clears interrupt and signals task completion
    |
    v
jsqz_task_schedule
    checks timeout flag
    reads 16 luma + 16 chroma quantisation registers
    or uses the default table on timeout
    runtime-PM autosuspend / clock disable
    |
    v
jsqz_process
    unmaps DMA resources and releases task context
    |
    v
jsqz_compat_ioctl32
    copies 32 result words back to userspace
    |
    v
HAL unpacks 128 bytes in getResultQT()
```

The workqueue is ordered, so driver tasks are serialized. The device is runtime-PM managed; the source sets a 1000 ms autosuspend delay.

### DMA and input layout

The API accepts NV21 or YUV422 image information, dimensions, optional stride, and one or two input buffers. The UAPI declares both DMA-BUF file descriptors and userspace pointers, but there is an important implementation limitation in this driver lineage: the helper that tries to resolve a USERPTR to a DMA-BUF has its VMA lookup code compiled out (`#if 0`) and returns `NULL`. The actual target kernel disassembly follows the USERPTR descriptor path into the `-EINVAL` error branch, while the DMA-BUF descriptor path calls `dma_buf_get`, attaches the buffer, and maps it. **For this target, use `HWSQZ_BUFFER_DMABUF` (type 1); do not use USERPTR (type 2).** The DMA-BUF is attached and mapped into the JSQZ device address space through Exynos IOVMM/System MMU.

The hardware reads the input (`DMA_TO_DEVICE` from the CPU driver's perspective). There is no output pixel buffer prepared by this operation. The result returned to userspace is the quantisation data.

## Register map

The device-tree resource in the target device is at physical base **`0x18B30000`**. The driver maps that region and uses the following offsets:

| Offset | Register | Purpose |
|---:|---|---|
| `0x000` | Y address | Luma input DMA address |
| `0x004` | U address | Chroma input DMA address |
| `0x008` | V address | Chroma input DMA address |
| `0x00C` | Input format | Width, height, colour space, mode and function |
| `0x010–0x04C` | Y Q matrix | 16 × 32-bit luma result words |
| `0x050–0x08C` | C Q matrix | 16 × 32-bit chroma result words |
| `0x090` | Software reset | Write 0, then 1 |
| `0x094` | Interrupt enable | Write 1 to enable |
| `0x098` | Interrupt clear/status | Read status; write 1 to clear |
| `0x09C` | Hardware start | Write 1 to start |
| `0x100` | Hardware done | Bit 0 is the done indication |
| `0x108` | Bus config | Stride control and timeout/configuration flags |
| `0x10C–0x148` | Initial Y Q table | 16 × 32-bit initial-table registers |
| `0x14C–0x188` | Initial C Q table | 16 × 32-bit initial-table registers |

The main scheduling path in the actual kernel disassembly confirms the reset writes at base+`0x90`, format/address programming, interrupt enable at base+`0x94`, interrupt clear at base+`0x98`, and hardware start at base+`0x9C`. After completion it reads the two output banks at base+`0x10` and base+`0x50`.

### Packed image-format word

The reference driver builds the format word as:

```c
format = (width & 0x1fff)
       | ((height & 0xfff) << 16)
       | ((cs & 1) << 30)
       | ((mode & 1) << 29)
       | ((function & 1) << 28);
```

Thus width occupies bits 0–12, height bits 16–27, colour-space selection bit 30, processing mode bit 29 and function bit 28.

The bus-config register also carries the stride enable/value (bit 16 and low 13 bits) and a timeout-related flag (bit 24). The normal launch path clears the timeout flag before starting.

## Alignment fallback — a successful ioctl does not prove hardware ran

Before setting up DMA, `jsqz_process` checks the image's stride (or width when stride is zero).

- NV21: accepted when the value is divisible by 64, or divisible by 32 with quotient modulo 4 equal to 1.
- YUV422: accepted when divisible by 128, or divisible by 64 with quotient modulo 4 equal to 1.

If this check fails, the driver copies a built-in 32-word default quantisation table to the result and returns success immediately. It does **not** map DMA or start the IP. Therefore an ioctl can return success without hardware processing.

There is a second fallback after hardware execution: if the bus-config timeout flag is set, the scheduler copies the default table instead of reading the hardware output registers.

The built-in default table in the reference driver begins:

```c
0x01010101, 0x01010101, 0x01010101, 0x03020201,
0x02020202, 0x03030402, 0x04050302, 0x04050505,
0x06050404, 0x05050607, 0x04040607, 0x07060906,
0x08080808, 0x09060508, 0x0a08090a, 0x08080807,
0x02010101, 0x02040202, 0x05080402, 0x08080504,
0x08080808, 0x08080808, 0x08080808, 0x08080808,
0x08080808, 0x08080808, 0x08080808, 0x08080808,
0x08080808, 0x08080808, 0x08080808, 0x08080808
```

Each word is unpacked little-endian by the HAL into four bytes. The first 16 words supply 64 luma bytes; the next 16 supply 64 chroma bytes.

## IRQ completion details

The recovered `jsqz_irq_handler` at `ffffff8008b08fb0` reads the interrupt status register at base+`0x98`, clears the device-running state, writes 1 to base+`0x98`, obtains the current task and marks it complete. The reference source has the check that would reject a zero/invalid interrupt status commented out; the handler therefore does not use the read status value to decide whether to return `IRQ_NONE`.

The scheduler waits on the task completion object, then checks bit 28 of bus config. If that timeout flag is set, it substitutes the static table. Otherwise it reads the two 16-word Q-matrix banks.

## An API field that appears unused by the active path

The UAPI structure contains `buf_init_q[32]`, and the register header defines initial-Q-table registers at `0x10C–0x188`. However, the reference core contains no call to its `jsqz_set_input_qtbl` or `jsqz_get_init_qtbl` helper, and the active task scheduling sequence writes no initial-Q-table values before starting the IP. The active path appears to use the hardware's internal defaults plus the image-derived output tables; a userspace-provided initial table is not demonstrated as consumed by this driver path.

## HAL buffer contract and test preparation

A separate public Beyond0 S10e vendor binary was recovered for comparison and its Thumb-2 methods were disassembled. Its Build ID is `59c8d485e3e238436e067348882030c1`, which **does not match** the installed device HAL Build ID `2954034e91ab9505aef839b7d9feded0`. Treat these HAL method-level details as a reference implementation, not byte-for-byte proof of the exact installed binary. The kernel-side ABI and driver findings above are from the target boot image.

In that reference HAL, `ExynosSqzBase::setBuf(..., int *fds, unsigned int *lengths, int count)` populates 12-byte descriptors with the DMA-BUF FD, byte length, and type 1. The character-pointer overload sets type 2 (USERPTR). Its execute method checks one or two descriptors and submits the 32-bit ioctl `0xC1344D00`.

The target kernel's recovered USERPTR branch is decisive: descriptor type 1 calls `dma_buf_get(fd)`, then attaches and maps the DMA-BUF; descriptor type 2 takes an error path and returns `-EINVAL`. The matching public driver source explains why: the userspace VMA-to-DMA-BUF resolver is wrapped in `#if 0` and returns NULL. Therefore, a standalone test must supply a real DMA-BUF FD, not a normal pointer from `malloc` or `mmap`.

Samsung's Exynos ION userspace API provides the necessary route: `exynos_ion_open()`, `exynos_ion_alloc()`, `exynos_ion_sync_start()`, and `exynos_ion_sync_end()`. The reference allocator uses the system heap mask `1 << 0` for ordinary DMA-BUF allocation. A 128×64 NV21 test frame needs 12,288 bytes: 8,192 bytes of Y followed by 4,096 bytes of interleaved VU. This size and width satisfy the JSQZ driver's minimum-size and NV21 alignment checks.

A one-shot test source has been prepared at [`analysis/jsqz_probe.c`](jsqz_probe.c). It dynamically loads `/vendor/lib64/libion_exynos.so`, allocates and maps a cached system-heap DMA-BUF, brackets CPU writes with the Exynos DMA-BUF sync calls, and fills one deterministic NV21 pattern. By default it performs preparation only and does not open `/dev/jsqz`; the `--execute` flag is required to submit exactly one native 64-bit task (`0xC1504D00`). It supports grey, black, white, gradient, checker and deterministic noise inputs. The 64-bit task layout is compile-time checked against the target kernel's 336-byte native ABI. The source has passed a hosted GCC syntax check with warnings treated as errors and all ABI static assertions enabled ([validation run](https://github.com/SF12-YT/dummy-assets/actions/runs/43661115433)).

**Important runtime caveat:** the target driver scheduler waits with `wait_for_completion()`, not a timed wait. If the hardware never raises its completion IRQ, the calling task may remain blocked. The harness therefore separates DMA-BUF preparation from the single hardware submission; do not use `--execute` until the dry run has succeeded and a potentially stalled first transaction is acceptable. It does not alter device-node permissions or SELinux policy.

## Current-device camera observation

Your 10× Pro capture log showed the camera HAL failing to open `/dev/jsqz` with **error 13 (Permission denied)** before the JPEG capture. The JPEG still completed successfully. The log does not show a successful JSQZ open or task submission for that capture.

That means this kernel reverse-engineering identifies what the hardware and driver do, but **does not establish that JSQZ contributed to that particular photo**. It also does not justify changing device-node permissions or SELinux policy; those changes have deliberately not been made.

## Source correspondence and limits

The semantic source reference used for structure names and helper logic is the public [LineageOS Exynos 9820 kernel tree at commit `a412cfaf86b980ce35cae96a35f678c8ea551575`](https://github.com/LineageOS/android_kernel_samsung_exynos9820/tree/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/media/platform/exynos/jsqz). Relevant files:

- [`jsqz-core.c`](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/media/platform/exynos/jsqz/jsqz-core.c)
- [`jsqz-regs.h`](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/media/platform/exynos/jsqz/jsqz-regs.h)
- [`hwjsqz.h`](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/include/uapi/linux/hwjsqz.h)

This is a matching Exynos 9820 JSQZ source lineage, **not proof that this exact LineageOS commit is the Cruel Kernel source revision**. Function names, strings, ABI constants, register offsets and the recovered control-flow in the target boot image agree with that implementation. The machine-code findings above come from the uploaded target kernel itself.

The internal quantisation algorithm inside the JSQZ silicon is not present in the Linux driver. The binary establishes how the IP is fed, started, signalled and read; it cannot reveal the RTL's internal image analysis or exact per-pixel/quantiser mathematics. Determining that would require a working runtime interface with carefully controlled YUV inputs and captured output tables, or Samsung's hardware documentation/RTL.

## Files and reproducibility

- [Private release containing the extracted boot and kernel payload](https://github.com/SF12-YT/dummy-assets/releases/tag/jsqz-boot-analysis)
- [Successful kernel symbol/disassembly workflow run #6](https://github.com/SF12-YT/dummy-assets/actions/runs/43659575021)
- [Recovered JSQZ symbol list](jsqz-symbols.txt)
- [Full driver disassembly](jsqz-driver-disassembly.txt)
- [String offsets](jsqz-string-offsets.txt)

No additional device-side changes were made during this analysis.
