# Exynos 9820 JSQZ — camera HAL integration and access-control analysis

**As of:** 16 November 2026  
**Target:** Samsung Galaxy S10e SM-G970F (beyond0lte), Exynos 9820  
**Question:** Why does the camera HAL report `/dev/jsqz` open failure with error 13 when a root-run userspace probe can use the same device?

## Executive conclusion

The strongest explanation is a device-node access-control mismatch, not a failure of the JSQZ hardware block.

The target device observation recorded earlier was:

- `/dev/jsqz` existed as a character device with mode `0600`, owner `root:root`.
- The camera capture log reported an attempt to open `/dev/jsqz` failing with error 13 (`EACCES` / Permission denied).
- The JPEG capture nevertheless completed.
- The native JSQZ probe succeeded when run through `su`, which runs with root privileges.

These observations fit the kernel/device-tree implementation below: the JSQZ driver does not supply a device-node mode, so Linux devtmpfs creates the node as `0600 root:root` by default. A device-specific ueventd rule is required to grant the camera service access.

This is a high-confidence explanation for the observed DAC (Unix permission) failure. It does **not** rule out a second SELinux denial, because no matching AVC denial or target `ls -Z` output has been recorded.

## 1. Why the kernel creates a root-only node by default

In the Exynos 9820 JSQZ driver, the device structure is allocated with `devm_kzalloc`. Probe then sets these misc-device members:

- `minor = MISC_DYNAMIC_MINOR`
- `name = NODE_NAME`
- `fops = &jsqz_fops`
- `parent = &pdev->dev`

It does not assign `misc.mode`.

The matching kernel's `drivers/char/misc.c` `misc_devnode()` callback only writes a requested mode when `misc->mode` is non-zero. The matching `drivers/base/devtmpfs.c` implementation then applies the default:

```c
if (req.mode == 0)
    req.mode = 0600;
```

It also initializes the node owner and group to root before asking the class for any overrides. Therefore, with no userspace ueventd permission rule, a node shown as `crw------- root root` is the expected result.

Source references (pinned Exynos 9820 kernel tree):
- [JSQZ driver registration](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/media/platform/exynos/jsqz/jsqz-core.c)
- [misc-device node-mode handling](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/char/misc.c)
- [devtmpfs default mode](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/base/devtmpfs.c)

## 2. The S10e Exynos common device tree contains the missing access configuration

The LineageOS Exynos 9820 common device configuration has a complete three-part rule set for JSQZ:

### DAC permissions — ueventd

`configs/init/ueventd.rc` contains:

```
/dev/jsqz  0660  cameraserver  camera
```

The common product configuration copies this file to `/vendor/etc/ueventd.rc`. This changes the node from the kernel default to owner/group access for the camera service.

Source:
- [ueventd rule](https://github.com/LineageOS/android_device_samsung_exynos9820-common/blob/38af956a4eec889cb352b4fd42e5e801c19a51b8/configs/init/ueventd.rc)
- [common product copies ueventd.rc into vendor](https://github.com/LineageOS/android_device_samsung_exynos9820-common/blob/38af956a4eec889cb352b4fd42e5e801c19a51b8/common.mk)

### SELinux type and path label

The device policy declares:

```
type jsqz_device, dev_type;
```

The file contexts assign:

```
/dev/jsqz  u:object_r:jsqz_device:s0
```

Sources:
- [device type declaration](https://github.com/LineageOS/android_device_samsung_exynos9820-common/blob/38af956a4eec889cb352b4fd42e5e801c19a51b8/sepolicy/vendor/device.te)
- [JSQZ device label](https://github.com/LineageOS/android_device_samsung_exynos9820-common/blob/38af956a4eec889cb352b4fd42e5e801c19a51b8/sepolicy/vendor/file_contexts)

### SELinux access grant

The camera HAL policy contains:

```
allow hal_camera_default jsqz_device:chr_file rw_file_perms;
```

The common board configuration includes the vendor device-policy directory in the build.

Sources:
- [camera HAL JSQZ access rule](https://github.com/LineageOS/android_device_samsung_exynos9820-common/blob/38af956a4eec889cb352b4fd42e5e801c19a51b8/sepolicy/vendor/hal_camera_default.te)
- [vendor SELinux policy inclusion](https://github.com/LineageOS/android_device_samsung_exynos9820-common/blob/38af956a4eec889cb352b4fd42e5e801c19a51b8/BoardConfigCommon.mk)

This device-tree configuration is particularly relevant because the S10e product inherits the Exynos 9820 common configuration. It is an implementation reference for a supported S10e software build, not proof that the user's installed Samsung firmware includes the same files or policies.

## 3. What the HAL evidence proves

The user's earlier EACCES log established that an earlier camera attempt could not open `/dev/jsqz`. The exact target binaries have now been extracted and analysed, so we can distinguish the intended code path from whether a particular capture actually reached it.

The installed `libexynoscamera3.so` depends on the installed `libhwjsqz.so` and imports the JSQZ constructor, create/destroy, buffer setup, image-info, configuration, run and Q-table retrieval methods. Its `m_jpegSqueezerThreadFunc()` contains direct calls to the JSQZ API and then stores the returned Q-table in camera configuration.

The exact target `libhwjsqz.so` opens `/dev/jsqz` read/write from `ExynosSqzBase::openNode()`. Its `ExynosJsqz::run()` dispatches to `ExynosSqzBase::execute()`, which submits `0xC1344D00` through libc `ioctl()` after checking the instance and input buffers. These are findings from the exact installed target build, not the public comparison binary.

## 4. Exact target library node and ioctl implementation

The exact installed `libhwjsqz.so` is ELF32 ARM, Build ID `2954034e91ab9505aef839b7d9feded0`. Its `ExynosSqzBase::openNode()` at `0x5428`:

- loads the embedded `/dev/jsqz` path;
- sets `r1 = 2` (`O_RDWR`) and `r2 = 0`;
- calls the PLT entry resolving to `open@LIBC`;
- stores the descriptor at object offset `+8` and takes the error path when the result is negative.

`ExynosSqzBase::create()` calls `openNode()` and only marks the instance initialised after a successful return. `ExynosJsqz::create()` dispatches to this base implementation.

The exact `ExynosSqzBase::execute()` at `0x517c` validates the initialised state and input buffer(s), loads the descriptor from object offset `+8`, constructs `0xC1344D00`, and calls `ioctl@LIBC` with a task structure at `this + 12`. `ExynosJsqz::run()` dispatches to this execute method. The separate `execute_msqz()` builds `0xC03C4D01` with a task structure at `this + 0x140`.

Thus the exact target transaction is:

```c
fd = open("/dev/jsqz", O_RDWR, 0);
ioctl(fd, 0xC1344D00, task);
```

The code proves what the target library does when those methods are reached. It does not by itself prove that a particular Camera2 capture reached the ioctl instruction.

## 5. Exact target camera HAL call path

The exact installed `libexynoscamera3.so` is ELF32 ARM, Build ID `1b7a6dfaf4dcbf7034b467a6fcec2fc1`. It has a dynamic dependency on `libhwjsqz.so` and imports all methods used by the worker.

Its `android::ExynosCamera::m_jpegSqueezerThreadFunc()` at `0x136dac` has these calls, resolved against the target camera HAL's own `R_ARM_JUMP_SLOT` relocation table and ARM32 PLT layout:

```asm
0x136ee0  blx 0x199d00   ; ExynosJsqz::setInBuf(...)
0x136ef0  blx 0x199d10   ; ExynosJsqz::setImageInfo(...)
0x136efa  blx 0x199d20   ; ExynosJsqz::setJsqzConfig(...)
0x136f00  blx 0x199d30   ; ExynosJsqz::run()
0x136f0a  blx 0x199d40   ; ExynosJsqz::getResultQT(...)
```

After retrieving the table, the same function calls `ExynosCameraConfigurations::setJpegQtable()` and sets Q-table status to 1. This is direct target-binary evidence of a real JPEG-squeezer code path, not just a linked dependency or a lifecycle log.

The worker has early-return/error branches. It checks that a frame is queued, that capture setup succeeds, that the input format is one of its supported FourCC values (`NV21` or `NM21`), and that its size/count values are positive before entering the JSQZ call sequence. In addition, `ExynosSqzBase::execute()` can return before the ioctl if its instance or buffer checks fail.

The earlier text-only call-site scan that reported zero calls was a PLT-symbol annotation false negative. The relocation-aware mapping above resolves the actual call instructions correctly.

## 6. Interpretation and limits

**What is now strongly supported:**

1. The hardware block and kernel driver can run when a privileged caller opens the node and submits a valid DMA-BUF task.
2. The exact target JSQZ library opens `/dev/jsqz` read/write from its create path and submits `0xC1344D00` from its execute path.
3. The exact target camera HAL has a JPEG-squeezer worker that calls `setInBuf()`, `setImageInfo()`, `setJsqzConfig()`, `run()` and `getResultQT()`, then stores the returned Q-table.
4. The target node's recorded `0600 root:root` mode is sufficient by itself to deny access to the non-root camera HAL; the temporary `0660 1047:1006` correction is compatible with its UID/GID and the loaded SELinux allow rule.
5. The Exynos 9820 common device tree explicitly solves the DAC issue with a ueventd `0660 cameraserver:camera` rule and matching SELinux policy.

**What is not yet established:**

- Whether the installed ROM has no matching ueventd rule, or whether another startup action later resets the mode.
- Whether the JSQZ worker reached its JSQZ call sequence during the user's specific Camera2 capture.
- Whether the target HAL's create path successfully opened the node and held a valid descriptor during that capture.
- Why the captured provider syscall trace did not contain a known JSQZ ioctl despite the static route being present.

No permissions, ownership, or SELinux policy have been changed on the user's device. A blanket `chmod 666`, running the camera service as root, or setting SELinux permissive is not an appropriate diagnostic fix. A ROM-level integration should use the narrowly scoped ueventd rule and SELinux type/allow rule shown above.

A ready-to-run, read-only collector is available at [`jsqz_camera_access_audit.sh`](jsqz_camera_access_audit.sh). It gathers the live node mode/SELinux label, active ueventd rule locations, camera process UID/GID/groups and SELinux domains, plus matching AVC/kernel log lines. It does not open the JSQZ device or submit an ioctl, and it does not change permissions or policy. It is only needed if the live-device ambiguity is worth checking later.

## 7. Next useful verification

The device label, camera HAL domain, loaded JSQZ allow rule and absence of a matching AVC have since been checked. The remaining uncertainty is runtime control flow: whether the capture queued a frame that passed the squeezer worker's format/setup checks, and whether the JSQZ instance opened the node successfully.

The static call path is now understood from the exact target binaries. Do not repeat broad syscall captures or synthetic hardware ioctl tests just to reconfirm the method implementation. A future runtime test should be narrowly designed to distinguish the create/open stage from the worker/run stage, without changing firmware or kernel state.


## 8. Live permission test and HAL lifecycle follow-up — 15 November 2026

After the read-only audit, the device node was changed temporarily from `0600 root:root` to:

```
crw-rw---- 1 cameraserver camera u:object_r:jsqz_device:s0 /dev/jsqz
```

The numeric owner/group were UID 1047 and GID 1006. The active camera-provider process ran as UID 1047, supplementary groups included 1006, and its SELinux domain was `u:r:hal_camera_default:s0`. The loaded policy explicitly allowed that domain to open/read/write/ioctl the `jsqz_device:chr_file`. No matching AVC record was found in the available log buffers.

The user then took a photo and recorded a video; normal camera operation continued. A subsequent read-only audit still showed mode `0660`, owner `1047`, group `1006`, and the expected SELinux label.

During camera use, the user captured:

```
11-15 21:05:05.712  5446 16507 I jsqz_hal: jsqz instance is created
11-15 21:05:07.069  5446  6045 I jsqz_hal: jsqz instance is destroyed
11-15 21:07:54.984  5446  6045 I jsqz_hal: jsqz instance is created
11-15 21:08:42.006  5446  6045 I jsqz_hal: jsqz instance is destroyed
```

This is evidence that the JSQZ wrapper lifecycle is now being entered. The old open-failure message was not present in this capture. However, the create/destroy messages alone do **not** prove that `open("/dev/jsqz", O_RDWR)` succeeded or that ioctl `0xC1344D00` was submitted. Exact target binaries were subsequently disassembled on 16 November; see sections 11 and 12 below. The static call path exists, but the earlier runtime syscall trace still did not show the expected ioctl.

A stored earlier camera log (`mfp_10x_live.log`, 14 November) contained repeated errors from JSQZ wrapper calls stating that the device had to be created first. The 15 November lifecycle messages are a material change in the observed log output, but they still do not reveal the final ioctl result.

## 9. Persistent permission correction: safest candidate

The temporary correction was tested on the running device, and the camera continued to operate. The persistent candidate is therefore to apply the exact same narrow change once per boot, without replacing vendor files or changing SELinux policy:

```sh
chown 1047:1006 /dev/jsqz
chmod 0660 /dev/jsqz
```

The best Magisk hook candidate is a module-local `post-fs-data.sh`, not `service.sh`:

- Magisk documents `post-fs-data.sh` as post-fs-data mode and `service.sh` as late_start service mode.
- Magisk documents post-fs-data scripts as running before Zygote, with a blocking stage limited to 10 seconds.
- Android 12 AOSP init triggers `post-fs-data` before `zygote-start`; the standard `class_start hal` is in the later `on boot` action. Thus this hook should correct the node before standard HAL services start.
- ueventd is started during early-init and completes coldboot before the later init stages, so the already-registered `/dev/jsqz` node should exist by post-fs-data.

References:
- [Magisk module structure and script stages](https://magisk.readthedocs.io/en/latest/developers/guides.html)
- [Magisk boot process](https://topjohnwu.github.io/Magisk/details.html)
- [Android 12 AOSP init sequence](https://android.googlesource.com/platform/system/core/+/refs/heads/android12-dev/rootdir/init.rc)

A minimal module would contain only `module.prop`, an empty `skip_mount` marker, and a guarded `post-fs-data.sh`. The script should verify that `/dev/jsqz` is a character device, apply owner/group and mode, verify the result, and exit promptly. It should not contain a `system/` overlay, `overlay.d` init replacement, `sepolicy.rule`, `chcon`, service restart, or any driver/HAL modification because none is currently justified.

This is a **well-supported candidate, not yet installed or reboot-tested**. A device-specific init action could theoretically start the camera provider earlier than the standard class sequence, and a Magisk script is not a substitute for the proper ROM-level ueventd rule. No persistent changes have been made. The user has no PC available for stock-firmware recovery, so the module must remain optional until its exact contents and rollback plan are reviewed.

## 10. Remaining verification

The current best status is:

1. **DAC failure identified and manually corrected:** confirmed.
2. **SELinux policy and label:** consistent with access; no AVC observed.
3. **Camera still works with the corrected node:** confirmed by the user.
4. **JSQZ instance lifecycle during camera use:** confirmed by logcat.
5. **Target HAL's intended open and ioctl implementation:** confirmed from exact binaries. **Successful open and ioctl during the user's particular Camera2 capture:** not yet directly confirmed.
6. **Reboot-persistent permission change:** candidate designed from boot ordering, but not installed or tested.

The least intrusive next verification of a successful open is to inspect the camera-provider process's open file descriptors while the JSQZ instance is active and look for `/dev/jsqz`. If the file descriptor is opened and retained during that lifetime, this proves the node open succeeded. A missing descriptor is inconclusive because the HAL may open/close it quickly. Direct ioctl confirmation would require tracing the provider's ioctl syscall or a target HAL instrumentation point; attaching a tracer can perturb the camera service and is not the first choice on this no-PC recovery situation.


## 11. Exact installed target binary analysis — 16 November 2026

The user uploaded the device's actual `libhwjsqz.so` and `libexynoscamera3.so` files to the repository. Their ELF Build IDs and SHA-256 hashes match the versions collected from the installed `/vendor/lib` directory:

| Library | Build ID | SHA-256 |
|---|---|---|
| `libhwjsqz.so` | `2954034e91ab9505aef839b7d9feded0` | `63936c0cf08afdf919d6a0a6e38e3887c69a4b8c8b81a7abca5e9b1178a6f1fd` |
| `libexynoscamera3.so` | `1b7a6dfaf4dcbf7034b467a6fcec2fc1` | `ec9a8f366cefbd4af14d84ec918c13949db5ec4d7c731be8bb72a50951715c0d` |

This is no longer reference-build analysis. The exact Samsung target binaries are available in the repository root, and the disassembly reports are stored under `analysis/target-*.txt`.

### Camera worker's JSQZ sequence

The exact target camera HAL's `m_jpegSqueezerThreadFunc()` calls the JSQZ methods in order: `setInBuf()`, `setImageInfo()`, `setJsqzConfig()`, `run()`, and `getResultQT()`. It then stores the returned Q-table with `ExynosCameraConfigurations::setJpegQtable()` and sets the Q-table status to 1.

The call sites are at `0x136ee0`, `0x136ef0`, `0x136efa`, `0x136f00`, and `0x136f0a`. They resolve through the camera HAL's own ARM32 PLT slots to the imported `ExynosJsqz` methods. The worker only reaches this block after its frame/setup/format/size checks; it is not executed unconditionally for every JPEG request.

### Runtime discrepancy remains

The Camera2 probe's logcat showed the JSQZ object lifecycle and Samsung hardware-JQ Q-table initialisation/deinitialisation logs. However, the existing syscall trace found no `0xC1344D00` or `0xC03C4D01` ioctl. These facts are compatible: the object and worker can be created while a particular frame still takes an early/error branch, or while the instance is not successfully initialised. The trace does not establish which condition occurred.

The strongest current conclusion is therefore:

- **Static integration:** confirmed. Samsung's installed camera HAL contains a real code path that calls the JSQZ library, and that library opens `/dev/jsqz` and issues the JSQZ ioctl when its validation passes.
- **Observed use in that particular Camera2 capture:** not yet confirmed. No JSQZ ioctl was seen in the captured runtime trace.
- **Stock/non-root public APK:** cannot directly access or repair permissions on `/dev/jsqz`; it can only ask the OEM Camera2 pipeline to perform a capture. Whether JSQZ actually runs depends on the OEM HAL's conditions and node access configuration.

See the detailed address-by-address evidence in [`target-jsqz-confirmed-callflow.md`](target-jsqz-confirmed-callflow.md).
