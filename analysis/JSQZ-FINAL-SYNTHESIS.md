# Exynos 9820 JSQZ — final practical synthesis

**As of:** 15 November 2026  
**Target:** Samsung Galaxy S10e SM-G970F, Exynos 9820  
**Kernel:** `4.14.113-Cruel Kernel M SebastianSNRL v1.0 N` (15 September 2023 build)  
**Boot payload SHA-256:** `838e2ac15c5270201eeafe67f6a603df25048783301f21e2617e5be68d142d5b`

This document consolidates the target-kernel reverse engineering and all fourteen recorded runtime submissions. It is the short, current conclusion document; the longer [driver analysis](JSQZ-DRIVER-ANALYSIS.md) remains the detailed chronological record.

## Executive conclusion

JSQZ is a hardware-assisted **quantisation-table generator**. It receives raw YUV image data, processes that input, and exposes two 64-byte tables: luma (Y) and chroma (C). The Linux driver returns those tables to userspace as 32 little-endian 32-bit words. It does not accept a JPEG bitstream or emit a JPEG bitstream; JPEG encoding is a subsequent stage that may use the generated tables.

The target kernel's ABI, buffer handling, register programming, interrupt path, fallback behaviour and output-register reads have been reconstructed from the extracted boot image. The runtime results show strong input-dependent changes in the Y and C banks, including changes isolated to one bank while the other remains unchanged.

**The practical black-box sequence is complete for its defined scope.** This does not mean the proprietary silicon algorithm has been reconstructed. Its internal estimator, thresholds, coefficient ordering and RTL remain unknown.

## What is established from the target kernel

### ABI and input contract

| Interface | Confirmed target value |
|---|---:|
| 32-bit compat ioctl | `0xC1344D00` |
| 32-bit compat task | 308 bytes (`0x134`) |
| 64-bit native ioctl | `0xC1504D00` |
| 64-bit native task | 336 bytes (`0x150`) |
| Input buffer type used by the probe | DMA-BUF, type 1 |
| Probe format/configuration | NV21, NORMAL, LIVE |
| Probe frame | 128 × 64, stride 0 (effective stride 128) |
| Input allocation | 12,288 bytes: 8,192 Y + 4,096 interleaved VU |

The tested frame passes the target's NV21 stride alignment and minimum-dimension guards. The driver uses the supplied stride when non-zero and otherwise substitutes the width. Its computed one-buffer NV21 size at 128 × 64 is exactly 12,288 bytes.

The probe allocates a cached system-heap DMA-BUF with the legacy Exynos ION ioctl, maps and fills it, and brackets CPU writes with `DMA_BUF_IOCTL_SYNC`. Those allocation, mapping and population steps succeeded on the target during the recorded tests.

### USERPTR is declared, but rejected by the active submission path

The UAPI defines both DMA-BUF and USERPTR descriptor types. In the matching driver source, however, `jsqz_get_dmabuf_from_userptr()` has its VMA lookup implementation compiled out with `#if 0`, so it returns `NULL`. `jsqz_buffer_get_and_attach()` then rejects the missing DMA-BUF with `-EINVAL`.

The target disassembly confirms that `jsqz_process` sends type 1 through `dma_buf_get(fd)`, while type 2 takes the error path before `jsqz_buffer_map()`. A lower-level helper, `jsqz_dma_addr_map()`, does contain a generic `exynos_iovmm_map_userptr()` branch, but the normal JSQZ USERPTR descriptor path fails earlier and does not reach it. That helper's presence must not be mistaken for working USERPTR support.

### YUV422 appears broken in the target driver

Although the UAPI declares YUV422, the target disassembly appears to calculate the V-plane address incorrectly in both one-buffer and two-buffer cases. At `ffffff8008b0a120` and `ffffff8008b0a528`, the code executes `lsr w2, w2, w3` with `w2 = width + 1` and `w3 = height`. This comes from the source expression `((width + 1) >> 1 * height)`, where C precedence makes the shift count `height), rather than computing `((width + 1) / 2) * height`.

For 128×64, the AArch64 W-register variable shift count is 64 modulo 32 = 0, so the calculated V-plane offset is only 129 bytes. A planar YUV422 U plane should occupy 64 × 64 = 4,096 bytes before V starts. The resulting address would overlap the U and V planes. See Arm's [LSRV instruction reference](https://documentation-service.arm.com/static/6245c734b059dc5ff9a8bdab) for the modulo-32 shift semantics.

This is a static analysis result only: no YUV422 test was run. The fourteen NV21 tests use `cs=0`, where U and V intentionally point to the same interleaved chroma base, so they are unaffected. YUV422 should remain marked unvalidated and should not be used for further experiments until this driver defect is addressed.

### Unexercised operating modes

The public UAPI also defines `JSQZ_MODE_HIGH_QUALITY` and `JSQZ_FUNCTION_TRANSCODE`. The header describes LIVE as using a camera preview and TRANSCODE as using the original image. The driver packs the mode and function selectors into bits 29 and 28 of the input-format register word. All fourteen submissions used NORMAL + LIVE; the other combinations have not been characterised. Their existence is established, but their output differences are unknown. See the [public UAPI header](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/include/uapi/linux/hwjsqz.h).

### Register and completion path

The recovered register banks are:

- Input addresses and format: `0x000–0x00C`
- Luma output matrix: `0x010–0x04C`
- Chroma output matrix: `0x050–0x08C`
- Software reset: `0x090`
- Interrupt enable: `0x094`; interrupt clear/status: `0x098`
- Hardware start: `0x09C`
- Hardware done: `0x100`
- Bus configuration/status: `0x108`
- Initial Y/C table registers: `0x10C–0x188`

The worker resets the IP, programs the Y/U/V input addresses and packed format word, enables and clears the interrupt, starts the engine, waits for completion, then reads the two output banks unless the timeout/fallback flag is set.

One qualification matters: the IRQ handler reads the interrupt status, but the check that would reject a zero/invalid status is commented out in the reference source. The handler clears the interrupt and signals completion without using that status value to decide whether the IRQ was valid. The scheduler also uses unbounded `wait_for_completion()`; if no completion interrupt arrives, the ioctl caller can remain blocked.

## Fallback analysis

The target contains two documented default-table paths:

1. An early software fallback for invalid image stride/alignment.
2. A post-completion fallback when bit 28 of bus-config register `0x108` is set.

The exact 32-word fallback table was recovered from the original target `kernel.bin`, at file offset `0x1274cd8`, with exactly one signature match. It matches the public driver table 32/32 words.

The tested frame's effective stride is 128 bytes, which satisfies the NV21 alignment guard. Every recorded output also differs from the recovered fallback table. Under the recovered target control flow, the recorded runs therefore bypassed both documented fallback paths and took the output-register read branch.

The ISR qualification above still applies: taking the output-register read branch is not, by itself, a proof that every interrupt was a genuine device-completion IRQ. The content-dependent table changes and the exact repeated-grey match provide much stronger evidence of a functioning, repeatable hardware response than ioctl success alone.

## Runtime evidence — fourteen submissions

Fourteen native ioctl submissions were recorded: thirteen distinct patterns and one repeat of the grey baseline. All used the same 128 × 64 NV21 frame geometry, one system-heap DMA-BUF, NORMAL/LIVE mode and effective stride 128.

All byte counts below compare the returned 128-byte table with the grey baseline (Table A). Y and C are separate 64-byte banks.

| Input | Y bytes changed | C bytes changed | Result |
|---|---:|---:|---|
| Grey baseline (Table A) | 0/64 | 0/64 | Reference table for all comparisons |
| Grey repeat | 0/64 | 0/64 | Exact 128/128-byte repeat |
| 8×8 luma checker | 0/64 | 0/64 | Identical to grey; each 8×8 tile is internally flat |
| Black (Y=16) | 29/64 | 15/64 | Strong change in both banks |
| Horizontal luma gradient | 1/64 | 0/64 | Small Y-only change |
| Deterministic luma noise | 50/64 | 0/64 | Strong Y-only change; all changed Y bytes increase |
| Constant non-neutral chroma | 0/64 | 0/64 | No measurable response to this constant offset |
| Spatial chroma checker, ±64 | 0/64 | 4/64 | C-only response |
| Spatial chroma checker, ±32 | 0/64 | 1/64 | C-only response |
| Spatial chroma checker, ±16 | 0/64 | 1/64 | Same quantised output as ±32 |
| Spatial chroma checker, ±8 | 0/64 | 1/64 | Same quantised output as ±16 and ±32 |
| Spatial chroma checker, ±64, finer tiles | 0/64 | 4/64 | Exact match to the ±64 larger-tile result |
| 4×4 luma checker, ±64 | 11/64 | 0/64 | Y-only response to within-8×8-block variation |
| 4×4 luma checker, ±16 | 6/64 | 0/64 | Weaker Y-only response; all six offsets also change at ±64 |

### Byte-level observations

- The gradient changes Y-bank linear byte offset 1 only.
- Deterministic noise changes 50 Y bytes and no C bytes.
- The ±64 chroma checker changes C-bank offsets 4, 51, 58 and 60, with no Y changes.
- The ±32, ±16 and ±8 chroma tests each change only C-bank offset 4. No transition below ±8 was measured.
- The 4×4 luma checker at ±16 changes Y offsets 4, 11, 13, 22, 24 and 42. At ±64, those six plus offsets 38, 51, 53, 58 and 60 change.
- The exact repeated-grey result establishes repeatability for that input and configuration only.

These findings support separate, content-dependent Y- and C-table responses for the tested stimuli. They do not establish universal contrast thresholds or general spatial-frequency invariance. The ±64 chroma tile-size comparison is one matched comparison, not a general result for all scales.

## What remains unknown

The Linux driver orchestrates the IP but does not contain its internal estimator. The current evidence does not establish:

- the mathematical/statistical estimator used to derive the Q tables;
- the precise contrast or noise thresholds across arbitrary patterns;
- whether output bytes are ordered in JPEG zigzag order, raster order or another internal arrangement;
- the mapping from each linear output byte to an 8×8 coefficient position;
- behaviour at other resolutions, strides, formats, modes or processing functions;
- whether the installed camera pipeline actually uses JSQZ for a particular capture.

The available reference HAL's byte-unpacking method writes bytes low-to-high from each 32-bit word, but that does not reveal the 8×8 coefficient scan order. Its Build ID differs from the installed target HAL. Separately, the supplied 10× Pro capture log showed the camera HAL failing to open `/dev/jsqz` with permission error 13; the JPEG capture still completed. That log does not show JSQZ contributing to that particular photo.

## Public patent search — a lead, not an attribution

A Samsung patent with a potentially related concept is [US20200145662A1, “Method and apparatus for encoding image by using quantization table adaptive to image”](https://patents.google.com/patent/US20200145662A1/en) (priority date 1 November 2018). It describes generating an image-adaptive quantisation table from multiple image patches and transform-coefficient groups using a machine-learning model. One embodiment uses fully connected layers; another describes a convolutional neural network. The model inputs can include transform coefficients or location-wise statistics derived from them.

This has a conceptual overlap with JSQZ's raw-image-to-Q-table contract, but **the patent does not mention Exynos 9820 or JSQZ**, and no source, binary string, register description or HAL call currently links it to this hardware. It could describe a separate Samsung implementation or research direction. It is not evidence that the JSQZ engine contains a neural network.

[US20140146881A1](https://patents.google.com/patent/US20140146881A1/en), a Samsung/KAIST quantisation-table estimation patent, appears less relevant because its described process estimates tables for already-compressed video blocks using candidate-table tests, edge-preserving filtering and energy costs. That is materially different from JSQZ receiving raw NV21 and returning a table for later JPEG encoding.

The remaining public-research target is a disclosure explicitly traceable to the Exynos JSQZ IP, or an implementation artifact that connects an algorithm to this block. Until then, the estimator itself remains unknown.
 
## Camera HAL integration and access control

A separate follow-up found a likely explanation for the target camera HAL's `/dev/jsqz` open failure. The target node was observed as `0600 root:root`; the JSQZ misc-device driver leaves its mode unset, and devtmpfs defaults an unset device-node mode to `0600`. The Exynos 9820 common device tree contains the corresponding access configuration: ueventd sets `/dev/jsqz` to `0660 cameraserver:camera`, SELinux labels it `jsqz_device`, and `hal_camera_default` is granted read/write access. This supports a device-node access-control failure as the reason the camera path cannot open JSQZ; it does not yet exclude an additional SELinux denial. No device permissions or policy were changed.

Detailed evidence, source links, build caveats and remaining verification are in [JSQZ camera integration analysis](JSQZ-CAMERA-INTEGRATION.md). A [read-only access audit script](jsqz_camera_access_audit.sh) is also included for one focused follow-up, if needed.

The public reference JSQZ HAL also shows an `openNode()` path using `/dev/jsqz` with `O_RDWR`, followed by a failure check, and an `execute()` path that builds compat ioctl `0xC1344D00`. The earlier camera-HAL scanner's zero direct-call hits are inconclusive: it did not detect GOT/PLT-loaded or vtable-mediated indirect calls. See [the detailed camera integration analysis](JSQZ-CAMERA-INTEGRATION.md) for the evidence and limitations.

## Final status

The planned bounded sequence is complete. The driver/API path, fallback table, output-register branch, and principal observable responses to the tested luma/chroma patterns are documented. No further phone-side submission is justified merely to extend the count.

Any deeper work should be driven by a specific unresolved question. Reconstructing the proprietary estimator or RTL would require Samsung design documentation, hardware design evidence or a substantially different reverse-engineering avenue; further arbitrary black-box patterns cannot guarantee that result.

## Reproducibility files

- [Detailed chronological driver analysis](JSQZ-DRIVER-ANALYSIS.md)
- [Native probe source](jsqz_probe.c)
- [Q-table byte/word diff utility](jsqz_qtable_diff.py)
- [Target driver disassembly](jsqz-driver-disassembly.txt)
- [Exact fallback table extraction](jsqz-default-table.txt)
- [GCC and Clang probe-check workflow](../.github/workflows/jsqz-probe-check.yml)
