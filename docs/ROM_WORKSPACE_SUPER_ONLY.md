# super.img-first ROM Workspace

## What changed

NCV's UI now sends files identified as `super.img` to a dedicated ROM Workspace. The user does not have to provide an OEM `boot.img` just to inspect the ROM. The previous low-level guest/boot diagnostic screen remains separate for kernel and boot-image experiments.

The workspace:

- Opens the selected image read-only through Android's document provider.
- Validates the liblp geometry, metadata header and table checksums.
- Reports logical partitions, slot-suffixed names, disabled flags and extent attributes.
- Preserves valid zero-extent entries as empty 0-byte partitions instead of silently pretending they contain data; extent-range errors report the start index, extent count and metadata extent-table size.
- Reads actual bytes from a logical partition by mapping its extent offsets through the expanded raw or Android sparse image.
- Returns zeros for liblp zero extents and rejects extents that require an external backing block device not included in the selected file.
- Performs a small partition read probe and checks the standard superblock location for ext4/EROFS magic.
- Explicitly reports that no Android boot or launcher has been reached.

## What it does not do

This is not yet an Android runtime or VM boot path. The workspace does not mount ext4/EROFS, execute any binaries from the ROM, create Linux device-mapper nodes, run `init`, start Binder/ART/system services, display the OEM boot animation, or launch the OEM setup wizard. The generic guest kernel/initramfs route that would replace the OEM `boot.img` is not connected to this workflow yet.

A super image is a container for dynamic system partitions; it is not a complete boot bundle. NCV must provide a working guest kernel and initramfs itself, then map the partitions into the guest and implement the required Linux/Android interfaces. If a ROM relies on vendor-specific HALs or services, matching vendor-side components or compatibility work may also be required.

## Read-only partition reader API

`SuperImageMetadataParser.open(source)` returns a `SuperImageLayout`. `layout.openPartition(name)` returns a `SuperImagePartitionReader`, whose `readAt(offset, length)` translates a logical partition range into the selected image's expanded logical address space. All reads remain read-only.

The parser/reader can be tested on the host without Android SDK using the included smoke-test source:

```sh
kotlinc app/src/main/java/com/aerolon/ncv/SuperImageMetadata.kt \
  tools/test_super_partition_reader.kt -include-runtime -d /tmp/ncv-super-reader-test.jar
java -jar /tmp/ncv-super-reader-test.jar /path/to/super.img
```

The test reads up to 4 KiB from every listed logical partition and reports any extent that requires an external backing device. UI/Android SDK build and physical-device tests are still required for the app integration.
