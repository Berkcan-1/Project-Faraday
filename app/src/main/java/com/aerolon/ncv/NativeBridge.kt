package com.aerolon.ncv

/**
 * NCV JNI sınırı. Native taraf (ncv_jni.cpp) ile birebir eşleşir; imza değişirse ikisini birlikte güncelle.
 * Dönüş kodları: >= 0 native Status (0 = Ok); -100 geçersiz handle; -101 native exception.
 * nativeCreateVm: > 0 handle, <= 0 hata.
 */
object NativeBridge {
    init {
        System.loadLibrary("ncv")
    }

    external fun nativeGetEngineVersion(): String
    external fun nativeSelfTest(): String
    /** Extracts the selected Android boot bundle's real ramdisk fragments for the NCV-kernel path. */
    external fun nativeExtractAndroidInitramfs(bootImage: ByteArray, vendorBootImage: ByteArray?, initBootImage: ByteArray?): ByteArray?
    /** Returns the boot bundle kernel command line, or an empty string when parsing fails. */
    external fun nativeExtractAndroidBootArgs(bootImage: ByteArray, vendorBootImage: ByteArray?, initBootImage: ByteArray?): String

    external fun nativeCreateVm(ramMiB: Int): Long
    external fun nativeCreateVmWithInitialEL(ramMiB: Int, initialEL: Int): Long
    /** compatibilityProfileId: 0=strict NCV board, 1=experimental MTK unmapped-MMIO probe mode. */
    external fun nativeCreateVmWithProfile(ramMiB: Int, initialEL: Int, compatibilityProfileId: Int): Long
    external fun nativeDestroyVm(handle: Long): Int
    external fun nativeLoadPayload(handle: Long, data: ByteArray, addr: Long): Int
    /** Returns the guest physical kernel entry address, or a negative status/error code. */
    external fun nativeLoadLinuxImage(handle: Long, data: ByteArray): Long
    /** Returns [status, entry, initrdAddress, initrdSize, headerVersion, selectedDtbSize, bootDtbSize, vendorDtbSize, selectedDtbFromVendorBoot, legacyKernelAddr, legacyRamdiskAddr, legacyTagsAddr, legacyDtbAddr]. */
    external fun nativeLoadAndroidBootBundle(handle: Long, bootImage: ByteArray, vendorBootImage: ByteArray?, initBootImage: ByteArray?): LongArray
    external fun nativeAttachBlockFd(handle: Long, fileDescriptor: Int, readOnly: Boolean, label: String): Int
    /** Exposes super.img through a synthetic GPT disk containing one partition named "super". */
    external fun nativeAttachSuperContainerFd(handle: Long, fileDescriptor: Int, label: String): Int
    external fun nativeAttachLogicalPartitionFd(
        handle: Long,
        fileDescriptor: Int,
        logicalSizeBytes: Long,
        logicalOffsetsBytes: LongArray,
        extentLengthsBytes: LongArray,
        physicalOffsetsBytes: LongArray, // -1 marks a liblp ZERO extent
        sourceDeviceIndices: IntArray,
        label: String
    ): Int
    external fun nativeGetBlockDeviceCount(handle: Long): Int
    external fun nativeGetBlockDevicesDiagnostics(handle: Long): String
    external fun nativeGetCompatibilityReport(handle: Long): String
    external fun nativeGetMtkProbeReport(handle: Long): String
    external fun nativeGetBootArgs(handle: Long): String
    external fun nativeGetRuntimeDiagnostics(handle: Long): String
    external fun nativeSetEntry(handle: Long, pc: Long): Int
    /** Returns the guest physical DTB address on success; non-positive values are errors. */
    external fun nativePrepareLinuxBootDtb(handle: Long, bootargs: String, initrdAddr: Long, initrdSize: Long): Long
    /** Builds a hybrid DTB from the embedded legacy boot.img DTB; experimental MTK path. */
    external fun nativePrepareAndroidBootDtb(handle: Long, bootargs: String, initrdAddr: Long, initrdSize: Long): Long
    external fun nativeStart(handle: Long): Int
    external fun nativePause(handle: Long): Int
    external fun nativeResume(handle: Long): Int
    external fun nativeStop(handle: Long): Int
    external fun nativeReset(handle: Long): Int
    external fun nativeGetState(handle: Long): Int
    external fun nativeGetConsole(handle: Long, take: Boolean): ByteArray
    external fun nativeGetStopInfo(handle: Long): String
    external fun nativeGetFramebufferArgb(handle: Long): IntArray
}
