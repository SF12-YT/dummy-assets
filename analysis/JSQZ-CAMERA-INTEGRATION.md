# Exynos 9820 JSQZ — camera HAL integration and access-control analysis

**As of:** 15 November 2026  
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

The user's capture log establishes that the installed camera path tried to open `/dev/jsqz` and that the open failed before a JSQZ task could be submitted for that capture. The fact that JPEG capture still completed means the camera stack can continue without a successful JSQZ open; the exact fallback implementation in the installed, build-mismatched camera HAL has not yet been reconstructed.

A public Beyond0 reference camera HAL imports the `ExynosJsqz` API methods, including buffer setup, image configuration, `run()` and `getResultQT()`, and contains the Samsung hardware-JQ log strings. Its Build ID differs from the installed target camera HAL, so it establishes a reference implementation, not exact control flow for the target binary.

The corresponding public `libhwjsqz.so` reference is 32-bit ARM. Its disassembly exposes an `openNode()` path using read/write access and an `execute()` path submitting the 32-bit compat request `0xC1344D00`. The same library includes the `/dev/jsqz` string and an explicit diagnostic for device-node open failure. This supports the expected userspace interface but does not replace analysis of the installed binary.

## 4. What the reference JSQZ HAL does at the node boundary

The public 32-bit ARM `libhwjsqz.so` reference has an `ExynosSqzBase::openNode()` method. Its disassembly:

- loads the `/dev/jsqz` path from a PC-relative literal;
- sets the second argument to `2` (`O_RDWR`) and the mode argument to zero;
- calls through a PLT/thunk target consistent with its imported libc `open()`;
- stores the returned descriptor and explicitly compares it with `-1` before taking the failure path.

The library's strings include the diagnostic `JSQZ device node (%s) opening failed with error %d: %s`, matching the kind of failure shown in the user's camera capture log. Its `ExynosSqzBase::execute()` code constructs ioctl request `0xC1344D00`, the 32-bit compat ioctl confirmed by the target kernel and the library's 308-byte ABI.

This makes the intended transaction sequence concrete in the reference implementation: open the node read/write, then submit the compat ioctl. The user's recorded EACCES occurs at the first step, so no JSQZ task would be submitted by that failed open attempt.

The reference HAL's Build ID is still different from the installed target library. These method-level findings corroborate the API shape and failure point, but they are not exact disassembly of the installed target build.

## 5. Why the previous zero-call-site scan is inconclusive

The earlier reference-camera-HAL scan looked only for direct `bl`/`blx` instruction lines that literally contained a demangled `ExynosJsqz::...` name. It reported zero matches. That is not a reliable proof that no call exists: ARM32 position-independent code can load an external function address through the GOT/PLT and branch indirectly via a register; virtual dispatch can also use vtable-loaded function pointers. Those call instructions do not contain the target C++ symbol name as text.

The reference camera HAL's dynamic symbol table does contain undefined imports for the JSQZ methods, and the strings include Jpeg Squeezer log messages and Q-table forwarding diagnostics. That establishes a link-time dependency and integration code, not the reachability of every path in a particular capture. A stronger call graph would require a relocation-aware PLT/GOT scan and analysis of the actual installed camera HAL binary.

## 6. Interpretation and limits

**What is now strongly supported:**

1. The hardware block and kernel driver can run when a privileged caller opens the node and submits a valid DMA-BUF task.
2. The normal camera path attempts to access the JSQZ node.
3. The target node's recorded `0600 root:root` mode is sufficient by itself to deny access to a non-root camera process.
4. The Exynos 9820 common device tree explicitly solves this with a ueventd `0660 cameraserver:camera` rule and matching SELinux policy.

**What is not yet established:**

- Whether the installed ROM has no matching ueventd rule, or whether another startup action later resets the mode.
- The exact SELinux context of the live node on the target device.
- Whether SELinux also denies the access (no relevant AVC record has been captured).
- The exact target HAL fallback branch after `open()` fails.
- Whether the installed target HAL and the public Beyond0 reference HAL are functionally identical.

No permissions, ownership, or SELinux policy have been changed on the user's device. A blanket `chmod 666`, running the camera service as root, or setting SELinux permissive is not an appropriate diagnostic fix. A ROM-level integration should use the narrowly scoped ueventd rule and SELinux type/allow rule shown above.

A ready-to-run, read-only collector is available at [`jsqz_camera_access_audit.sh`](jsqz_camera_access_audit.sh). It gathers the live node mode/SELinux label, active ueventd rule locations, camera process UID/GID/groups and SELinux domains, plus matching AVC/kernel log lines. It does not open the JSQZ device or submit an ioctl, and it does not change permissions or policy. It is only needed if the live-device ambiguity is worth checking later.

## 7. Next useful verification

The remaining device-side uncertainty is narrow: inspect the active node's SELinux label and the live camera service domain, then check whether an AVC record exists for the failed open. That distinguishes a missing DAC rule from an additional SELinux denial. It does not require another JSQZ ioctl or another synthetic frame.

Until that is checked, the most accurate conclusion is: **the camera HAL's JSQZ open is being blocked at the device-node access layer; the recorded root:root 0600 mode is already sufficient to explain the failure.**
