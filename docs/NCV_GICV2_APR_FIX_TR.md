# NCV GICv2 APR register fix

## Kesin bulgu

Gerçek guest logu şu erişimi raporladı:

- Physical address: `0x080100d0`
- Access: 4-byte write
- Bus result: `DeviceError`
- Guest consequence: same-EL Data Abort (`ESR_EL1=0x96000050`), sonra `B .` park loop

NCV board GIC base is `0x08000000`; GIC CPU interface base is `0x08010000`; offset `0xD0` is `GICC_APR0` in GICv2. The former implementation returned `DeviceError` for that architected register.

## Change

- Added backing state for `GICC_APR0..APR3` offsets `0xD0..0xDC`.
- Added backing state for `GICC_NSAPR0..NSAPR3` offsets `0xE0..0xEC`.
- Added basic alias/read/write behavior for `AIAR`, `AEOIR`, `AHPPIR`, optional `STATUSR`, and `DIR`.
- Added regression test using the exact physical address `0x080100D0` through the bus.
- Updated native engine marker to `0.1.6-dev-gic-apr-20261027`.

## Host verification

The focused `ncv_fault_diagnostic_tests` target passed 7/7 tests in Release, ASan+UBSan, and ThreadSanitizer configurations. This is host-side device-model verification, not Android APK or guest-boot verification.

## Scope

This is a targeted GICv2 compatibility fix, not a complete GICv2/security/SMP emulation and not proof that Android boots. It should clear the exact DeviceError in the supplied log; the next boot may expose another missing register/device or a separate boot-image mismatch.
