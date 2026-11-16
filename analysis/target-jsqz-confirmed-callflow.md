# Exact installed Samsung JSQZ call path

**Analysis date:** 16 November 2026  
**Device build:** Samsung SM-G970F / Exynos 9820 / Android 12  
**Target binaries:** extracted from the device and uploaded to this repository.

## Binary identity

| Binary | SHA-256 | ELF Build ID |
|---|---|---|
| `libhwjsqz.so` | `63936c0cf08afdf919d6a0a6e38e3887c69a4b8c8b81a7abca5e9b1178a6f1fd` | `2954034e91ab9505aef839b7d9feded0` |
| `libexynoscamera3.so` | `ec9a8f366cefbd4af14d84ec918c13949db5ec4d7c731be8bb72a50951715c0d` | `1b7a6dfaf4dcbf7034b467a6fcec2fc1` |

The Build IDs match the values collected from the installed `/vendor/lib` files. The disassembly below is therefore of the target firmware's binaries, not a public comparison build.

## 1. Target JSQZ library opens the node

In `libhwjsqz.so`:

- `ExynosJsqz::create()` at `0x2e7c` branches through a linker veneer to `ExynosSqzBase::create()` at `0x4e20`.
- `ExynosSqzBase::create()` calls `ExynosSqzBase::openNode()` through its PLT entry. It only marks the instance initialised after the open helper returns success.
- `ExynosSqzBase::openNode()` at `0x5428` loads the embedded `/dev/jsqz` string, sets `r1 = 2` (`O_RDWR`) and `r2 = 0), then calls the PLT entry resolving to `open@LIBC`.
- The descriptor is stored at object offset `+8`. A negative open result is reset to `-1` and takes the error path.

This establishes the exact target call as:

```c
open("/dev/jsqz", O_RDWR, 0);
```

## 2. Target JSQZ library submits the hardware task

`ExynosJsqz::run()` at `0x3044` branches through a linker veneer to the PLT entry for `ExynosSqzBase::execute()`.

In `ExynosSqzBase::execute()` at `0x517c`, the method:

1. checks the instance-initialised flag;
2. validates the configured input buffer(s);
3. loads the node descriptor from object offset `+8`;
4. constructs request `0xC1344D00`;
5. passes the task structure at `this + 12` to the PLT entry resolving to `ioctl@LIBC`.

The call is equivalent to:

```c
ioctl(fd, 0xC1344D00, task);
```

The separate `ExynosSqzBase::execute_msqz()` method constructs `0xC03C4D01` and passes `this + 0x140`. The camera JPEG path traced below calls `ExynosJsqz::run()`, not the MSQZ method.

## 3. The exact camera HAL links to, and calls, the exact JSQZ library

The target `libexynoscamera3.so`:

- has a dynamic dependency on `libhwjsqz.so`;
- has undefined imports for the `ExynosJsqz` constructor, `create()`, `destroy()`, `setInBuf()`, `setImageInfo()`, `setJsqzConfig()`, `run()`, and `getResultQT()`;
- exports `android::ExynosCamera::m_jpegSqueezerThreadFunc()` at `0x136dac`.

The dynamic relocation table and ARM32 PLT layout resolve the relevant JSQZ entries as follows:

| Camera HAL PLT address | Import resolved by the matching JUMP_SLOT |
|---:|---|
| `0x199cb0` | `ExynosJsqz::ExynosJsqz()` |
| `0x199cc0` | `ExynosJsqz::create()` |
| `0x199ce0` | `ExynosJsqz::destroy()` |
| `0x199d00` | `ExynosJsqz::setInBuf(int*, unsigned int*, int)` |
| `0x199d10` | `ExynosJsqz::setImageInfo(...)` |
| `0x199d20` | `ExynosJsqz::setJsqzConfig(...)` |
| `0x199d30` | `ExynosJsqz::run()` |
| `0x199d40` | `ExynosJsqz::getResultQT(unsigned char*)` |

The table is calculated from the camera HAL's own `.plt` base (`0x199260`) and its `R_ARM_JUMP_SLOT` relocation order; the imported symbols are independently visible in the target's dynamic symbol and relocation tables.

Within `m_jpegSqueezerThreadFunc()`, these five instructions call the JSQZ API in order:

```asm
0x136ee0  blx 0x199d00   ; setInBuf
0x136ef0  blx 0x199d10   ; setImageInfo
0x136efa  blx 0x199d20   ; setJsqzConfig
0x136f00  blx 0x199d30   ; run
0x136f0a  blx 0x199d40   ; getResultQT
```

Immediately afterwards, the same worker passes the returned 128-byte table to `ExynosCameraConfigurations::setJpegQtable()` at `0x136f12`, then sets the Q-table status to 1 at `0x136f1a`.

This is direct machine-code evidence of a real Samsung camera-HAL-to-JSQZ hardware-Q-table processing path in the installed target build. It is not merely an object-lifecycle log or a dependency that happens to be linked.

## 4. Conditions for reaching the JSQZ calls

The worker does not call the JSQZ API unconditionally. The disassembly shows early/error branches before the five calls. In summary, it requires a queued capture frame and successful setup, then validates the input image format and size/count fields.

In particular, the format comparisons use the FourCC values:

- `0x3132564e` — `NV21`
- `0x31324d4e` — `NM21`

The worker then checks that its two dimension/count values are positive before calling `setInBuf()` through `getResultQT()`.

Separately, `ExynosSqzBase::execute()` also has its own initialised-state and buffer-validity checks before the ioctl instruction. Therefore, the presence of a code path does not mean every JPEG capture will submit an ioctl.

## 5. Reconcile this with the user's syscall trace

The user's Camera2 JPEG probe produced a valid JPEG and logged:

- `jsqz instance is created`;
- `[SAMSUNG_HW_JQ] : JPEG Qtable is initiated`;
- `[SAMSUNG_HW_JQ] : JPEG Qtable is denitiated`;
- `jsqz instance is destroyed`.

However, the recorded provider syscall trace contained no observed `0xC1344D00` or `0xC03C4D01` ioctl. That trace result must be kept separate from the static disassembly finding.

The target binaries now establish what would happen **if** the JSQZ worker reaches `run()`: its implementation dispatches to `execute()`, which attempts `ioctl(fd, 0xC1344D00, task)`. The missing runtime ioctl means that this particular trace did not demonstrate the worker completing that route. Possible explanations consistent with the code are:

1. the worker was instantiated but no frame met its JSQZ input conditions;
2. the worker reached an error/early-return path before `run()`;
3. the instance's node open failed, leaving it uninitialised, so the execute path could not reach the ioctl;
4. the syscall capture window/filter missed the specific call.

The trace did capture many other camera-provider ioctls, so option 4 is not the first assumption; it remains possible but unproven. The static analysis alone cannot decide which runtime branch occurred.

## 6. Implication for a public Android app

The installed Samsung camera HAL has a real JSQZ implementation and calls it from its JPEG-squeezer worker. A normal third-party app can request a standard Camera2 JPEG capture and thereby enter Samsung's camera HAL, but it cannot directly access `/dev/jsqz` on stock Android or change the node permissions.

The most promising public-app route is still to rely on the OEM Camera2 JPEG pipeline on firmware where Samsung's HAL successfully uses JSQZ. There is no JSQZ-specific public Camera2 metadata/API exposed by this device. A public APK cannot make up for an OEM permission/configuration problem by itself.

## Files supporting this report

- `target-jsqz-library-identification.txt`
- `target-libhwjsqz-symbol-table.txt`
- `target-libhwjsqz-relocations.txt`
- `target-libhwjsqz-disassembly.txt`
- `target-libexynoscamera3-elf.txt`
- `target-libexynoscamera3-symbol-table.txt`
- `target-libexynoscamera3-relocations.txt`
- `target-camera-jpeg-squeezer-thread.txt`
- `target-camera-jsqz-jump-slots.txt`
