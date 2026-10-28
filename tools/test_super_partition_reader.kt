package com.aerolon.ncv

import java.io.Closeable
import java.io.File
import java.io.IOException
import java.io.RandomAccessFile

/** Host-side smoke test for metadata and read-only logical-partition extent mapping. */
private class FileImageSource(path: String) : SeekableImageDataSource, Closeable {
    private val file = RandomAccessFile(File(path), "r")
    override val physicalSizeBytes: Long = file.length()

    @Synchronized
    override fun readAt(offset: Long, length: Int): ByteArray {
        if (offset < 0L || length < 0 || offset > physicalSizeBytes || length.toLong() > physicalSizeBytes - offset) {
            throw IOException("Read outside the selected image")
        }
        val output = ByteArray(length)
        file.seek(offset)
        file.readFully(output)
        return output
    }

    override fun close() = file.close()
}

fun main(args: Array<String>) {
    require(args.isNotEmpty()) { "Usage: <super.img> [super.img ...]" }
    args.forEach { path ->
        FileImageSource(path).use { source ->
            val layout = SuperImageMetadataParser.open(source)
            println("${File(path).name}: liblp ${layout.summary.metadataMajorVersion}.${layout.summary.metadataMinorVersion}; " +
                "slot=${layout.summary.metadataSlot}; sparse=${layout.summary.sparseEncoded}; " +
                "partitions=${layout.summary.partitions.size}")
            layout.summary.partitions.forEach { partition ->
                val length = minOf(4096L, partition.sizeBytes).toInt()
                val bytes = layout.openPartition(partition.name).readAt(0, length)
                check(bytes.size == length) { "short read from ${partition.name}" }
                println("  PASS ${partition.name.padEnd(24)} ${partition.sizeBytes} bytes; " +
                    "read=$length; extents=${partition.extentCount}; disabled=${partition.disabled}; " +
                    "external=${partition.usesExternalBlockDevice}; zero=${partition.hasZeroExtents}")
            }
        }
    }
}
