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

The API declares NV21 or YUV422 image information, dimensions, optional stride, and one or two input buffers. Its UAPI includes both DMA-BUF file descriptors and USERPTR descriptors, but the USERPTR route is not usable through this driver's task-submission path. The helper that resolves a USERPTR to a DMA-BUF has its VMA lookup code compiled out (`#if 0`) and returns `NULL`; `jsqz_buffer_get_and_attach()` then rejects the missing DMA-BUF with `-EINVAL`. The target `jsqz_process` disassembly confirms that descriptor type 1 takes the `dma_buf_get()` path, while type 2 takes the error path before buffer mapping. **For this target, use `HWSQZ_BUFFER_DMABUF` (type 1); do not use USERPTR (type 2).** The DMA-BUF is attached and mapped into the JSQZ device address space through Exynos IOVMM/System MMU.

The hardware reads the input (`DMA_TO_DEVICE` from the CPU driver's perspective). There is no output pixel buffer prepared by this operation. The result returned to userspace is the quantisation data.


### YUV422 chroma-plane offset defect in the target binary

The UAPI declares YUV422 support, but the target's address calculation for the V plane appears incorrect. The matching source expression is:

```c
v_addr = u_addr + ((width + 1) >> 1 * height);
```

Because multiplication has higher C precedence than shift, this parses as shifting `width + 1` right by `height`, not as `((width + 1) >> 1) * height`. The target boot-image disassembly confirms the resulting code: both the one-buffer path at `ffffff8008b0a120` and the two-buffer path at `ffffff8008b0a528` execute `lsr w2, w2, w3` where `w2 = width + 1` and `w3 = height`.

For the 128×64 case, AArch64's 32-bit variable shift uses the shift amount modulo 32 (the low five bits); 64 therefore becomes a shift by zero. The computed V offset is consequently 129 bytes from U. A planar YUV422 layout needs a U-plane size of `(128 / 2) × 64 = 4096` bytes before V begins. This would cause the two chroma planes to overlap heavily. The relevant [Arm LSRV instruction reference](https://documentation-service.arm.com/static/6245c734b059dc5ff9a8bdab) specifies the modulo-register-width shift behaviour.

This is a **static target-binary finding**, not a runtime-observed YUV422 result. All fourteen experiments used NV21 (`cs = 0`), for which the driver intentionally sets U and V to the same base address of the interleaved VU plane; those results are unaffected. Do not treat YUV422 as validated by the NV21 tests, and do not use this path for further experiments without first correcting or otherwise accounting for the offset bug.

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

The bus-config register carries the stride enable/value (bit 16 and low 13 bits). There are two distinct control/status bits here: the launch helper clears bit 24 when timeout control is disabled, while the post-completion scheduler checks bit 28 as the JSQZ timeout/bug indication. Do not conflate bit 24 with the bit-28 fallback decision.

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

The public Beyond0 camera HAL dump was also checked. Its Build ID is `7fba34153b4f05284f31fe62c4b6a672`, which differs from the installed target camera HAL Build ID `1b7a6dfaf4dcbf7034b467a6fcec2fc1`; it is therefore a corroborating reference, not an exact build match. Its dynamic imports explicitly include `ExynosJsqz::create`, `setInBuf(int*, unsigned int*, int)`, `setImageInfo`, `setJsqzConfig`, `run`, `getResultQT(unsigned char*)`, and `destroy`. It also contains the `[SAMSUNG_HW_JQ]` log strings for the Jpeg Squeezer instance, its input-buffer queue, and forwarding the resulting Q table to the JPEG encoder's `setQuality(qtable[])` path. This independently corroborates the intended pipeline: JSQZ derives a Q table from the source frame, then the JPEG encoder uses that table. The stripped reference binary did not yield reliable direct named call-site xrefs, so I am not inferring exact control-flow order or target addresses from it.

Reference files: [camera HAL imports](reference-camera-hal-undefined-jsqz.txt), [camera HAL strings](reference-camera-hal-strings.txt), [Build ID comparison](reference-camera-hal-version-match.txt), and [call-site scan result](reference-camera-hal-jsqz-xrefs.txt). The corresponding inspection run is [Actions run #12](https://github.com/SF12-YT/dummy-assets/actions/runs/43661495882).

The descriptor handling in the target kernel is explicit. In `jsqz_process` (starting at `ffffff8008b092d0`), the type byte at descriptor offset 16 is checked: type 1 branches to `dma_buf_get(fd)` at `ffffff8008b09574`, then attaches and maps the DMA-BUF; type 2 enters the error path and returns `-EINVAL`. The matching public source explains why: `jsqz_get_dmabuf_from_userptr()` has its VMA lookup code compiled out (`#if 0`) and returns `NULL`, which `jsqz_buffer_get_and_attach()` rejects before `jsqz_buffer_map()` is called.

    There is a potentially confusing lower-level detail: `jsqz_dma_addr_map()` at `ffffff8008b0a690` contains a generic branch to `exynos_iovmm_map_userptr()` when no DMA-BUF scatter-gather mapping is present. That branch does not make USERPTR submissions usable here; the top-level `jsqz_process` USERPTR path fails earlier during buffer acquisition and never reaches that mapper. The practical rule remains: submit a real DMA-BUF FD with descriptor type 1, not a normal pointer from `malloc` or `mmap`.

The matching Exynos 9820 4.14 ION UAPI provides a simpler route that avoids loading a vendor library. Its `ion_allocation_data` is 24 bytes (`u64 len`, three `u32` fields for heap mask, flags and returned fd, plus a reserved `u32`), and `ION_IOC_ALLOC` returns a DMA-BUF fd directly in the `fd` field. The reference `ion_ioctl` implementation passes the requested length, heap mask and flags to `ion_alloc()`, then copies that fd back to userspace. The target phone's debugfs heap listing independently confirmed `ion_system_heap` has heap ID 0, so the system-heap mask is `1 << 0`.

The same kernel UAPI defines `DMA_BUF_IOCTL_SYNC` with an 8-byte flags structure. The DMA-BUF ioctl handler translates START/WRITE and END/WRITE into the buffer's begin/end CPU-access callbacks, which is the correct cache-coherency bracket around CPU writes to a cached allocation. The probe subsequently confirmed this ABI on the Cruel Kernel target: the recorded runs successfully allocated the system-heap DMA-BUF, mapped it, populated it and completed the DMA-BUF CPU-access synchronisation calls before submission.

A one-shot test source is prepared at [`analysis/jsqz_probe.c`](jsqz_probe.c). It now opens `/dev/ion` and uses `ION_IOC_ALLOC` directly, then maps a cached system-heap DMA-BUF and brackets CPU writes with `DMA_BUF_IOCTL_SYNC`. This removes the previous `dlopen("/vendor/lib64/libion_exynos.so")` failure caused by Termux's isolated Android linker namespace. The 128×64 NV21 frame is 12,288 bytes: 8,192 bytes of Y followed by 4,096 bytes of interleaved VU; its width meets the driver's NV21 alignment check. By default, the probe performs allocation, mapping and pattern population only, and does not open `/dev/jsqz`. The `--execute` flag is still required to submit exactly one native 64-bit task (`0xC1504D00`). It supports the tested luma patterns (grey, black, white, gradient, 8×8 checker, 4×4 luma checker at ±64 and ±16, and deterministic noise), constant non-neutral chroma, and spatial chroma checkers at ±64/±32/±16/±8 with both tested tile sizes. Compile-time assertions check the native 336-byte JSQZ task ABI plus the ION and DMA-BUF ioctl numbers. GCC validation with `-Wall -Wextra -Werror` passed in [Actions run #14](https://github.com/SF12-YT/dummy-assets/actions/runs/43664643364).

**Important runtime caveat:** the target driver scheduler waits with `wait_for_completion()`, not a timed wait. If the hardware never raises its completion IRQ, the calling task may remain blocked. The harness therefore separates DMA-BUF preparation from the single hardware submission; do not use `--execute` until the dry run has succeeded and a potentially stalled first transaction is acceptable. It does not alter device-node permissions or SELinux policy.

## Camera HAL access-control finding

The target camera log showed `/dev/jsqz` open failing with error 13, while the device node was observed as `0600 root:root`. The matching Exynos 9820 JSQZ driver registers a misc device without assigning its mode; the matching kernel devtmpfs code defaults a zero mode to `0600` and root ownership. This makes ordinary non-root camera-service access fail at the discretionary-permission layer before any JSQZ ioctl can be submitted.

The S10e Exynos 9820 common device tree provides a complete access configuration: `ueventd.rc` sets `/dev/jsqz` to `0660 cameraserver:camera`; `file_contexts` labels it `jsqz_device`; and `hal_camera_default.te` grants `rw_file_perms` to that character-device type. `common.mk` copies the ueventd file into the vendor image, and the common board configuration includes the vendor SELinux policy directory.

This is strong evidence for a node-access configuration problem on the observed installation. It does not establish whether SELinux also denies access, because no relevant AVC record or live target `ls -Z` output is available. The user's camera log proves an open attempt failed, not that the camera HAL reached an ioctl; successful JPEG capture shows the broader capture pipeline can continue without the JSQZ open succeeding. The public reference camera HAL and JSQZ library are build-mismatched comparisons, not exact target-binary proof.

See the separate [camera integration and access-control analysis](JSQZ-CAMERA-INTEGRATION.md) for source locations, the full reasoning, and the narrow remaining verification. No device permissions or SELinux settings were modified.

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


## Exact validation of the current 128×64 probe against the target kernel

A second pass over the recovered target-kernel machine code confirms the concrete values used by `analysis/jsqz_probe.c`:

- The driver uses `stride` when it is non-zero; otherwise it substitutes `width`. The probe's stride of zero therefore gives an effective row stride of 128 bytes.
- For colour-space value 0 (NV21), the accepted alignment test is satisfied when the effective stride is divisible by 64. Since 128 is divisible by 64, this input passes the check and does not take the early default-table path.
- The dimension guard rejects widths of 31 or less and heights of 7 or less. A 128×64 frame passes.
- With one NV21 input buffer, the driver computes the required size as `width × height + even_width × ceil(height / 2)`. At 128×64 this is `8192 + 4096 = 12288` bytes, exactly the allocation and descriptor length in the probe.
- The native descriptor is 24 bytes; its FD, 64-bit length and type byte are laid out at offsets 0, 8 and 16 respectively. The probe's descriptor matches that layout and uses type 1. The target `jsqz_process` path calls `dma_buf_get`, attaches the buffer, maps the attachment, then maps it into the JSQZ IOVMM.
- The kernel requires the supplied buffer length to be at least the computed image size. The probe supplies exactly that size.

This is stronger than merely matching a public driver header: the checks above were read from the disassembly of the kernel extracted from the user's uploaded Cruel Kernel boot image. It establishes that the probe's metadata and DMA-BUF descriptor satisfy the visible software-side validation checks. It still cannot prove that the silicon is powered, that its clocks/IRQ function correctly under this kernel, or that it will return a non-fallback table.

### New non-submitting device-open check

The probe now accepts `--check-device`. It performs the same ION allocation, mapping and frame fill as the dry run, then opens and closes `/dev/jsqz` without issuing an ioctl. This isolates node access from hardware execution. `--execute` and `--check-device` are mutually exclusive.

A new GitHub Actions workflow, `.github/workflows/jsqz-probe-check.yml`, compiles the probe with both GCC and Clang using `-Wall -Wextra -Werror`, then checks that the help output advertises the device-open preflight. This workflow does not attempt to access device hardware.


## Task submission semantics confirmed from the UAPI and driver

The public Samsung UAPI header at the matching Exynos 9820 source revision confirms the probe's enum values rather than relying on guessed numeric constants:

- `JSQZ_MODE_NORMAL = 0`
- `JSQZ_FUNCTION_LIVE = 0`
- `JSQZ_INPUT_CS_FORMAT_NV21 = 0`
- `HWSQZ_BUFFER_DMABUF = 1`

The UAPI describes `info_out` as the image information supplied to the hardware, `buf_out` as the source buffer(s), and `buf_q[32]` as the output quantisation table. The kernel's native ioctl copies exactly 336 bytes from userspace, calls `jsqz_process`, and copies the task back to userspace on success. The probe reads the 32 returned words and can save them as a 128-byte binary file.

The target-kernel disassembly independently confirms the native ioctl command comparison against `0xC1504D00`, the 336-byte copy-in/copy-out, and the worker's output-table read after completion. It also confirms the worker calls `wait_for_completion()`; the scheduler does not use a bounded wait. The source reference sets the hardware timeout parameter to zero, which clears the timeout-enable bit in the bus configuration register. Consequently, a first hardware request can remain blocked if the completion interrupt never arrives.

The current `--execute grey` path therefore has a well-defined purpose: one 128×64 NV21 source frame, one DMA-BUF, NORMAL/LIVE configuration, one ioctl, then return the 32 output Q-table words. No image output buffer is required for this API path. The output is the quantisation table, not a compressed image stream.

The successful `--check-device` run proves `open(O_RDWR)` and close succeed. It does not exercise the ioctl, runtime power-up, DMA attachment, IOVMM mapping, hardware start, IRQ, or output-register read.


## Runtime pattern tests — 15 November 2026

The device-side probe has now completed fourteen single-frame NV21 submissions through the native 64-bit ioctl, covering thirteen distinct input patterns plus one repeatability run. All used 128×64, stride 0 (effective stride 128), one ION system-heap DMA-BUF of 12,288 bytes, NORMAL/LIVE mode, and returned success.

| Pattern | Luma input | Returned table |
|---|---|---|
| grey | Y=128 throughout | Table A |
| checker | alternating 8×8 blocks of Y=16 and Y=235 | Table A, exactly identical to grey |
| black | Y=16 throughout | Table B |
| gradient | horizontal Y gradient | Table C |
| noise | deterministic high-frequency Y noise | Table D |
| chroma | Y=128; V=192, U=64 throughout | Table E, exactly identical to grey |
| chroma-checker | Y=128; V/U ±64 spatial checker | Table F |
| chroma-checker-low | Y=128; V/U ±32 spatial checker | Table G |
| chroma-checker-tiny | Y=128; V/U ±16 spatial checker | Table H |
| chroma-checker-micro | Y=128; V/U ±8 spatial checker | Table I |
| luma-checker-4 | Y=64/192 in alternating 4×4 tiles; neutral chroma | Table J |
| grey-repeat | Y=128 throughout, repeated baseline | Table A again; exact 32/32-word match to the first grey run |
| chroma-checker-fine | Y=128; V/U ±64 in 2×2 chroma-sample tiles | Table K; exactly identical to Table F |
| luma-checker-4-low | Y=112/144 in alternating 4×4 tiles; neutral chroma | Table L |

Grey, checker, black, gradient and noise used neutral chroma (VU=128). The `chroma` test used V=192/U=64 throughout. Table A is non-default-looking; Table B closely matches the matching public driver's built-in `init_q_table`.

The full returned word tables are preserved below. Each line contains four 32-bit words; the first 16 words are luma and the last 16 are chroma.

**Table A — grey and checker (identical, 32/32 words)**

```
03030301 03030303 03030303 03030303
03030303 03030403 04050303 04050505
06050404 05050607 05040607 07060906
08080808 09080608 0c0c090a 150f0f0c
03030301 03040303 05080403 08080504
08080808 08080808 08080808 08080808
08080808 08080808 08080808 08080808
08080808 09080808 0c0c0809 150f0f0c
```

**Table D — deterministic noise**

```
04040401 04040404 04040404 04040404
04040404 04040504 05050405 05050506
06050505 07060607 07060607 07080907
0b0a0808 0d0b080a 11100b0d 1c151510
03030301 03040303 05080403 08080504
08080808 08080808 08080808 08080808
08080808 08080808 08080808 08080808
08080808 09080808 0c0c0809 150f0f0c
```

Compared with Table A (grey/checker), Table D changes all 16 luma words and none of the 16 chroma words. The luma table becomes more aggressive across the frequency coefficients; for example word 15 changes from `150f0f0c` to `1c151510`. This is strong evidence that JSQZ adapts its luma quantisation output to spatial detail. The constant-chroma test is recorded separately as Table E below.

**Table E — non-neutral constant chroma**

```
03030301 03030303 03030303 03030303
03030303 03030403 04050303 04050505
06050404 05050607 05040607 07060906
08080808 09080608 0c0c090a 150f0f0c
03030301 03040303 05080403 08080504
08080808 08080808 08080808 08080808
08080808 08080808 08080808 08080808
08080808 09080808 0c0c0809 150f0f0c
```

Table E is exactly identical to Table A (grey): all 32 words match. This test held luma constant at 128 and changed the NV21 chroma planes to V=192 and U=64. No change was observed in either the luma or chroma quantisation matrix. This does not establish that chroma adaptation is absent; it establishes only that this particular constant chroma offset did not alter the returned table.

The `chroma-checker` input pattern held all luma samples at 128 and alternated NV21 VU pairs (V=192/U=64 versus V=64/U=192) in 4×4 chroma-sample tiles. Because NV21 is 4:2:0, each chroma tile corresponds to an 8×8 luma region. Its runtime result is recorded as Table F above; GCC and Clang compile checks passed in [Actions run #43671178141](https://github.com/SF12-YT/dummy-assets/actions/runs/43671178141).






**Table L — lower-contrast 4×4 luma checker**

```
03030301 03030304 04030303 03030403
03030303 03040403 04050305 04050505
06050404 05050607 05060607 07060906
08080808 09080608 0c0c090a 150f0f0c
03030301 03040303 05080403 08080504
08080808 08080808 08080808 08080808
08080808 08080808 08080808 09080808
0c0c0809 150f0f0c
```

Table L was returned by one successful hardware submission with Y alternating between 112 and 144 in 4×4 tiles (mean 128) and neutral chroma. Compared with grey Table A, six Y-bank bytes change at linear offsets 4, 11, 13, 22, 24 and 42; all 64 C-bank bytes remain identical. Compared with the higher-contrast Table J (Y=64/192, same tile geometry), these six changes remain and five additional Y offsets (38, 51, 53, 58 and 60) are absent at the lower contrast. This demonstrates a graded, stimulus-specific luma response at both tested amplitudes; it does not identify a threshold below ±16.

**Table K — finer spatial chroma checker, ±64**

```
03030301 03030303 03030303 03030303
03030303 03030403 04050303 04050505
06050404 05050607 05040607 07060906
08080808 09080608 0c0c090a 150f0f0c
03030301 03040304 05080403 08080504
08080808 08080808 08080808 08080808
08080808 08080808 0b080808 09080808
0c0d0809 150f0f0d
```

Table K was returned by one successful hardware submission with Y=128 and the same alternating VU pairs (192,64)/(64,192) as Table F, but at 2×2 chroma-sample tile size rather than 4×4. It is bit-for-bit identical to Table F. Relative to grey Table A, the same four C-bank bytes at linear offsets 4, 51, 58 and 60 change; no Y-bank bytes change. Changing the checker scale over this one octave therefore did not alter this quantised output.

**Table J — 4×4 luma checker**

```
03030301 03030304 04030303 03030403
03030303 03040403 04050305 04050505
06050404 05060607 05060607 07060906
0b080808 09080808 0c0d090a 150f0f0d
03030301 03040303 05080403 08080504
08080808 08080808 08080808 08080808
08080808 08080808 08080808 08080808
08080808 09080808 0c0c0809 150f0f0c
```

Table J was produced by one successful hardware submission with Y alternating between 64 and 192 in 4×4 luma tiles (mean 128), with neutral NV21 chroma VU=128. Compared with grey Table A, 11 of 64 luma bytes change at linear Y-bank offsets 4, 11, 13, 22, 24, 38, 42, 51, 53, 58 and 60. These affect luma words 1, 2, 3, 5, 6, 9, 10, 12, 13, 14 and 15. All 64 chroma bytes remain identical to Table A. The result directly demonstrates a luma-table response to spatial variation within the 8×8 regions while the chroma table stays unchanged. The offsets are linear byte positions, not inferred 8×8 coefficient coordinates.

**Table I — spatial chroma checker, ±8**

```
03030301 03030303 03030303 03030303
03030303 03030403 04050303 04050505
06050404 05050607 05040607 07060906
08080808 09080608 0c0c090a 150f0f0c
03030301 03040304 05080403 08080504
08080808 08080808 08080808 08080808
08080808 08080808 08080808 08080808
08080808 09080808 0c0c0809 150f0f0c
```

Table I was returned by a successful single hardware submission using Y=128 and alternating VU pairs (136,120) and (120,136) in the same 4×4 chroma-sample tiles as Tables F–H. It is exactly identical to Tables G and H. Compared with grey Table A, all luma words are unchanged and only chroma word 17 differs (`03040303` → `03040304`), corresponding to the same one changed C-bank byte at linear offset 4. The observed quantised response therefore persists at ±8 for this stimulus. This is a lower bound on the tested amplitude, not a measured threshold.

**Table F — spatial chroma checker**

```
03030301 03030303 03030303 03030303
03030303 03030403 04050303 04050505
06050404 05050607 05040607 07060906
08080808 09080608 0c0c090a 150f0f0c
03030301 03040304 05080403 08080504
08080808 08080808 08080808 08080808
08080808 08080808 0b080808 09080808
0c0d0809 150f0f0d
```

Table F was returned for a spatially varying chroma pattern with constant luma Y=128. Compared with Table A (grey), all 16 luma words are identical, while four chroma words differ: word 17 (`03040303` → `03040304`), word 28 (`08080808` → `0b080808`), word 30 (`0c0c0809` → `0c0d0809`), and word 31 (`150f0f0c` → `150f0f0d`). This is direct evidence that the returned chroma quantisation matrix responds to spatial chroma variation independently of the luma matrix, under this test pattern.

**Table C — gradient**

```
03030401 03030303 03030303 03030303
03030303 03030403 04050303 04050505
06050404 05050607 05040607 07060906
08080808 09080608 0c0c090a 150f0f0c
03030301 03040303 05080403 08080504
08080808 08080808 08080808 08080808
08080808 08080808 08080808 08080808
08080808 09080808 0c0c0809 150f0f0c
```

Table C differs from Table A (grey/checker) in exactly one 32-bit word: word 0 changes from `03030301` to `03030401`. The other 31 words are identical, including all 16 chroma words. This is a small but direct input-dependent change in the returned luma table.

**Table B — black**

```
01010101 01010101 01010101 03020201
02020202 03030402 04050302 04050505
06050404 05050607 04040607 07060906
08080808 09060508 0a08090a 0a080807
02010101 02040202 05080402 08080504
08080808 08080808 08080808 08080808
08080808 08080808 08080808 08080808
08080808 08080808 08080808 0a080808
```

Table B differs from Table A in 17 of the 32 words. Table B differs from the exact fallback table in only two words.

### What the target binary actually does with the status bit

The target boot-image disassembly resolves the fallback decision:

- In `jsqz_task_schedule`, after `wait_for_completion()`, it reads bus-config register `base + 0x108`.
- At `ffffff8008b0a1dc`, `tbnz w0, #28` branches to the fallback-copy block at `ffffff8008b0a358`.
- That block copies the driver's global 32-word `init_q_table` into the task result.
- When bit 28 is clear, the scheduler instead reads the 16 luma words from `base + 0x010` and the 16 chroma words from `base + 0x050`.
- Separately, the launch path clears bit 24 at `ffffff8008b0a0f4`; that is timeout-control configuration, not the post-run bit-28 status test.

### Exact fallback table recovered from the target boot image

The first attempt to dump the inferred virtual address from `vmlinux.elf` returned no bytes because the reconstructed ELF did not expose that data address as a dumpable section. The fallback table was then recovered directly from the original extracted `kernel.bin` by searching for its known 30-word prefix/suffix signature. Exactly one location matched: file offset `0x1274cd8`.

The complete target-kernel fallback table is:

```
01010101 01010101 01010101 03020201
02020202 03030402 04050302 04050505
06050404 05050607 04040607 07060906
08080808 09060508 0a08090a 08080807
02010101 02040202 05080402 08080504
08080808 08080808 08080808 08080808
08080808 08080808 08080808 08080808
08080808 08080808 08080808 08080808
```

This is an exact 32/32 match to the public driver's `init_q_table`; the target build does **not** have a two-word-different fallback table. The extraction and comparison are recorded in [`jsqz-default-table.txt`](jsqz-default-table.txt).

### Interpretation of the fourteen completed submissions

The stride is valid for NV21 (128 is divisible by 64), and the frame dimensions and buffer size pass the target driver's guards. Therefore the early alignment fallback is not expected for any of these tests.

- Grey and checker returned exactly the same Table A, which is substantially different from the exact fallback table. They did not return the fallback copy.

- The `luma-checker-4` test returned Table J. Relative to grey Table A, 11 of 64 luma bytes change across 11 luma words, while all 64 chroma bytes remain identical. The input alternates Y=64/192 in 4×4 tiles, so each 8×8 region contains within-block variation. This confirms that the luma table responds to this spatial detail independently of the chroma table. It also explains why the earlier checker made of flat 8×8 tiles matched grey: the latter did not vary within those blocks.
- The finer `chroma-checker-fine` run used the same ±64 V/U values as Table F but reduced the tile size from 4×4 to 2×2 chroma samples. It returned Table K, exactly identical to Table F across all 32 words (128/128 bytes). Thus, for these two tested tile sizes and this input amplitude/configuration, halving the chroma checker tile size produced no measurable change in the returned quantisation table. This is a narrow result, not proof that JSQZ is generally insensitive to spatial frequency.
- The lower-contrast `luma-checker-4-low` input (Y=112/144, ±16) retains a measurable response relative to grey: six Y-bank bytes change, at linear offsets 4, 11, 13, 22, 24 and 42. All C-bank bytes remain unchanged. The higher-contrast Table J input (Y=64/192, ±64) changed eleven Y bytes; its six changes at the same offsets remain, while five additional offsets (38, 51, 53, 58 and 60) change only at the higher tested contrast. Thus the luma response persists at ±16 for this pattern and is weaker than at ±64. No threshold below ±16 has been measured.
- The repeatability run submitted the original grey pattern again. All 32 returned words matched Table A exactly (128/128 bytes); there was no output variation between the two identical grey submissions. This confirms deterministic output for this input under the same device/configuration conditions, though it does not establish repeatability for every possible pattern.
- Gradient returned Table C: only word 0 differs from Table A (`03030401` versus `03030301`); the remaining 31 words are identical. This is a small but direct response to a change in luma spatial content, while chroma remains unchanged.
- Deterministic noise returned Table D. Compared with Table A, all 16 luma words differ while all 16 chroma words are identical. The higher-frequency luma coefficients increase substantially (word 15: `150f0f0c` → `1c151510`). This is strong evidence that the hardware adapts luma quantisation to spatial detail. Chroma was held neutral (VU=128) for this test.
- The non-neutral chroma test returned Table E, exactly identical to Table A across all 32 words. It used constant Y=128 with V=192 and U=64. Thus this particular chroma offset did not measurably affect either matrix. This is not proof that the chroma path is ignored or that chroma adaptation is impossible; spatially constant chroma may not exercise whatever image statistics the hardware uses.
- The spatial chroma-checker test returned Table F with Y held constant at 128. Compared with Table A, all 16 luma words are identical and four chroma words (17, 28, 30 and 31) differ. This demonstrates an independent chroma-table response to spatial chroma variation for this input pattern.
- Black returned Table B. Although it matches the fallback in 30 of 32 words, word 15 is `0a080807` instead of `08080807`, and word 31 is `0a080808` instead of `08080808`. In little-endian byte order, these are exactly the final byte of the 64-byte luma matrix and the final byte of the 64-byte chroma matrix; both change from `0x08` to `0x0a`, while the other 126 bytes match. Therefore it also did **not** return the exact fallback copy.
- Given the target driver's two known fallback paths (invalid alignment and the post-completion bit-28 branch), and the validated input dimensions/stride, all fourteen submissions most likely reached the output-register read path. Their returned tables are not equal to the recovered fallback table. The empty `dmesg` search is consistent with this, although absence of a log line alone is not proof.
- Black differs from grey/checker in 17 of 32 words. That is useful evidence that the returned Q table is responding to the input pattern, not merely returning one constant fallback table. It does not by itself prove every DMA byte was consumed as intended, but it is substantially stronger evidence of functional JSQZ execution than the earlier fallback hypothesis.
- Grey and checker remain identical. The checker consists of uniform 8×8 blocks aligned to the JPEG 8×8 transform grid, so each block has no within-block luma variation. That makes it a weak contrast test against uniform grey. The prepared `luma-checker-4` pattern is the next controlled follow-up because it varies within each 8×8 luma region.

No further hardware submission is needed to settle the fallback-table question: the exact target table has now been recovered and the black output is demonstrably not that table.

## Controlled chroma contrast experiment — runtime results

The spatial chroma checker at ±64 (Table F) changed four bytes in the chroma table while leaving the luma table identical to grey. The follow-up `chroma-checker-low` preserved the same spatial pattern, tile size, luma plane and execution parameters, but halved the chroma displacement to ±32. Its output is Table G above.

The next run, `chroma-checker-tiny`, halved the displacement again to ±16. Its complete 32-word output was **identical to Table G**. Compared with grey Table A:

- All 16 luma words are unchanged.
- Only chroma word 17 changes: `03040303` → `03040304`.
- The same single C-bank byte at linear offset 4 remains changed (0x03 → 0x04).
- Exactly 1/64 chroma bytes changes at ±32, ±16 and ±8, compared with 4/64 at ±64.

This shows that, for this exact spatial pattern and device configuration, the one-byte chroma-table response persists at least down to ±8 component amplitude. The output has reached the same quantised table at ±8, ±16 and ±32; there is no evidence of a further change among those three levels. The three additional changed bytes at ±64 are absent at all three lower amplitudes. The response is still measurable at the smallest tested amplitude, ±8. No test below ±8 has been run, so the lower transition point has not been located.

The matching controls are the same 128×64 NV21 frame, Y=128, 4×4 chroma-sample checker tiles (equivalent to 8×8 luma blocks), NORMAL/LIVE mode and 12,288-byte ION DMA-BUF. Only the chroma sample levels differ:
- ±64: VU pairs (192,64) and (64,192), Table F.
- ±32: VU pairs (160,96) and (96,160), Table G.
- ±16: VU pairs (144,112) and (112,144), Table H; output exactly matches Table G.
- ±8: VU pairs (136,120) and (120,136), Table I; output exactly matches Tables G and H.

The ±8 `chroma-checker-micro` device-side run has now completed successfully. Its complete output is Table I below. It exactly matches Tables G and H, confirming that the same single-byte chroma response persists at ±8. This does not establish a universal threshold for other patterns, resolutions or operating modes.

The `jsqz_qtable_diff.py` utility compares saved 128-byte dumps at packed-word and individual-byte levels. The ±32, ±16 and ±8 tests were performed on the phone and returned success.
## Q-table byte ordering — HAL reference confirmation

The available public Beyond0 reference HAL contains two overloads of `ExynosJsqz::getResultQT`. The `unsigned int *` overload copies 32 consecutive words into the caller's output at four-byte intervals. The `unsigned char *` overload at reference-library offset `0x221a` iterates over the same 128 bytes and explicitly writes the four bytes of each 32-bit word in low-to-high order to consecutive output positions. It does not apply a zigzag/raster reordering in that method.

**Build caveat:** this library's Build ID is `59c8d485e3e238436e067348882030c1`, not the installed target HAL's `2954034e91ab9505aef839b7d9feded0`. This is corroboration of the exposed HAL contract, not exact disassembly of the installed library. The native probe saves the 32 returned words directly on the target's little-endian ARM64 system; the diff utility therefore unpacks with explicit little-endian `<32I` and compares the 64-byte Y and C banks independently.

The exact 8×8 coefficient scan layout (zigzag versus raster) is **not established** by this HAL copy routine or the kernel register reads. Accordingly, `jsqz_qtable_diff.py` reports linear byte offsets within each 64-byte table and deliberately does not label them as 8×8 row/column coordinates.

## Byte-level comparison of the captured pattern outputs

The captured 32-word outputs were unpacked as little-endian bytes (64-byte Y bank followed by 64-byte C bank). The byte counts below include all completed tests through the lower-contrast luma checker:

| Input compared with grey Table A | Y bytes changed | C bytes changed | Observation |
|---|---:|---:|---|
| Gradient (Table C) | 1/64 | 0/64 | Y table byte offset 1 changes 0x03 → 0x04 |
| Deterministic noise (Table D) | 50/64 | 0/64 | All 50 changed Y bytes increase; 14 remain unchanged |
| Spatial chroma checker, ±64 (Table F) | 0/64 | 4/64 | C offsets 4, 51, 58 and 60 change; each increases |
| Spatial chroma checker, ±64, 2×2 chroma tiles (Table K) | 0/64 | 4/64 | Exact match to Table F; same four C offsets |
| Spatial chroma checker, ±32 (Table G) | 0/64 | 1/64 | Only C offset 4 changes, 0x03 → 0x04 |
| Spatial chroma checker, ±16 (Table H) | 0/64 | 1/64 | Identical to Table G; C offset 4 remains changed |
| Spatial chroma checker, ±8 (Table I) | 0/64 | 1/64 | Identical to Tables G and H; C offset 4 remains changed |
| Luma checker, 4×4 tiles, ±64 (Table J) | 11/64 | 0/64 | Y offsets 4, 11, 13, 22, 24, 38, 42, 51, 53, 58 and 60 change |
| Luma checker, 4×4 tiles, ±16 (Table L) | 6/64 | 0/64 | Y offsets 4, 11, 13, 22, 24 and 42 change; subset of Table J |
| Black (Table B) | 29/64 | 15/64 | 44 of 128 output bytes differ from grey |

For the spatial chroma checker specifically, the changed C-bank bytes are offset 4 (0x03 → 0x04), 51 (0x08 → 0x0b), 58 (0x0c → 0x0d), and 60 (0x0c → 0x0d). This confirms that the four changed packed words each contain exactly one changed byte.

The byte counts make the separation between the observed Y and C responses explicit: the deterministic-noise and 4×4 luma-checker inputs changed only the Y bank, while the spatial chroma checker changed only the C bank. The HAL byte-order caveat above still applies to interpreting these as matrix coordinates: the offsets are linear output-byte positions, not asserted 8×8 row/column positions.

## Within-block luma-frequency experiment — runtime results

The earlier `checker` test alternated Y=16 and Y=235 in 8×8 luma tiles. Because each tile aligned to an 8×8 block, every such block was internally constant; its output therefore matched the grey table and it was not a useful within-block frequency stimulus.

The `luma-checker-4` test has now completed successfully. Its controlled input was:

- Y alternates between 64 and 192 in 4×4 luma tiles, giving a mean of exactly 128.
- Every 8×8 region contains four alternating 4×4 quadrants, so the pattern has within-block spatial variation.
- NV21 chroma remains neutral VU=128 throughout.
- Frame dimensions, buffer size, mode and function remain identical to the other probes.

The returned Table J differs from grey Table A in 11 of the 64 luma bytes, spread across 11 of the 16 luma words. All 16 chroma words (64 bytes) are unchanged. This shows that within-block luma variation affects the luma quantisation matrix independently of the chroma matrix for this stimulus. The result complements the deterministic-noise test, which changed 50 luma bytes, and shows a measurable response to this lower-complexity periodic pattern.

## Controlled chroma spatial-frequency experiment — runtime result

The `chroma-checker-fine` run has completed successfully. It holds Y=128 and the same ±64 V/U checker levels as Table F, but changes the tile size from 4×4 to 2×2 chroma samples (4×4 instead of 8×8 luma-equivalent regions). The returned Table K is identical to Table F across all 32 words (128/128 bytes): no luma or chroma output byte changed as a result of this tile-size change. Therefore the quantised output is unchanged across these two tested spatial scales for this stimulus. This does not establish general spatial-frequency invariance; it is one matched comparison.

At the time this intermediate subsection was written, the phone had completed thirteen successful submissions: eleven base patterns, one repeatability submission, and this additional chroma-frequency variant. The later `luma-checker-4-low` run brought the final documented total to fourteen submissions (thirteen distinct patterns plus one grey repeat). Together, the results demonstrate repeatability for the grey baseline, independent luma/chroma table responses, and no detected table change in this particular one-octave chroma scale comparison.

## Lower-contrast within-block luma experiment — runtime result

The `luma-checker-4` pattern (Table J) used Y=64/192 (±64 around 128) in alternating 4×4 tiles and changed 11 Y-bank bytes while leaving all C-bank bytes unchanged. To isolate luma contrast while preserving spatial geometry, a new `luma-checker-4-low` pattern has been added. It uses Y=112/144 (±16 around 128) in the same 4×4 tiles, with neutral NV21 chroma. Comparing this result with Table J will vary only the luma amplitude. If its table still differs from grey, the response persists at ±16 for this pattern; if it matches grey, the output transition is bracketed between the tested ±16 and ±64 amplitudes. This remains a stimulus-specific quantised observation, not a universal threshold.

The `luma-checker-4-low` test completed successfully on the phone. Relative to grey, it changes six Y-bank bytes and zero C-bank bytes; all six changed offsets are also changed by the higher-contrast Table J stimulus.

## Additional algorithm leads — public Samsung patent comparison

The public UAPI defines two processing modes, `JSQZ_MODE_NORMAL` and `JSQZ_MODE_HIGH_QUALITY`, and two functions, `JSQZ_FUNCTION_LIVE` (preview input) and `JSQZ_FUNCTION_TRANSCODE` (original-image input). The driver packs these into the hardware format word at bits 29 and 28 respectively. The fourteen recorded submissions exercised **NORMAL + LIVE only**. HIGH_QUALITY and TRANSCODE remain statically understood as API options but have no captured output comparison in this investigation. The UAPI definitions are in the [public Exynos 9820 kernel header](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/include/uapi/linux/hwjsqz.h).

A potentially relevant but unlinked Samsung disclosure surfaced during the external patent search: [US20200145662A1, “Method and apparatus for encoding image by using quantization table adaptive to image”](https://patents.google.com/patent/US20200145662A1/en), with a stated priority date of 1 November 2018. It describes deriving an image-adaptive quantisation table with a machine-learning model from multiple image patches and corresponding transform-coefficient groups. Described model inputs can include transform coefficients or statistics grouped by coefficient location; disclosed model options include fully connected layers and a convolutional neural network. This is conceptually compatible with a system that analyses raw image content and produces a frame-level Q table.

**There is no evidence in the patent that it describes the Exynos 9820 JSQZ block.** The text does not identify Exynos, JSQZ, the register map, the kernel interface or this particular camera pipeline. It may be a separate software or processor-based Samsung approach. It is therefore a research lead only—not a basis for claiming that JSQZ uses neural networks.

A second Samsung/KAIST publication, [US20140146881A1, “Method and apparatus for estimating a quantization table for a video image”](https://patents.google.com/patent/US20140146881A1/en), is a weaker match: its described method estimates a table for an already-compressed image block using candidate tables, edge-preserving filtering and energy-cost comparisons. That operating context differs from JSQZ's observed input contract, which supplies raw YUV image data and returns a table for later JPEG encoding. I have not treated it as an explanation of the JSQZ silicon.

The practical next reverse-engineering route is therefore not “assume the patent is the algorithm”, but to search for a patent or design disclosure explicitly matching the JSQZ/Exynos IP, or obtain an implementation artifact that connects a published algorithm to the actual hardware. The HIGH_QUALITY/TRANSCODE modes are also open behavioural dimensions, but they are outside the completed NORMAL/LIVE test matrix and do not justify another device submission without a focused question.
 
## Investigation status and practical completion criteria

**Estimated overall progress for a bounded, practical investigation of the target phone's JSQZ driver and observable Q-table behaviour: approximately 80–85%.** This is a project estimate, not a measurable scientific percentage.

- Target kernel, UAPI, native/compat ABI, DMA-BUF path, register map and fallback behaviour: approximately 95% understood.
- Hardware execution and proof of input-dependent Y/C table output on this device: approximately 90–95% established.
- Black-box mapping of output sensitivity to image content, spatial frequency, contrast and repeatability: approximately 90% characterised. The grey baseline repeated with an exact 32/32-word match, and the ±64 chroma checker returned an exact 32/32-word match at two tile sizes.
- Exact internal silicon algorithm (the estimator, thresholds, coefficient-to-position mapping and RTL): approximately 20–25% understood. The Linux driver and finite I/O experiments do not expose the RTL.

A sensible stopping point for the **bounded practical black-box investigation** has now been reached: the planned contrast comparison is complete, and the next work is to consolidate the results and limitations rather than keep adding arbitrary stimuli.

1. Consolidate the practical findings, byte-level comparisons, driver analysis and limitations into a final report.
2. Add further hardware experiments only when a concrete unresolved question justifies them.

With the planned contrast comparison now complete, the driver/API investigation and the principal observable Y/C response behaviours are adequately documented for this kernel/device combination, within the stated black-box limits. Exhaustively testing every image, resolution, format, mode, input value and hardware corner case is not a finite task. Reconstructing the exact proprietary silicon algorithm would require Samsung documentation, RTL or equivalent design-level evidence; black-box probes alone cannot promise that result.
