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

## 4. Interpretation and limits

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

## 5. Next useful verification

The remaining device-side uncertainty is narrow: inspect the active node's SELinux label and the live camera service domain, then check whether an AVC record exists for the failed open. That distinguishes a missing DAC rule from an additional SELinux denial. It does not require another JSQZ ioctl or another synthetic frame.

Until that is checked, the most accurate conclusion is: **the camera HAL's JSQZ open is being blocked at the device-node access layer; the recorded root:root 0600 mode is already sufficient to explain the failure.**
