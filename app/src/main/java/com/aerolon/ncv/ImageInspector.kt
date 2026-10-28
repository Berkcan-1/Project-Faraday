package com.aerolon.ncv

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Locale

/** Image classes that the UI can identify without pretending that every .img is bootable. */
enum class ImageKind {
    LINUX_KERNEL,
    GSI_SYSTEM_IMAGE,
    ANDROID_BOOT_IMAGE,
    ANDROID_PARTITION_IMAGE,
    ARCHIVE,
    UNKNOWN
}

data class ImageMetadata(
    val uri: Uri,
    val displayName: String,
    val sizeBytes: Long?,
    val kind: ImageKind,
    val formatDescription: String,
    val details: String
)

object ImageInspector {
    private val ANDROID_SPARSE_MAGIC = 0xED26FF3A.toInt()
    private const val MAX_KERNEL_INPUT_BYTES = 64L * 1024 * 1024

    fun inspect(context: Context, uri: Uri): ImageMetadata {
        val (nameFromProvider, size) = queryMetadata(context, uri)
        val name = nameFromProvider.ifBlank { uri.lastPathSegment?.substringAfterLast('/') ?: "Selected image" }
        val lower = name.lowercase(Locale.ROOT)
        val header = readHeader(context, uri)
        val sparse = header.size >= 28 && leInt(header, 0) == ANDROID_SPARSE_MAGIC
        val ext4 = header.size >= 1082 && (header[1080].toInt() and 0xff) == 0x53 && (header[1081].toInt() and 0xff) == 0xef
        val linuxMagic = header.size >= 64 && leInt(header, 56) == 0x644d5241
        val gzip = header.size >= 2 && (header[0].toInt() and 0xff) == 0x1f && (header[1].toInt() and 0xff) == 0x8b
        val androidBootMagic = header.size >= 8 && String(header, 0, 8, Charsets.US_ASCII) == "ANDROID!"
        val vendorBootMagic = header.size >= 8 && String(header, 0, 8, Charsets.US_ASCII) == "VNDRBOOT"

        val kind: ImageKind
        val format: String
        val details: String
        when {
            lower.contains("vendor_boot") || lower == "boot.img" || androidBootMagic || vendorBootMagic -> {
                kind = ImageKind.ANDROID_BOOT_IMAGE
                format = "Android boot container"
                details = "Android boot container recognized. NCV parses supported boot.img/vendor_boot structures; for Android 13+ launch devices, also select the matching init_boot.img. Matching virtual-board compatibility is not guaranteed."
            }
            linuxMagic -> {
                kind = ImageKind.LINUX_KERNEL
                format = "ARM64 Linux Image"
                details = "Kernel header signature detected. NCV can attempt its experimental direct-kernel path; Linux boot has not yet been proven."
            }
            lower.endsWith("image.gz") || lower == "image.gz" || lower.endsWith("vmlinuz.gz") -> {
                kind = ImageKind.LINUX_KERNEL
                format = if (gzip) "Gzip-compressed candidate kernel" else "Compressed kernel candidate"
                details = "The native loader will validate the decompressed ARM64 Image header before loading."
            }
            lower.contains("super") && (sparse || ext4 || lower.endsWith(".img") || lower.endsWith(".simg")) -> {
                kind = ImageKind.ANDROID_PARTITION_IMAGE
                format = "Android super image (dynamic partitions)"
                details = "Android super container recognized. NCV validates raw or Android sparse liblp metadata and can read bytes from logical partitions such as system, system_ext, vendor, product and odm through their real extents. This is read-only partition access, not a filesystem mount or Android boot; device-mapper guest devices, AVB/verity and the Android userspace runtime are not implemented end-to-end."
            }
            lower.contains("system") && (sparse || ext4 || lower.endsWith(".img") || lower.endsWith(".simg")) -> {
                kind = ImageKind.GSI_SYSTEM_IMAGE
                format = when {
                    sparse -> "Android sparse image"
                    ext4 -> "Raw ext4 filesystem image"
                    else -> "System partition image (format not confirmed)"
                }
                details = buildString {
                    append("GSI/system partition imported. This is not a kernel. NCV can attach supported images as read-only virtio block devices and parse supported boot containers, but a compatible Android kernel/vendor setup and graphical display are still unverified.")
                    if (sparse && header.size >= 28) {
                        val blockSize = leInt(header, 12).toLong() and 0xffffffffL
                        val blocks = leInt(header, 16).toLong() and 0xffffffffL
                        val chunks = leInt(header, 20).toLong() and 0xffffffffL
                        val expandedSize = if (blocks == 0L || blockSize <= Long.MAX_VALUE / blocks) blockSize * blocks else -1L
                        append(" Sparse layout: block size ").append(blockSize)
                            .append(" B, expanded size ").append(if (expandedSize >= 0) formatBytes(expandedSize) else "overflow/unknown")
                            .append(", ").append(chunks).append(" chunks.")
                    }
                }
            }
            lower.endsWith(".zip") -> {
                kind = ImageKind.ARCHIVE
                format = "Archive"
                details = "Archive selected. Automatic archive extraction is not implemented; select boot.img and system.img/super.img individually, then add init_boot.img/vendor_boot.img only if the target build requires their contents."
            }
            sparse || ext4 -> {
                kind = ImageKind.ANDROID_PARTITION_IMAGE
                format = if (sparse) "Android sparse partition image" else "Raw ext4 filesystem image"
                details = "Partition image detected. NCV can attach supported raw or Android sparse images as read-only guest virtio block devices; confirm that this partition matches the selected boot bundle."
            }
            gzip -> {
                kind = ImageKind.UNKNOWN
                format = "Gzip data"
                details = "Gzip signature found, but the file name/header does not identify it as a Linux ARM64 Image."
            }
            else -> {
                kind = ImageKind.UNKNOWN
                format = "Unrecognized image"
                details = "The file was imported safely for inspection, but its format could not be identified."
            }
        }

        val detailWithSize = if (size != null) "$details\nFile size: ${formatBytes(size)}." else details
        return ImageMetadata(uri, name, size, kind, format, detailWithSize)
    }

    @Throws(IOException::class)
    fun readBounded(context: Context, uri: Uri, maxBytes: Long = MAX_KERNEL_INPUT_BYTES): ByteArray {
        val output = ByteArrayOutputStream()
        context.contentResolver.openInputStream(uri)?.use { input ->
            val buffer = ByteArray(64 * 1024)
            var total = 0L
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                if (count == 0) continue
                total += count
                if (total > maxBytes) throw IOException("File exceeds the ${formatBytes(maxBytes)} safety limit.")
                output.write(buffer, 0, count)
            }
        } ?: throw IOException("Android could not open the selected document.")
        if (output.size() == 0) throw IOException("The selected file is empty.")
        return output.toByteArray()
    }

    fun formatBytes(bytes: Long): String {
        if (bytes < 1024) return "$bytes B"
        val units = arrayOf("KB", "MB", "GB", "TB")
        var value = bytes.toDouble()
        var index = -1
        do {
            value /= 1024.0
            index++
        } while (value >= 1024 && index < units.lastIndex)
        return String.format(Locale.US, "%.1f %s", value, units[index])
    }

    private fun queryMetadata(context: Context, uri: Uri): Pair<String, Long?> {
        return try {
            context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) {
                    val nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    val sizeIndex = cursor.getColumnIndex(OpenableColumns.SIZE)
                    val name = if (nameIndex >= 0) cursor.getString(nameIndex).orEmpty() else ""
                    val size = if (sizeIndex >= 0 && !cursor.isNull(sizeIndex)) cursor.getLong(sizeIndex) else null
                    name to size
                } else "" to null
            } ?: ("" to null)
        } catch (_: Exception) {
            "" to null
        }
    }

    private fun readHeader(context: Context, uri: Uri): ByteArray {
        return try {
            context.contentResolver.openInputStream(uri)?.use { input ->
                val buffer = ByteArray(4096)
                var count = 0
                while (count < buffer.size) {
                    val read = input.read(buffer, count, buffer.size - count)
                    if (read < 0) break
                    if (read == 0) break
                    count += read
                }
                buffer.copyOf(count)
            } ?: ByteArray(0)
        } catch (_: Exception) {
            ByteArray(0)
        }
    }

    private fun leInt(bytes: ByteArray, offset: Int): Int {
        return ByteBuffer.wrap(bytes, offset, 4).order(ByteOrder.LITTLE_ENDIAN).int
    }
}
