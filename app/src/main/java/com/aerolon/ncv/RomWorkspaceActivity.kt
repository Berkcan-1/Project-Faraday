package com.aerolon.ncv

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import java.io.IOException
import java.util.concurrent.Executors

/**
 * super.img-first workspace. This screen performs real, read-only liblp/extent reads; it never
 * reports an Android boot until a real guest kernel + Android userspace runtime are connected.
 */
class RomWorkspaceActivity : Activity() {
    companion object {
        const val EXTRA_URI = "ncv.rom.uri"
        const val EXTRA_NAME = "ncv.rom.name"
        private const val PICK_SUPER_IMAGE = 6301
        private const val PICK_BOOT_IMAGE = 6302
        private const val PICK_VENDOR_BOOT_IMAGE = 6303
        private const val PICK_INIT_BOOT_IMAGE = 6304
    }

    private val worker = Executors.newSingleThreadExecutor()
    @Volatile private var disposed = false
    private var selectedUri: Uri? = null
    private var selectedName: String = "No super image selected"
    private var selectedBootUri: Uri? = null
    private var selectedBootName: String? = null
    private var selectedVendorBootUri: Uri? = null
    private var selectedVendorBootName: String? = null
    private var selectedInitBootUri: Uri? = null
    private var selectedInitBootName: String? = null
    private var openedImage: OpenSuperImage? = null
    private var summary: SuperImageSummary? = null
    private var loading = false
    private var loadError: String? = null
    private var partitionProbe = "Not run yet."
    private lateinit var rootColumn: LinearLayout

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.statusBarColor = NcvUi.BG
        window.navigationBarColor = NcvUi.BG
        selectedUri = intent.getStringExtra(EXTRA_URI)?.takeIf { it.isNotBlank() }?.let { Uri.parse(it) }
        selectedName = intent.getStringExtra(EXTRA_NAME)?.takeIf { it.isNotBlank() } ?: selectedName
        render()
        selectedUri?.let { analyzeSuperImage(it, selectedName) }
    }

    private fun render() {
        val scroll = ScrollView(this).apply {
            setBackgroundColor(NcvUi.BG)
            clipToPadding = false
            isFillViewport = true
            setPadding(0, 0, 0, NcvUi.dp(this@RomWorkspaceActivity, 20f))
        }
        rootColumn = NcvUi.vertical(this)
        rootColumn.setPadding(NcvUi.dp(this, 20f), NcvUi.dp(this, 18f), NcvUi.dp(this, 20f), NcvUi.dp(this, 16f))
        scroll.addView(rootColumn, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT))

        val top = NcvUi.horizontal(this)
        top.addView(NcvUi.button(this, "←  Library", primary = false) { finish() })
        top.addView(NcvUi.rowSpacer(this, 10))
        top.addView(NcvUi.text(this, "ROM WORKSPACE", 10f, NcvUi.MUTED, true).apply { letterSpacing = 0.12f })
        rootColumn.addView(top)
        rootColumn.addView(NcvUi.spacer(this, 22))
        rootColumn.addView(NcvUi.pill(this, "SUPER.IMG-FIRST  ·  READ-ONLY", NcvUi.CYAN, 0x1F7CDBE8))
        rootColumn.addView(NcvUi.spacer(this, 13))
        rootColumn.addView(NcvUi.text(this, "Bring a ROM\ninto NCV.", 32f, NcvUi.TEXT, true).apply {
            setLineSpacing(NcvUi.dp(this@RomWorkspaceActivity, 0f).toFloat(), 0.98f)
        })
        rootColumn.addView(NcvUi.spacer(this, 9))
        rootColumn.addView(NcvUi.text(
            this,
            "Inspect super.img and its real logical partitions without a boot image. For the separate guest-kernel experiment below, provide matching boot components from the same firmware build. Android userspace will never be marked as started without real evidence.",
            13f, NcvUi.MUTED
        ))
        rootColumn.addView(NcvUi.spacer(this, 18))
        rootColumn.addView(buildImageCard())
        rootColumn.addView(NcvUi.spacer(this, 13))
        rootColumn.addView(buildGuestRuntimeCard())
        rootColumn.addView(NcvUi.spacer(this, 13))
        rootColumn.addView(buildRuntimeTruthCard())
        summary?.let {
            rootColumn.addView(NcvUi.spacer(this, 13))
            rootColumn.addView(buildPartitionsCard(it))
            rootColumn.addView(NcvUi.spacer(this, 13))
            rootColumn.addView(buildReaderProbeCard())
        }
        rootColumn.addView(NcvUi.spacer(this, 18))
        rootColumn.addView(NcvUi.text(this, "NCV · ROM WORKSPACE · DEVELOPMENT BUILD", 9f, 0xFF647992.toInt(), true).apply {
            gravity = Gravity.CENTER
            letterSpacing = 0.06f
        })
        setContentView(scroll)
    }

    private fun buildImageCard(): View {
        val card = NcvUi.card(this, 17)
        card.addView(NcvUi.text(this, "SELECTED ROM CONTAINER", 10f, NcvUi.MUTED, true).apply { letterSpacing = 0.1f })
        card.addView(NcvUi.spacer(this, 10))
        card.addView(NcvUi.text(this, selectedName, 16f, NcvUi.TEXT, true))
        card.addView(NcvUi.spacer(this, 5))
        val sizeText = selectedUri?.let { uri ->
            runCatching { contentResolver.query(uri, arrayOf(android.provider.OpenableColumns.SIZE), null, null, null)?.use { c ->
                if (c.moveToFirst() && !c.isNull(0)) ImageInspector.formatBytes(c.getLong(0)) else "size unavailable"
            } }.getOrNull() ?: "size unavailable"
        } ?: "No file selected"
        card.addView(NcvUi.text(this, sizeText, 12f, NcvUi.MUTED))
        card.addView(NcvUi.spacer(this, 12))
        val stateLabel = when {
            loading -> "READING METADATA AND EXTENTS…"
            loadError != null -> "ROM ANALYSIS FAILED"
            summary != null -> "LIBLP METADATA VERIFIED"
            else -> "WAITING FOR SUPER.IMG"
        }
        val stateColor = when {
            loading -> NcvUi.AMBER
            loadError != null -> NcvUi.RED
            summary != null -> NcvUi.GREEN
            else -> NcvUi.MUTED
        }
        card.addView(NcvUi.pill(this, stateLabel, stateColor, 0x22263855))
        if (loading) {
            card.addView(NcvUi.spacer(this, 10))
            card.addView(NcvUi.text(this, "Reading the selected file without modifying it.", 12f, NcvUi.MUTED))
        }
        loadError?.let {
            card.addView(NcvUi.spacer(this, 10))
            card.addView(NcvUi.text(this, it, 12f, NcvUi.RED))
        }
        card.addView(NcvUi.spacer(this, 14))
        card.addView(NcvUi.button(this, if (selectedUri == null) "Choose super.img" else "Choose another super.img", primary = summary == null && !loading, enabled = !loading) {
            openPicker()
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        if (summary != null && !loading) {
            card.addView(NcvUi.spacer(this, 8))
            card.addView(NcvUi.button(this, "Re-read ROM metadata", primary = false) {
                selectedUri?.let { analyzeSuperImage(it, selectedName) }
            }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        }
        return card
    }


    private fun buildGuestRuntimeCard(): View {
        val card = NcvUi.card(this, 17)
        card.addView(NcvUi.pill(this, "GUEST RUNTIME · EXPERIMENTAL", NcvUi.CYAN, 0x227CDBE8))
        card.addView(NcvUi.spacer(this, 10))
        card.addView(NcvUi.text(this, "Prepare a real kernel boot attempt", 18f, NcvUi.TEXT, true))
        card.addView(NcvUi.spacer(this, 6))
        card.addView(NcvUi.text(
            this,
            "Select the matching boot.img from the same firmware build. vendor_boot.img and init_boot.img are conditional inputs based on that build's ramdisk and DTB layout. NCV will parse the real images and start its software-emulated ARM64 guest; Android userspace boot is not yet validated.",
            12f, NcvUi.MUTED
        ))
        card.addView(NcvUi.spacer(this, 13))
        card.addView(NcvUi.text(this, "BOOT PROFILE", 10f, NcvUi.MUTED, true).apply { letterSpacing = 0.1f })
        card.addView(NcvUi.spacer(this, 7))
        val profileText = buildString {
            append("SUPER       ").append(selectedName).append('\n')
            append("BOOT        ").append(selectedBootName ?: "Required · not selected").append('\n')
            append("VENDOR BOOT ").append(selectedVendorBootName ?: "Optional · not selected").append('\n')
            append("INIT BOOT   ").append(selectedInitBootName ?: "Optional · not selected")
        }
        card.addView(NcvUi.text(this, profileText, 11f, NcvUi.TEXT, mono = true))
        card.addView(NcvUi.spacer(this, 12))
        card.addView(NcvUi.button(
            this,
            if (selectedBootName == null) "Choose matching boot.img" else "Boot: $selectedBootName · Change",
            primary = false
        ) { openComponentPicker(PICK_BOOT_IMAGE) }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(NcvUi.spacer(this, 7))
        card.addView(NcvUi.button(
            this,
            if (selectedVendorBootName == null) "Choose vendor_boot.img (if required)" else "Vendor boot: $selectedVendorBootName · Change",
            primary = false
        ) { openComponentPicker(PICK_VENDOR_BOOT_IMAGE) }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(NcvUi.spacer(this, 7))
        card.addView(NcvUi.button(
            this,
            if (selectedInitBootName == null) "Choose init_boot.img (if required)" else "Init boot: $selectedInitBootName · Change",
            primary = false
        ) { openComponentPicker(PICK_INIT_BOOT_IMAGE) }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(NcvUi.spacer(this, 12))
        val canContinue = selectedUri != null && summary != null && selectedBootUri != null && !loading && loadError == null
        card.addView(NcvUi.button(
            this,
            "Open Guest Runtime controls  →",
            primary = true,
            enabled = canContinue
        ) { openGuestRuntime() }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(NcvUi.spacer(this, 7))
        val note = when {
            loading -> "Wait for super.img metadata validation to finish."
            summary == null -> "First, choose and validate super.img."
            selectedBootUri == null -> "A boot.img is required for the current kernel-entry experiment."
            else -> "Inputs selected. Compatibility remains unverified; continue to inspect boot logs and start the guest explicitly."
        }
        card.addView(NcvUi.text(this, note, 11f, if (canContinue) NcvUi.AMBER else NcvUi.MUTED))
        return card
    }

    private fun openGuestRuntime() {
        val superUri = selectedUri ?: return
        val bootUri = selectedBootUri ?: return
        if (summary == null || loading || loadError != null) {
            Toast.makeText(this, "Validate super.img before opening the guest runtime.", Toast.LENGTH_LONG).show()
            return
        }
        startActivity(Intent(this, GuestConsoleActivity::class.java).apply {
            putExtra(GuestConsoleActivity.EXTRA_URI, superUri.toString())
            putExtra(GuestConsoleActivity.EXTRA_NAME, selectedName)
            putExtra(GuestConsoleActivity.EXTRA_KIND, ImageKind.ANDROID_PARTITION_IMAGE.name)
            putExtra(GuestConsoleActivity.EXTRA_FORMAT, "Android super image (dynamic partitions)")
            putExtra(GuestConsoleActivity.EXTRA_SUPER_URI, superUri.toString())
            putExtra(GuestConsoleActivity.EXTRA_SUPER_NAME, selectedName)
            putExtra(GuestConsoleActivity.EXTRA_BOOT_URI, bootUri.toString())
            putExtra(GuestConsoleActivity.EXTRA_BOOT_NAME, selectedBootName)
            selectedVendorBootUri?.let { putExtra(GuestConsoleActivity.EXTRA_VENDOR_BOOT_URI, it.toString()) }
            selectedVendorBootName?.let { putExtra(GuestConsoleActivity.EXTRA_VENDOR_BOOT_NAME, it) }
            selectedInitBootUri?.let { putExtra(GuestConsoleActivity.EXTRA_INIT_BOOT_URI, it.toString()) }
            selectedInitBootName?.let { putExtra(GuestConsoleActivity.EXTRA_INIT_BOOT_NAME, it) }
        })
    }

    private fun buildRuntimeTruthCard(): View {
        val card = NcvUi.card(this, 17)
        card.addView(NcvUi.text(this, "REAL EXECUTION STATUS", 10f, NcvUi.MUTED, true).apply { letterSpacing = 0.1f })
        card.addView(NcvUi.spacer(this, 12))
        addStatusRow(card, if (summary != null) "✓" else "○", "super.img / liblp metadata", if (summary != null) "Parsed with metadata checksums and bounds checks." else "Select an image to validate dynamic-partition metadata.", if (summary != null) NcvUi.GREEN else NcvUi.MUTED)
        card.addView(NcvUi.spacer(this, 12))
        val readerDetails = when {
            partitionProbe.startsWith("PASS · ") -> partitionProbe.removePrefix("PASS · ")
            partitionProbe.startsWith("BLOCKED · ") -> partitionProbe.removePrefix("BLOCKED · ")
            else -> "The ROM reader will test real bytes from a mapped logical partition after metadata parsing."
        }
        addStatusRow(card, if (partitionProbe.startsWith("PASS")) "✓" else if (partitionProbe.startsWith("BLOCKED")) "!" else "○", "Read-only logical partition reader", readerDetails, if (partitionProbe.startsWith("PASS")) NcvUi.GREEN else if (partitionProbe.startsWith("BLOCKED")) NcvUi.AMBER else NcvUi.MUTED)
        card.addView(NcvUi.spacer(this, 12))
        addStatusRow(card, "!", "Android guest runtime", "Not implemented end-to-end: NCV does not yet launch Android init, ART, Binder services or SurfaceFlinger from this super.img-only workflow.", NcvUi.AMBER)
        card.addView(NcvUi.spacer(this, 12))
        val bootDetails = when {
            selectedBootName != null -> "Selected: $selectedBootName. NCV will parse the actual Android boot container in Guest Runtime; compatibility is unverified."
            else -> "Choose the matching boot.img in Guest Runtime to attempt real kernel entry. vendor_boot/init_boot are conditional inputs."
        }
        addStatusRow(card, if (selectedBootName != null) "✓" else "!", "Boot image input", bootDetails, if (selectedBootName != null) NcvUi.CYAN else NcvUi.AMBER)
        card.addView(NcvUi.spacer(this, 13))
        card.addView(NcvUi.text(this, "No Android boot is claimed on this screen. A parsed partition table or a readable filesystem signature is not proof that ART/framework is running.", 11f, NcvUi.MUTED))
        return card
    }

    private fun buildPartitionsCard(info: SuperImageSummary): View {
        val card = NcvUi.card(this, 17)
        card.addView(NcvUi.text(this, "LOGICAL PARTITIONS", 10f, NcvUi.MUTED, true).apply { letterSpacing = 0.1f })
        card.addView(NcvUi.spacer(this, 7))
        card.addView(NcvUi.text(this, "${info.partitions.size} partitions · ${ImageInspector.formatBytes(info.logicalImageSizeBytes)} logical image · liblp ${info.metadataMajorVersion}.${info.metadataMinorVersion}", 12f, NcvUi.MUTED))
        card.addView(NcvUi.spacer(this, 13))
        info.partitions.forEachIndexed { index, partition ->
            val row = NcvUi.horizontal(this).apply { gravity = Gravity.TOP }
            val icon = TextView(this).apply {
                text = when {
                    partition.usesExternalBlockDevice -> "↗"
                    partition.hasZeroExtents -> "◌"
                    else -> "▦"
                }
                textSize = 15f
                setTextColor(if (partition.usesExternalBlockDevice) NcvUi.AMBER else NcvUi.CYAN)
                gravity = Gravity.CENTER
                background = NcvUi.rounded(0x227CDBE8, NcvUi.dp(this@RomWorkspaceActivity, 9f))
            }
            row.addView(icon, LinearLayout.LayoutParams(NcvUi.dp(this, 34f), NcvUi.dp(this, 34f)))
            row.addView(NcvUi.rowSpacer(this, 10))
            val labels = NcvUi.vertical(this)
            labels.addView(NcvUi.text(this, partition.name, 14f, NcvUi.TEXT, true, mono = true))
            val attributes = buildList {
                add("${partition.extentCount} extent(s)")
                if (partition.readOnly) add("ROM read-only flag")
                if (partition.disabled) add("disabled by liblp metadata")
                if (partition.sizeBytes == 0L) add("empty partition")
                if (partition.usesExternalBlockDevice) add("external backing device required")
                if (partition.hasZeroExtents) add("zero-filled extent(s)")
            }.joinToString(" · ")
            labels.addView(NcvUi.text(this, "${ImageInspector.formatBytes(partition.sizeBytes)} · $attributes", 11f, if (partition.usesExternalBlockDevice) NcvUi.AMBER else NcvUi.MUTED))
            row.addView(labels, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            card.addView(row)
            if (index != info.partitions.lastIndex) {
                card.addView(NcvUi.spacer(this, 11))
                card.addView(NcvUi.divider(this, 0x332C4765))
                card.addView(NcvUi.spacer(this, 11))
            }
        }
        return card
    }

    private fun buildReaderProbeCard(): View {
        val card = NcvUi.card(this, 17)
        card.addView(NcvUi.text(this, "PARTITION READ TEST", 10f, NcvUi.MUTED, true).apply { letterSpacing = 0.1f })
        card.addView(NcvUi.spacer(this, 9))
        card.addView(NcvUi.text(this, partitionProbe, 12f, when {
            partitionProbe.startsWith("PASS") -> NcvUi.GREEN
            partitionProbe.startsWith("BLOCKED") -> NcvUi.AMBER
            else -> NcvUi.MUTED
        }, mono = true))
        card.addView(NcvUi.spacer(this, 12))
        card.addView(NcvUi.button(this, "Read real partition bytes again", primary = false, enabled = !loading && summary != null) {
            probePartitionBytes()
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(NcvUi.spacer(this, 7))
        card.addView(NcvUi.text(this, "This reads mapped bytes from the selected image. It does not mount ext4/EROFS, execute binaries, or change the image.", 11f, NcvUi.MUTED))
        return card
    }

    private fun addStatusRow(card: LinearLayout, marker: String, heading: String, details: String, color: Int) {
        val row = NcvUi.horizontal(this).apply { gravity = Gravity.TOP }
        row.addView(NcvUi.text(this, marker, 18f, color, true).apply { gravity = Gravity.TOP }, LinearLayout.LayoutParams(NcvUi.dp(this, 24f), LinearLayout.LayoutParams.WRAP_CONTENT))
        row.addView(NcvUi.rowSpacer(this, 8))
        val labels = NcvUi.vertical(this)
        labels.addView(NcvUi.text(this, heading, 13f, NcvUi.TEXT, true))
        labels.addView(NcvUi.spacer(this, 3))
        labels.addView(NcvUi.text(this, details, 11f, color))
        row.addView(labels, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        card.addView(row)
    }

    private fun analyzeSuperImage(uri: Uri, name: String) {
        if (loading) return
        loading = true
        loadError = null
        summary = null
        partitionProbe = "Reading metadata…"
        selectedUri = uri
        selectedName = name
        openedImage?.close()
        openedImage = null
        render()
        worker.execute {
            var opened: OpenSuperImage? = null
            try {
                val imageHandle = SuperImageAnalyzer.open(this, uri)
                opened = imageHandle
                val parsed = imageHandle.layout.summary
                val probe = runCatching { readProbe(imageHandle, parsed) }.getOrElse {
                    "BLOCKED · ${it.message ?: it.javaClass.simpleName}"
                }
                val accepted = imageHandle
                runOnUiThread {
                    if (disposed) {
                        accepted.close()
                        return@runOnUiThread
                    }
                    openedImage = accepted
                    summary = parsed
                    partitionProbe = probe
                    loading = false
                    render()
                }
            } catch (error: Throwable) {
                val message = error.message ?: error.javaClass.simpleName
                opened?.close()
                runOnUiThread {
                    if (disposed) return@runOnUiThread
                    loading = false
                    loadError = message
                    partitionProbe = "No partition reads completed."
                    render()
                }
            }
        }
    }

    private fun probePartitionBytes() {
        val opened = openedImage ?: return
        val info = summary ?: return
        if (loading) return
        loading = true
        partitionProbe = "Reading logical partition bytes…"
        render()
        worker.execute {
            val probe = try { readProbe(opened, info) } catch (error: Throwable) {
                "BLOCKED · ${error.message ?: error.javaClass.simpleName}"
            }
            runOnUiThread {
                if (disposed) return@runOnUiThread
                partitionProbe = probe
                loading = false
                render()
            }
        }
    }

    private fun readProbe(opened: OpenSuperImage, info: SuperImageSummary): String {
        val usablePartitions = info.partitions.filter { !it.disabled && it.sizeBytes > 0L }
        val partition = usablePartitions.firstOrNull { it.name == "system" }
            ?: usablePartitions.firstOrNull { it.name.startsWith("system_") }
            ?: usablePartitions.firstOrNull()
            ?: throw IOException("No non-empty, enabled logical partition is available for a read probe.")
        val reader = opened.layout.openPartition(partition.name)
        val bytesToRead = minOf(4096L, partition.sizeBytes).toInt()
        if (bytesToRead <= 0) throw IOException("Partition '${partition.name}' is empty.")
        val bytes = reader.readAt(0, bytesToRead)
        if (bytes.size != bytesToRead) throw IOException("Short logical partition read.")
        val fs = detectFilesystem(reader, partition.sizeBytes)
        return "PASS · Read $bytesToRead bytes from logical partition '${partition.name}' through ${partition.extentCount} liblp extent(s). Filesystem probe: $fs. Image remains read-only."
    }

    private fun detectFilesystem(reader: SuperImagePartitionReader, sizeBytes: Long): String {
        if (sizeBytes < 1152) return "partition too small for a standard superblock probe"
        val bytes = reader.readAt(1024, 128)
        val magic = (bytes[0].toInt() and 0xff) or
            ((bytes[1].toInt() and 0xff) shl 8) or
            ((bytes[2].toInt() and 0xff) shl 16) or
            ((bytes[3].toInt() and 0xff) shl 24)
        if (magic == 0xE0F5E1E2.toInt()) return "EROFS signature detected"
        if ((bytes[56].toInt() and 0xff) == 0x53 && (bytes[57].toInt() and 0xff) == 0xEF) return "ext4 signature detected"
        return "no ext4/EROFS signature at the standard superblock offset"
    }

    private fun openPicker() {
        val picker = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
        }
        try {
            startActivityForResult(picker, PICK_SUPER_IMAGE)
        } catch (error: Exception) {
            Toast.makeText(this, "Could not open file picker: ${error.message ?: "unknown error"}", Toast.LENGTH_LONG).show()
        }
    }

    private fun openComponentPicker(requestCode: Int) {
        val picker = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
        }
        try {
            startActivityForResult(picker, requestCode)
        } catch (error: Exception) {
            Toast.makeText(this, "Could not open image picker: ${error.message ?: "unknown error"}", Toast.LENGTH_LONG).show()
        }
    }

    @Deprecated("Kept compatible with the dependency-free platform Activity API")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        try {
            val flags = data.flags and Intent.FLAG_GRANT_READ_URI_PERMISSION
            if (flags != 0) contentResolver.takePersistableUriPermission(uri, flags)
        } catch (_: Exception) {
            // A transient read permission may still work for this activity session.
        }
        val name = runCatching {
            contentResolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) cursor.getString(0).orEmpty() else "Selected image"
            }
        }.getOrNull().orEmpty().ifBlank { "Selected image" }
        when (requestCode) {
            PICK_SUPER_IMAGE -> {
                if (selectedUri != uri) {
                    // Avoid accidentally combining boot components from a different firmware build.
                    selectedBootUri = null; selectedBootName = null
                    selectedVendorBootUri = null; selectedVendorBootName = null
                    selectedInitBootUri = null; selectedInitBootName = null
                }
                analyzeSuperImage(uri, name)
            }
            PICK_BOOT_IMAGE, PICK_VENDOR_BOOT_IMAGE, PICK_INIT_BOOT_IMAGE -> {
                val metadata = runCatching { ImageInspector.inspect(this, uri) }.getOrElse {
                    Toast.makeText(this, "Could not inspect boot component: ${it.message ?: it.javaClass.simpleName}", Toast.LENGTH_LONG).show()
                    return
                }
                if (metadata.kind != ImageKind.ANDROID_BOOT_IMAGE) {
                    Toast.makeText(this, "This does not look like a supported Android boot container. Select a matching boot.img/vendor_boot.img/init_boot.img.", Toast.LENGTH_LONG).show()
                    return
                }
                when (requestCode) {
                    PICK_BOOT_IMAGE -> { selectedBootUri = uri; selectedBootName = metadata.displayName }
                    PICK_VENDOR_BOOT_IMAGE -> { selectedVendorBootUri = uri; selectedVendorBootName = metadata.displayName }
                    PICK_INIT_BOOT_IMAGE -> { selectedInitBootUri = uri; selectedInitBootName = metadata.displayName }
                }
                render()
            }
        }
    }

    override fun onDestroy() {
        disposed = true
        worker.shutdownNow()
        runCatching { openedImage?.close() }
        openedImage = null
        super.onDestroy()
    }
}
