package com.aerolon.ncv

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import java.io.Closeable
import java.io.IOException

/** Owns the selected read-only document handle for the lifetime of the partition readers. */
class OpenSuperImage internal constructor(
    private val descriptor: ParcelFileDescriptor,
    val layout: SuperImageLayout
) : Closeable {
    internal val rawFileDescriptor: Int get() = descriptor.fd
    override fun close() = descriptor.close()
}

/** Opens a user-selected image read-only and supplies random access to the pure Kotlin liblp parser. */
object SuperImageAnalyzer {
    fun inspect(context: Context, uri: Uri): SuperImageSummary = open(context, uri).use { it.layout.summary }

    /** Opens a persistent, read-only logical view; callers must close it when finished. */
    fun open(context: Context, uri: Uri): OpenSuperImage {
        val descriptor = context.contentResolver.openFileDescriptor(uri, "r")
            ?: throw IOException("Android could not open the selected super image.")
        try {
            val source = AndroidFdImageDataSource(descriptor)
            val layout = SuperImageMetadataParser.open(source)
            return OpenSuperImage(descriptor, layout)
        } catch (error: Throwable) {
            runCatching { descriptor.close() }
            throw error
        }
    }

    private class AndroidFdImageDataSource(private val descriptor: ParcelFileDescriptor) : SeekableImageDataSource {
        override val physicalSizeBytes: Long = try {
            Os.fstat(descriptor.fileDescriptor).st_size
        } catch (error: ErrnoException) {
            throw IOException("Could not determine super image size: ${error.message}", error)
        }

        @Synchronized
        override fun readAt(offset: Long, length: Int): ByteArray {
            if (offset < 0 || length < 0 || offset > physicalSizeBytes || length.toLong() > physicalSizeBytes - offset) {
                throw IOException("Read request is outside the selected image.")
            }
            val result = ByteArray(length)
            try {
                Os.lseek(descriptor.fileDescriptor, offset, OsConstants.SEEK_SET)
                var done = 0
                while (done < length) {
                    val count = Os.read(descriptor.fileDescriptor, result, done, length - done)
                    if (count <= 0) throw IOException("Unexpected end of image while reading data.")
                    done += count
                }
            } catch (error: ErrnoException) {
                throw IOException(
                    "The selected document provider does not support random-access reads (${error.message}). Copy the image to local storage and retry.",
                    error
                )
            }
            return result
        }
    }
}
