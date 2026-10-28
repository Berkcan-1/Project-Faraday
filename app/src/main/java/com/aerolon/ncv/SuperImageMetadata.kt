package com.aerolon.ncv

import java.io.IOException
import java.security.MessageDigest

/** Random-access view over the physical image file. Implementations must return exactly [length] bytes. */
interface SeekableImageDataSource {
    val physicalSizeBytes: Long
    fun readAt(offset: Long, length: Int): ByteArray
}

data class LogicalPartitionExtent(
    /** Byte offset within the logical partition. */
    val logicalOffsetBytes: Long,
    val lengthBytes: Long,
    /** True for liblp zero extents; reads from these extents return zero-filled bytes. */
    val isZero: Boolean,
    /** 0 means the selected super image; other indices refer to external block devices. */
    val sourceDeviceIndex: Int,
    /** Byte offset in the expanded super image, null for zero/external-device extents. */
    val physicalOffsetBytes: Long?
)

data class LogicalPartitionInfo(
    val name: String,
    val sizeBytes: Long,
    val extentCount: Int,
    val readOnly: Boolean,
    val usesExternalBlockDevice: Boolean,
    val hasZeroExtents: Boolean,
    /** True when liblp marks this entry disabled; it must not be launched as an Android partition. */
    val disabled: Boolean = false,
    val extents: List<LogicalPartitionExtent> = emptyList()
)

data class SuperImageSummary(
    val metadataSlot: Int,
    val metadataMajorVersion: Int,
    val metadataMinorVersion: Int,
    val logicalImageSizeBytes: Long,
    val sparseEncoded: Boolean,
    val partitions: List<LogicalPartitionInfo>
)

/**
 * Read-only parser for Android liblp metadata inside raw or Android sparse super images.
 * It does not mount ext4, verify AVB, create device-mapper nodes, or boot the guest.
 */
/** Read-only logical byte view for one liblp partition. This does not mount a filesystem. */
class SuperImagePartitionReader internal constructor(
    val partition: LogicalPartitionInfo,
    private val readExpandedImageAt: (Long, Int) -> ByteArray
) {
    fun readAt(offset: Long, length: Int): ByteArray {
        if (offset < 0L || length < 0 || offset > partition.sizeBytes || length.toLong() > partition.sizeBytes - offset) {
            throw IOException("Read is outside logical partition '${partition.name}'.")
        }
        if (length == 0) return ByteArray(0)
        val output = ByteArray(length)
        var logicalPosition = offset
        var destination = 0
        var remaining = length
        while (remaining > 0) {
            val extent = findExtent(logicalPosition)
                ?: throw IOException("Logical partition '${partition.name}' contains an unmapped extent gap.")
            val withinExtent = logicalPosition - extent.logicalOffsetBytes
            val count = minOf(remaining.toLong(), extent.lengthBytes - withinExtent).toInt()
            when {
                extent.isZero -> output.fill(0, destination, destination + count)
                extent.sourceDeviceIndex != 0 -> throw IOException(
                    "Partition '${partition.name}' uses external liblp block device ${extent.sourceDeviceIndex}; that backing device was not supplied."
                )
                extent.physicalOffsetBytes != null -> {
                    val bytes = readExpandedImageAt(Math.addExact(extent.physicalOffsetBytes, withinExtent), count)
                    bytes.copyInto(output, destination)
                }
                else -> throw IOException("Partition '${partition.name}' has an unsupported extent mapping.")
            }
            logicalPosition += count
            destination += count
            remaining -= count
        }
        return output
    }

    private fun findExtent(position: Long): LogicalPartitionExtent? {
        val extents = partition.extents
        var low = 0
        var high = extents.lastIndex
        while (low <= high) {
            val mid = (low + high) ushr 1
            val extent = extents[mid]
            val end = extent.logicalOffsetBytes + extent.lengthBytes
            when {
                position < extent.logicalOffsetBytes -> high = mid - 1
                position >= end -> low = mid + 1
                else -> return extent
            }
        }
        return null
    }
}

/** Parsed image metadata plus read-only logical partition views backed by the selected image. */
class SuperImageLayout internal constructor(
    val summary: SuperImageSummary,
    private val readExpandedImageAt: (Long, Int) -> ByteArray
) {
    fun openPartition(name: String): SuperImagePartitionReader {
        val partition = summary.partitions.firstOrNull { it.name == name }
            ?: throw IOException("Logical partition '$name' was not found in super metadata.")
        return SuperImagePartitionReader(partition, readExpandedImageAt)
    }
}

object SuperImageMetadataParser {
    private const val LP_GEOMETRY_MAGIC = 0x616c4467
    private const val LP_HEADER_MAGIC = 0x414C5030
    private const val LP_GEOMETRY_SIZE = 4096L
    private const val LP_RESERVED_BYTES = 4096L
    private const val LP_SECTOR_SIZE = 512L
    private const val GEOMETRY_STRUCT_SIZE = 52
    private const val HEADER_V1_0_SIZE = 128
    private const val HEADER_V1_2_SIZE = 256
    private const val PARTITION_ENTRY_SIZE = 52
    private const val EXTENT_ENTRY_SIZE = 24
    private const val GROUP_ENTRY_SIZE = 48
    private const val BLOCK_DEVICE_ENTRY_SIZE = 64
    private const val MAX_TABLE_BYTES = 32 * 1024 * 1024
    private const val MAX_PARTITIONS = 4096
    private const val MAX_EXTENTS = 262_144
    private const val MAX_GROUPS = 4096
    private const val MAX_BLOCK_DEVICES = 256
    private const val MAX_SPARSE_CHUNKS = 250_000
    private const val SPARSE_MAGIC = 0xED26FF3A.toInt()
    private const val CHUNK_RAW = 0xCAC1
    private const val CHUNK_FILL = 0xCAC2
    private const val CHUNK_DONT_CARE = 0xCAC3
    private const val CHUNK_CRC32 = 0xCAC4
    private const val EXTENT_LINEAR = 0
    private const val EXTENT_ZERO = 1
    private const val PARTITION_ATTR_READONLY = 1 shl 0
    private const val PARTITION_ATTR_SLOT_SUFFIXED = 1 shl 1
    private const val PARTITION_ATTR_UPDATED = 1 shl 2
    private const val PARTITION_ATTR_DISABLED = 1 shl 3

    fun inspect(source: SeekableImageDataSource): SuperImageSummary = open(source).summary

    fun open(source: SeekableImageDataSource): SuperImageLayout {
        if (source.physicalSizeBytes < 4096L) throw IOException("Image is too small to contain super metadata.")
        val image = LogicalImage(source)
        val geometry = readGeometry(image)
        val errors = mutableListOf<String>()

        // Slot A is normally first. If its metadata is damaged, fall back to another valid slot.
        for (slot in 0 until geometry.slotCount) {
            val primary = primaryMetadataOffset(geometry, slot)
            val backup = backupMetadataOffset(geometry, slot)
            for (metadataOffset in listOf(primary, backup)) {
                try {
                    val result = parseMetadata(image, geometry, metadataOffset, slot)
                    val summary = SuperImageSummary(
                        metadataSlot = slot,
                        metadataMajorVersion = result.majorVersion,
                        metadataMinorVersion = result.minorVersion,
                        logicalImageSizeBytes = image.logicalSizeBytes,
                        sparseEncoded = image.sparseEncoded,
                        partitions = result.partitions
                    )
                    return SuperImageLayout(summary, image::readAt)
                } catch (error: IOException) {
                    errors += "slot $slot @ $metadataOffset: ${error.message ?: "invalid metadata"}"
                } catch (error: ArithmeticException) {
                    errors += "slot $slot @ $metadataOffset: integer overflow"
                }
            }
        }
        throw IOException("No valid liblp metadata copy found. ${errors.take(3).joinToString("; ")}")
    }

    private data class Geometry(
        val metadataMaxSize: Long,
        val slotCount: Int,
        val logicalBlockSize: Long
    )

    private data class ParsedMetadata(
        val majorVersion: Int,
        val minorVersion: Int,
        val partitions: List<LogicalPartitionInfo>
    )

    private fun readGeometry(image: LogicalImage): Geometry {
        val candidates = listOf(LP_RESERVED_BYTES, LP_RESERVED_BYTES + LP_GEOMETRY_SIZE)
        var lastError: IOException? = null
        for (offset in candidates) {
            try {
                val bytes = image.readAt(offset, GEOMETRY_STRUCT_SIZE)
                if (u32(bytes, 0) != LP_GEOMETRY_MAGIC.toLong()) throw IOException("geometry magic mismatch")
                val structSize = u32(bytes, 4).toInt()
                if (structSize != GEOMETRY_STRUCT_SIZE) throw IOException("unsupported geometry structure size $structSize")
                val checksumBytes = bytes.copyOf()
                checksumBytes.fill(0, 8, 40)
                val expectedChecksum = bytes.copyOfRange(8, 40)
                if (!MessageDigest.isEqual(sha256(checksumBytes), expectedChecksum)) {
                    throw IOException("geometry SHA-256 mismatch")
                }
                val metadataMaxSize = u32(bytes, 40)
                val slotCount = u32(bytes, 44).toInt()
                val blockSize = u32(bytes, 48)
                if (metadataMaxSize <= 0L || metadataMaxSize % LP_SECTOR_SIZE != 0L || metadataMaxSize > 64L * 1024 * 1024) {
                    throw IOException("invalid metadata_max_size")
                }
                if (slotCount !in 1..8) throw IOException("invalid metadata slot count $slotCount")
                if (blockSize < LP_SECTOR_SIZE || blockSize % LP_SECTOR_SIZE != 0L) {
                    throw IOException("invalid logical block size $blockSize")
                }
                return Geometry(metadataMaxSize, slotCount, blockSize)
            } catch (error: IOException) {
                lastError = error
            }
        }
        throw IOException("Android dynamic-partition geometry was not found: ${lastError?.message ?: "unknown format"}")
    }

    private fun parseMetadata(
        image: LogicalImage,
        geometry: Geometry,
        metadataOffset: Long,
        metadataSlot: Int
    ): ParsedMetadata {
        val headerV10 = image.readAt(metadataOffset, HEADER_V1_0_SIZE)
        if (u32(headerV10, 0) != LP_HEADER_MAGIC.toLong()) throw IOException("metadata header magic mismatch")
        val major = u16(headerV10, 4)
        val minor = u16(headerV10, 6)
        if (major != 10 || minor !in 0..2) throw IOException("unsupported liblp metadata version $major.$minor")
        val expectedHeaderSize = if (minor >= 2) HEADER_V1_2_SIZE else HEADER_V1_0_SIZE
        val headerSize = u32(headerV10, 8).toInt()
        if (headerSize != expectedHeaderSize) throw IOException("invalid metadata header size $headerSize")
        val header = if (headerSize == HEADER_V1_0_SIZE) headerV10 else image.readAt(metadataOffset, headerSize)
        val headerForHash = header.copyOf()
        headerForHash.fill(0, 12, 44)
        if (!MessageDigest.isEqual(sha256(headerForHash), header.copyOfRange(12, 44))) {
            throw IOException("metadata header SHA-256 mismatch")
        }

        val tablesSize = u32(header, 44).toInt()
        if (tablesSize < 0 || tablesSize > MAX_TABLE_BYTES || tablesSize.toLong() > geometry.metadataMaxSize) {
            throw IOException("invalid metadata tables size $tablesSize")
        }
        val tables = image.readAt(metadataOffset + headerSize, tablesSize)
        if (!MessageDigest.isEqual(sha256(tables), header.copyOfRange(48, 80))) {
            throw IOException("metadata tables SHA-256 mismatch")
        }

        val partitionsDesc = descriptor(header, 80)
        val extentsDesc = descriptor(header, 92)
        val groupsDesc = descriptor(header, 104)
        val devicesDesc = descriptor(header, 116)
        validateDescriptor(partitionsDesc, PARTITION_ENTRY_SIZE, tablesSize, MAX_PARTITIONS, "partitions")
        validateDescriptor(extentsDesc, EXTENT_ENTRY_SIZE, tablesSize, MAX_EXTENTS, "extents")
        validateDescriptor(groupsDesc, GROUP_ENTRY_SIZE, tablesSize, MAX_GROUPS, "groups")
        validateDescriptor(devicesDesc, BLOCK_DEVICE_ENTRY_SIZE, tablesSize, MAX_BLOCK_DEVICES, "block devices")
        if (groupsDesc.count < 1) throw IOException("metadata has no partition group")
        if (devicesDesc.count < 1) throw IOException("metadata has no block-device entry")

        val devices = ArrayList<String>(devicesDesc.count)
        repeat(devicesDesc.count) { i ->
            val pos = devicesDesc.offset + i * devicesDesc.entrySize
            devices += fixedAscii(tables, pos + 24, 36)
        }

        val extents = ArrayList<Extent>(extentsDesc.count)
        repeat(extentsDesc.count) { i ->
            val pos = extentsDesc.offset + i * extentsDesc.entrySize
            val sectors = u64(tables, pos)
            val targetType = u32(tables, pos + 8).toInt()
            val targetData = u64(tables, pos + 12)
            val targetSource = u32(tables, pos + 20).toInt()
            if (sectors <= 0L) throw IOException("extent $i has zero/invalid length")
            if (targetType != EXTENT_LINEAR && targetType != EXTENT_ZERO) throw IOException("extent $i has unsupported target type $targetType")
            if (targetSource !in devices.indices) throw IOException("extent $i references invalid block device $targetSource")
            if (targetType == EXTENT_ZERO && targetData != 0L) throw IOException("zero extent $i has unexpected target data")
            if (targetType == EXTENT_LINEAR && targetSource == 0) {
                val endSector = safeAdd(targetData, sectors, "extent $i end")
                val deviceSize = image.logicalSizeBytes / LP_SECTOR_SIZE
                if (endSector > deviceSize) throw IOException("extent $i maps outside the selected image")
            }
            extents += Extent(sectors, targetType, targetData, targetSource)
        }

        val partitions = ArrayList<LogicalPartitionInfo>(partitionsDesc.count)
        val seenNames = HashSet<String>()
        repeat(partitionsDesc.count) { i ->
            val pos = partitionsDesc.offset + i * partitionsDesc.entrySize
            val storedName = fixedAscii(tables, pos, 36)
            if (storedName.isBlank() || !storedName.matches(Regex("[A-Za-z0-9_]{1,36}"))) {
                throw IOException("partition $i has an invalid name")
            }
            val attributesLong = u32(tables, pos + 36)
            val attributes = attributesLong.toInt()
            val validAttributeMask = PARTITION_ATTR_READONLY or PARTITION_ATTR_SLOT_SUFFIXED or
                (if (minor >= 1) PARTITION_ATTR_UPDATED or PARTITION_ATTR_DISABLED else 0)
            if ((attributes and validAttributeMask.inv()) != 0) {
                throw IOException("partition '$storedName' has unsupported attributes 0x${attributesLong.toString(16)} for liblp $major.$minor")
            }
            val name = if ((attributes and PARTITION_ATTR_SLOT_SUFFIXED) != 0) {
                val suffix = ('a'.code + metadataSlot).toChar()
                val slottedName = "${storedName}_$suffix"
                if (slottedName.length > 36) throw IOException("slot-suffixed partition name '$slottedName' exceeds 36 bytes")
                slottedName
            } else storedName
            if (!seenNames.add(name)) throw IOException("duplicate partition name '$name'")

            val firstExtentRaw = u32(tables, pos + 40)
            val numExtentsRaw = u32(tables, pos + 44)
            val groupIndexRaw = u32(tables, pos + 48)
            if (groupIndexRaw >= groupsDesc.count.toLong()) throw IOException("partition '$name' references invalid group $groupIndexRaw")
            // Keep liblp's extent-table bounds check strict. Never skip or repair an invalid
            // range: it could turn corrupted/misread metadata into a misleading ROM result.
            // This diagnostic marker also helps confirm that the APK contains this parser build.
            val extentEndExclusive = firstExtentRaw + numExtentsRaw
            if (firstExtentRaw > extents.size.toLong() ||
                extentEndExclusive < firstExtentRaw ||
                extentEndExclusive > extents.size.toLong()
            ) {
                throw IOException(
                    "NCV_LIBLP_DIAG_V2: invalid partition extent range; " +
                        "partition='$name', stored_name='$storedName', metadata_slot=$metadataSlot, " +
                        "partition_index=$i, partition_table_relative_offset=$pos, " +
                        "first_extent_index=$firstExtentRaw, num_extents=$numExtentsRaw, " +
                        "range_end_exclusive=$extentEndExclusive, total_extents=${extents.size}, " +
                        "extent_table_offset=${extentsDesc.offset}, extent_entry_size=${extentsDesc.entrySize}"
                )
            }
            val firstExtent = firstExtentRaw.toInt()
            val numExtents = numExtentsRaw.toInt()
            var sizeBytes = 0L
            var external = false
            var zero = false
            val partitionExtents = ArrayList<LogicalPartitionExtent>(numExtents)
            for (extentIndex in firstExtent until firstExtent + numExtents) {
                val extent = extents[extentIndex]
                val lengthBytes = try {
                    Math.multiplyExact(extent.sectors, LP_SECTOR_SIZE)
                } catch (_: ArithmeticException) {
                    throw IOException("partition '$name' extent length overflow")
                }
                val isZero = extent.targetType == EXTENT_ZERO
                val physicalOffsetBytes = if (!isZero && extent.targetSource == 0) {
                    try { Math.multiplyExact(extent.targetData, LP_SECTOR_SIZE) } catch (_: ArithmeticException) {
                        throw IOException("partition '$name' physical offset overflow")
                    }
                } else null
                partitionExtents += LogicalPartitionExtent(
                    logicalOffsetBytes = sizeBytes,
                    lengthBytes = lengthBytes,
                    isZero = isZero,
                    sourceDeviceIndex = extent.targetSource,
                    physicalOffsetBytes = physicalOffsetBytes
                )
                sizeBytes = safeAdd(sizeBytes, lengthBytes, "partition '$name' size")
                external = external || extent.targetSource != 0
                zero = zero || isZero
            }
            partitions += LogicalPartitionInfo(
                name = name,
                sizeBytes = sizeBytes,
                extentCount = numExtents,
                readOnly = (attributes and PARTITION_ATTR_READONLY) != 0,
                usesExternalBlockDevice = external,
                hasZeroExtents = zero,
                disabled = (attributes and PARTITION_ATTR_DISABLED) != 0,
                extents = partitionExtents
            )
        }

        if (partitions.isEmpty()) throw IOException("metadata contains no logical partitions")
        return ParsedMetadata(major, minor, partitions.sortedBy { it.name })
    }

    private data class Descriptor(val offset: Int, val count: Int, val entrySize: Int)
    private data class Extent(val sectors: Long, val targetType: Int, val targetData: Long, val targetSource: Int)

    private fun descriptor(header: ByteArray, offset: Int): Descriptor = Descriptor(
        offset = u32(header, offset).toInt(),
        count = u32(header, offset + 4).toInt(),
        entrySize = u32(header, offset + 8).toInt()
    )

    private fun validateDescriptor(
        descriptor: Descriptor,
        expectedEntrySize: Int,
        tablesSize: Int,
        maxCount: Int,
        label: String
    ) {
        if (descriptor.count !in 0..maxCount) throw IOException("invalid $label count ${descriptor.count}")
        if (descriptor.entrySize != expectedEntrySize) throw IOException("invalid $label entry size ${descriptor.entrySize}")
        if (descriptor.offset < 0 || descriptor.offset > tablesSize) throw IOException("invalid $label table offset")
        val bytes = descriptor.count.toLong() * descriptor.entrySize
        if (bytes > tablesSize.toLong() - descriptor.offset) throw IOException("$label table exceeds metadata bounds")
    }

    private fun primaryMetadataOffset(geometry: Geometry, slot: Int): Long =
        safeAdd(LP_RESERVED_BYTES + 2L * LP_GEOMETRY_SIZE, geometry.metadataMaxSize * slot, "primary metadata offset")

    private fun backupMetadataOffset(geometry: Geometry, slot: Int): Long =
        safeAdd(
            LP_RESERVED_BYTES + 2L * LP_GEOMETRY_SIZE,
            Math.multiplyExact(geometry.metadataMaxSize, geometry.slotCount.toLong() + slot),
            "backup metadata offset"
        )

    private fun sha256(bytes: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").digest(bytes)

    private fun u16(bytes: ByteArray, offset: Int): Int {
        requireRange(bytes, offset, 2)
        return (bytes[offset].toInt() and 0xff) or ((bytes[offset + 1].toInt() and 0xff) shl 8)
    }

    private fun u32(bytes: ByteArray, offset: Int): Long {
        requireRange(bytes, offset, 4)
        return (bytes[offset].toLong() and 0xff) or
            ((bytes[offset + 1].toLong() and 0xff) shl 8) or
            ((bytes[offset + 2].toLong() and 0xff) shl 16) or
            ((bytes[offset + 3].toLong() and 0xff) shl 24)
    }

    private fun u64(bytes: ByteArray, offset: Int): Long {
        requireRange(bytes, offset, 8)
        val low = u32(bytes, offset)
        val high = u32(bytes, offset + 4)
        if (high > 0x7fffffffL) throw IOException("unsigned 64-bit field exceeds supported address range")
        return (high shl 32) or low
    }

    private fun fixedAscii(bytes: ByteArray, offset: Int, length: Int): String {
        requireRange(bytes, offset, length)
        val end = (offset until offset + length).firstOrNull { bytes[it].toInt() == 0 } ?: (offset + length)
        return String(bytes, offset, end - offset, Charsets.US_ASCII)
    }

    private fun requireRange(bytes: ByteArray, offset: Int, length: Int) {
        if (offset < 0 || length < 0 || offset.toLong() + length > bytes.size) throw IOException("metadata field exceeds buffer bounds")
    }

    private fun safeAdd(a: Long, b: Long, label: String): Long = try {
        Math.addExact(a, b)
    } catch (_: ArithmeticException) {
        throw IOException("$label overflow")
    }

    /** Exposes a raw or Android sparse image as a logical, uncompressed byte address space. */
    private class LogicalImage(private val source: SeekableImageDataSource) {
        private data class Chunk(
            val logicalStart: Long,
            val length: Long,
            val kind: Int,
            val physicalDataStart: Long = 0L,
            val fill: ByteArray = ByteArray(0)
        )

        val sparseEncoded: Boolean
        val logicalSizeBytes: Long
        private val chunks: List<Chunk>

        init {
            if (source.physicalSizeBytes < 4) throw IOException("Image is empty")
            val magic = u32(source.readAt(0, 4), 0).toInt()
            sparseEncoded = magic == SPARSE_MAGIC
            if (!sparseEncoded) {
                logicalSizeBytes = source.physicalSizeBytes
                chunks = emptyList()
            } else {
                val indexed = indexSparseImage()
                logicalSizeBytes = indexed.first
                chunks = indexed.second
            }
        }

        fun readAt(offset: Long, length: Int): ByteArray {
            if (offset < 0L || length < 0 || offset > logicalSizeBytes || length.toLong() > logicalSizeBytes - offset) {
                throw IOException("logical image read is outside image bounds")
            }
            if (length == 0) return ByteArray(0)
            if (!sparseEncoded) return source.readAt(offset, length)
            val output = ByteArray(length)
            var current = offset
            var destination = 0
            var remaining = length
            while (remaining > 0) {
                val chunk = findChunk(current) ?: throw IOException("sparse image has a logical data gap")
                val inChunk = current - chunk.logicalStart
                val count = minOf(remaining.toLong(), chunk.length - inChunk).toInt()
                when (chunk.kind) {
                    CHUNK_RAW -> {
                        val bytes = source.readAt(chunk.physicalDataStart + inChunk, count)
                        bytes.copyInto(output, destination)
                    }
                    CHUNK_FILL -> {
                        for (i in 0 until count) output[destination + i] = chunk.fill[((inChunk + i) % 4L).toInt()]
                    }
                    CHUNK_DONT_CARE -> output.fill(0, destination, destination + count)
                    else -> throw IOException("unsupported sparse chunk kind ${chunk.kind}")
                }
                current += count
                destination += count
                remaining -= count
            }
            return output
        }

        private fun findChunk(position: Long): Chunk? {
            var low = 0
            var high = chunks.lastIndex
            while (low <= high) {
                val mid = (low + high) ushr 1
                val chunk = chunks[mid]
                when {
                    position < chunk.logicalStart -> high = mid - 1
                    position >= chunk.logicalStart + chunk.length -> low = mid + 1
                    else -> return chunk
                }
            }
            return null
        }

        private fun indexSparseImage(): Pair<Long, List<Chunk>> {
            val header = source.readAt(0, 28)
            val major = u16(header, 4)
            if (major != 1) throw IOException("unsupported Android sparse-image version $major")
            val fileHeaderSize = u16(header, 8)
            val chunkHeaderSize = u16(header, 10)
            val blockSize = u32(header, 12)
            val totalBlocks = u32(header, 16)
            val totalChunks = u32(header, 20).toInt()
            if (fileHeaderSize !in 28..4096 || chunkHeaderSize !in 12..64) throw IOException("invalid sparse-image header sizes")
            if (blockSize < 512 || blockSize > 1024 * 1024 || blockSize % 4L != 0L) throw IOException("invalid sparse-image block size")
            if (totalChunks !in 1..MAX_SPARSE_CHUNKS) throw IOException("sparse image has an unreasonable chunk count")
            val logicalSize = try { Math.multiplyExact(totalBlocks, blockSize) } catch (_: ArithmeticException) {
                throw IOException("sparse image expands beyond supported address range")
            }
            if (logicalSize <= 0L) throw IOException("sparse image has zero logical size")
            val indexed = ArrayList<Chunk>(totalChunks)
            var physicalCursor = fileHeaderSize.toLong()
            var logicalCursor = 0L
            repeat(totalChunks) { index ->
                if (physicalCursor < 0L || physicalCursor + chunkHeaderSize > source.physicalSizeBytes) {
                    throw IOException("sparse chunk $index header exceeds physical image bounds")
                }
                val chunkHeader = source.readAt(physicalCursor, chunkHeaderSize)
                val type = u16(chunkHeader, 0)
                val chunkBlocks = u32(chunkHeader, 4)
                val totalSize = u32(chunkHeader, 8)
                if (totalSize < chunkHeaderSize.toLong() || physicalCursor + totalSize > source.physicalSizeBytes) {
                    throw IOException("sparse chunk $index has invalid size")
                }
                val logicalLength = try { Math.multiplyExact(chunkBlocks, blockSize) } catch (_: ArithmeticException) {
                    throw IOException("sparse chunk $index logical size overflow")
                }
                val payloadOffset = physicalCursor + chunkHeaderSize
                when (type) {
                    CHUNK_RAW -> {
                        if (totalSize != chunkHeaderSize.toLong() + logicalLength || logicalLength <= 0L) {
                            throw IOException("sparse raw chunk $index has invalid length")
                        }
                        indexed += Chunk(logicalCursor, logicalLength, CHUNK_RAW, payloadOffset)
                        logicalCursor = safeAdd(logicalCursor, logicalLength, "sparse logical offset")
                    }
                    CHUNK_FILL -> {
                        if (totalSize != chunkHeaderSize.toLong() + 4L || logicalLength <= 0L) {
                            throw IOException("sparse fill chunk $index has invalid length")
                        }
                        indexed += Chunk(logicalCursor, logicalLength, CHUNK_FILL, fill = source.readAt(payloadOffset, 4))
                        logicalCursor = safeAdd(logicalCursor, logicalLength, "sparse logical offset")
                    }
                    CHUNK_DONT_CARE -> {
                        if (totalSize != chunkHeaderSize.toLong() || logicalLength <= 0L) {
                            throw IOException("sparse don't-care chunk $index has invalid length")
                        }
                        indexed += Chunk(logicalCursor, logicalLength, CHUNK_DONT_CARE)
                        logicalCursor = safeAdd(logicalCursor, logicalLength, "sparse logical offset")
                    }
                    CHUNK_CRC32 -> {
                        if (chunkBlocks != 0L || totalSize != chunkHeaderSize.toLong() + 4L) {
                            throw IOException("sparse CRC chunk $index has invalid length")
                        }
                    }
                    else -> throw IOException("unknown Android sparse chunk type 0x${type.toString(16)}")
                }
                physicalCursor = safeAdd(physicalCursor, totalSize, "sparse physical offset")
            }
            if (logicalCursor != logicalSize) throw IOException("sparse chunk total does not match expanded image size")
            return logicalSize to indexed
        }
    }
}
