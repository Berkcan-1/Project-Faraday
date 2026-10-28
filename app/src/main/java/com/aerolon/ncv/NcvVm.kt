package com.aerolon.ncv

enum class VmState { CREATED, RUNNING, PAUSED, STOPPED, FAULTED, UNKNOWN }

data class AndroidBootLoadInfo(
    val entry: Long,
    val initrdAddress: Long,
    val initrdSize: Long,
    val headerVersion: Int,
    val selectedDtbSize: Long,
    val bootDtbSize: Long,
    val vendorDtbSize: Long,
    val selectedDtbFromVendorBoot: Boolean,
    val commandLine: String,
    val kernelHeaderAddress: Long,
    val ramdiskHeaderAddress: Long,
    val tagsHeaderAddress: Long,
    val dtbHeaderAddress: Long
)

class NcvException(val code: Int, message: String) : RuntimeException(message)

/**
 * NativeBridge üzerinde ince sarmalayıcı. UI thread'inde çağrılabilir: start/pause/stop kısa sürer,
 * CPU kendi native thread'inde çalışır. (pause(), CPU'nun slice'ını bitirmesini bekler - kısa bloklar.)
 */
class NcvVm private constructor(private var handle: Long) : AutoCloseable {

    companion object {
        const val DEFAULT_RAM_BASE = 0x40000000L

        /**
         * Creates a VM with EL0 (default) or EL1 initial state. EL2/EL3 are not implemented.
         * Use initialEL=1 for kernel-oriented guest payloads; this does not imply MMU/Android boot support.
         */
        fun create(ramMiB: Int = 16, initialEL: Int = 0, compatibilityProfileId: Int = 0): NcvVm {
            val h = if (compatibilityProfileId == 0) NativeBridge.nativeCreateVmWithInitialEL(ramMiB, initialEL)
                    else NativeBridge.nativeCreateVmWithProfile(ramMiB, initialEL, compatibilityProfileId)
            if (h <= 0) throw NcvException(h.toInt(), "VM oluşturulamadı (kod=$h)")
            return NcvVm(h)
        }
    }

    private fun check(code: Int, what: String) {
        if (code != 0) throw NcvException(code, "$what başarısız (kod=$code)")
    }

    fun loadPayload(data: ByteArray, addr: Long = DEFAULT_RAM_BASE) =
        check(NativeBridge.nativeLoadPayload(handle, data, addr), "loadPayload")

    /**
     * Loads a raw or gzip-compressed ARM64 Linux Image using its standard 64-byte header
     * and selects EL1 entry. Decompression is bounded. This is not an Android boot.img/
     * vendor_boot parser. Returns the kernel's physical entry address.
     */
    fun loadLinuxImage(image: ByteArray): Long {
        val entry = NativeBridge.nativeLoadLinuxImage(handle, image)
        if (entry <= 0) throw NcvException(entry.toInt(), "ARM64 Linux Image yüklenemedi (kod=$entry)")
        return entry
    }

    /** Parses boot.img v0-v4 and optional vendor_boot/init_boot components. A monolithic boot.img
     * can be attempted by itself only when it embeds a non-empty generic ramdisk. Missing vendor
     * ramdisk/DTB may still make the selected kernel incompatible with the NCV virtual board.
     * Android userspace and display compatibility are unverified. */
    fun loadAndroidBootBundle(
        bootImage: ByteArray,
        vendorBootImage: ByteArray? = null,
        initBootImage: ByteArray? = null
    ): AndroidBootLoadInfo {
        val result = NativeBridge.nativeLoadAndroidBootBundle(handle, bootImage, vendorBootImage, initBootImage)
        if (result.size < 13) throw NcvException(-101, "Android boot image parser returned an invalid result")
        val status = result[0].toInt()
        if (status != 0) {
            val message = when (status) {
                1 -> "Invalid Android boot bundle. This boot.img may have no embedded generic ramdisk. A boot.img-only profile requires a non-empty ramdisk inside boot.img; otherwise select the matching init_boot.img or build a monolithic NCV boot image. vendor_boot is optional for this diagnostic attempt, but its vendor ramdisk/DTB may still be needed by a real Android kernel."
                4 -> "Android boot component points outside the selected file or guest RAM limits."
                6 -> "Not enough memory to prepare the Android kernel/ramdisk. Use smaller boot components or a device with more available memory."
                else -> "Android boot bundle could not be loaded (status=$status). Check that all components come from a compatible build."
            }
            throw NcvException(status, message)
        }
        return AndroidBootLoadInfo(
            result[1], result[2], result[3], result[4].toInt(), result[5], result[6], result[7],
            result[8] != 0L, NativeBridge.nativeGetBootArgs(handle), result[9], result[10], result[11], result[12]
        )
    }

    /** Attaches a disk image through virtio-mmio. Native duplicates the file descriptor. */
    fun attachBlockFd(fileDescriptor: Int, readOnly: Boolean = true, label: String = "disk") =
        check(NativeBridge.nativeAttachBlockFd(handle, fileDescriptor, readOnly, label), "attachBlockFd")

    /** Presents a super.img file as a GPT carrier disk with a partition labelled "super". */
    fun attachSuperContainerFd(fileDescriptor: Int, label: String = "super") =
        check(NativeBridge.nativeAttachSuperContainerFd(handle, fileDescriptor, label), "attachSuperContainerFd")

    /**
     * Exposes one liblp logical partition as an NCV host-side read-only linear virtio disk.
     * This is not the guest Linux device-mapper; the guest must still have a compatible virtio driver.
     */
    fun attachLogicalPartitionFd(fileDescriptor: Int, partition: LogicalPartitionInfo) {
        require(partition.sizeBytes > 0 && partition.extents.isNotEmpty()) { "Logical partition is empty." }
        require(!partition.disabled) { "Disabled logical partitions cannot be attached." }
        require(!partition.usesExternalBlockDevice) { "Partition uses an external liblp backing device." }
        val result = NativeBridge.nativeAttachLogicalPartitionFd(
            handle,
            fileDescriptor,
            partition.sizeBytes,
            partition.extents.map { it.logicalOffsetBytes }.toLongArray(),
            partition.extents.map { it.lengthBytes }.toLongArray(),
            partition.extents.map { extent ->
                when {
                    extent.isZero -> -1L
                    extent.sourceDeviceIndex != 0 -> -2L
                    extent.physicalOffsetBytes != null -> extent.physicalOffsetBytes
                    else -> -2L
                }
            }.toLongArray(),
            partition.extents.map { it.sourceDeviceIndex }.toIntArray(),
            partition.name
        )
        check(result, "attachLogicalPartitionFd(${partition.name})")
    }

    fun blockDeviceCount(): Int = NativeBridge.nativeGetBlockDeviceCount(handle)
    fun blockDevicesDiagnostics(): String = NativeBridge.nativeGetBlockDevicesDiagnostics(handle)
    fun compatibilityReport(): String = NativeBridge.nativeGetCompatibilityReport(handle)
    fun mtkProbeReport(): String = NativeBridge.nativeGetMtkProbeReport(handle)
    fun bootArgs(): String = NativeBridge.nativeGetBootArgs(handle)
    fun runtimeDiagnostics(): String = NativeBridge.nativeGetRuntimeDiagnostics(handle)

    fun setEntry(pc: Long) = check(NativeBridge.nativeSetEntry(handle, pc), "setEntry")

    /**
     * Generates a minimal NCV ARM64 board DTB, places it near the top of guest RAM,
     * selects initial EL1 and passes its physical address in X0 on start().
     * Load a compatible raw or gzip-compressed Linux Image first. This does not unpack Android
     * boot/vendor_boot containers or promise Linux/GSI boot success.
     */
    fun prepareLinuxBootDtb(
        bootargs: String = "console=ttyAMA0 earlycon=pl011,mmio32,0x09000000",
        initrdAddress: Long = 0L,
        initrdSize: Long = 0L
    ): Long {
        val address = NativeBridge.nativePrepareLinuxBootDtb(handle, bootargs, initrdAddress, initrdSize)
        if (address <= 0) throw NcvException(address.toInt(), "Linux DTB hazırlanamadı (kod=$address)")
        return address
    }

    /** Adapts the DTB embedded in legacy boot.img v2 for NCV RAM, GICv2, PL011 and virtio. */
    fun prepareAndroidBootDtb(bootargs: String, initrdAddress: Long, initrdSize: Long): Long {
        val address = NativeBridge.nativePrepareAndroidBootDtb(handle, bootargs, initrdAddress, initrdSize)
        if (address <= 0) throw NcvException(address.toInt(), "Embedded Android DTB could not be adapted (code=$address)")
        return address
    }

    fun start() = check(NativeBridge.nativeStart(handle), "start")
    fun pause() = check(NativeBridge.nativePause(handle), "pause")
    fun resume() = check(NativeBridge.nativeResume(handle), "resume")
    fun stop() = check(NativeBridge.nativeStop(handle), "stop")
    fun reset() = check(NativeBridge.nativeReset(handle), "reset")

    fun state(): VmState = when (NativeBridge.nativeGetState(handle)) {
        0 -> VmState.CREATED
        1 -> VmState.RUNNING
        2 -> VmState.PAUSED
        3 -> VmState.STOPPED
        4 -> VmState.FAULTED
        else -> VmState.UNKNOWN
    }

    /** take=true okunan çıktıyı tüketir. */
    fun console(take: Boolean = true): String = String(NativeBridge.nativeGetConsole(handle, take), Charsets.UTF_8)
    fun stopInfo(): String = NativeBridge.nativeGetStopInfo(handle)
    fun framebufferArgb(): IntArray = NativeBridge.nativeGetFramebufferArgb(handle)

    override fun close() {
        if (handle > 0) {
            NativeBridge.nativeDestroyVm(handle)
            handle = 0
        }
    }
}
