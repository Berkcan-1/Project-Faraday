# NCV v0.1.20 — Android Path Focus

## Why the Infinix logo did not appear

The supplied `0.1.18` log came from `beginNcvGenericSuperBringup()`. That path loads the generic NCV Linux `Image` and BusyBox test initramfs. It deliberately does not launch Android `init`, ART, Binder, SurfaceFlinger, or the boot animation.

## Changes

- Bumped the app version to `0.1.20-dev`, versionCode `8`, and native engine marker to `0.1.20-dev-android-path-focus-20261028`.
- Made the actual OEM `boot.img + super.img` attempt the primary action in the ROM boot profile.
- Clearly labelled the NCV generic Linux path as a storage test only, not Android.
- Defaulted OEM attempts to the hybrid Infinix/MediaTek DTB and MTK probe fallback profile; both remain experimental and do not implement the MT6768 SoC.
- Added a UART console/earlycon fallback to the effective Android kernel command line while preserving existing OEM arguments.

## Important limitations

This UI change does not by itself make the Infinix ROM boot. The NCV virtio storage is not an emulation of the phone's MediaTek eMMC/UFS controller; stock init/fstab may expect device-specific by-name paths; the SoC clock/reset/PMIC/IOMMU/Mali/HWC and proprietary HALs are not implemented. The custom ARM64 interpreter also has a limited instruction/system-register surface. A genuine boot animation requires Android's actual boot chain and graphics path to reach `bootanimation`; this release does not claim that it does.

## Recommended use

Choose matching `boot.img` and `super.img` from one firmware build, optionally add that build's `vendor_boot.img` and `init_boot.img` if present/required, leave the default hybrid vendor DTB and MTK probe profile for the first OEM attempt, then use the primary **Attempt Android** button. Use the generic Linux button only when intentionally testing NCV's own virtual board/storage backend.
