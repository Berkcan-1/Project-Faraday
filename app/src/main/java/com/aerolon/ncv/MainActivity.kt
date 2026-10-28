package com.aerolon.ncv

import android.app.Activity
import android.content.Intent
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast

/** Landing page and Storage Access Framework importer. No broad storage permission is requested. */
class MainActivity : Activity() {
    companion object {
        private const val REQUEST_PICK_IMAGE = 4101
        private const val PREFS = "ncv_local_state"
        private const val PREF_LAST_URI = "last_image_uri"
    }

    private var selectedImage: ImageMetadata? = null
    private lateinit var rootColumn: LinearLayout

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.statusBarColor = NcvUi.BG
        window.navigationBarColor = NcvUi.BG
        window.decorView.systemUiVisibility = 0
        restoreLastImage()
        render()
    }

    private fun restoreLastImage() {
        val stored = getSharedPreferences(PREFS, MODE_PRIVATE).getString(PREF_LAST_URI, null) ?: return
        try {
            selectedImage = ImageInspector.inspect(this, Uri.parse(stored))
        } catch (_: Exception) {
            selectedImage = null
        }
    }

    private fun render() {
        val scroll = ScrollView(this).apply {
            setBackgroundColor(NcvUi.BG)
            isFillViewport = true
            clipToPadding = false
            setPadding(0, 0, 0, NcvUi.dp(this@MainActivity, 18f))
        }
        rootColumn = NcvUi.vertical(this)
        rootColumn.setPadding(NcvUi.dp(this, 20f), NcvUi.dp(this, 18f), NcvUi.dp(this, 20f), NcvUi.dp(this, 16f))
        scroll.addView(rootColumn, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT))

        rootColumn.addView(buildBrandRow())
        rootColumn.addView(NcvUi.spacer(this, 24))
        rootColumn.addView(NcvUi.pill(this, "SUPER.IMG-FIRST  •  READ-ONLY", NcvUi.CYAN, 0x1F7CDBE8))
        rootColumn.addView(NcvUi.spacer(this, 15))
        rootColumn.addView(NcvUi.text(this, "Bring your ROM\ninto NCV.", 34f, NcvUi.TEXT, true).apply {
            setLineSpacing(NcvUi.dp(this@MainActivity, 0f).toFloat(), 0.98f)
        })
        rootColumn.addView(NcvUi.spacer(this, 10))
        rootColumn.addView(NcvUi.text(
            this,
            "Start with super.img. NCV reads its real logical partitions without asking for the OEM boot.img. Running Android's ART and framework is a separate runtime milestone, and this build does not pretend it is already working.",
            14f, NcvUi.MUTED
        ))
        rootColumn.addView(NcvUi.spacer(this, 22))
        rootColumn.addView(buildEngineCard())
        val bundledKernelAvailable = hasBundledAsset("ncv/guest/diagnostic/Image")
        val bundledInitramfsAvailable = hasBundledAsset("ncv/guest/diagnostic/initramfs.cpio.gzip")
        if (bundledKernelAvailable) {
            rootColumn.addView(NcvUi.spacer(this, 14))
            rootColumn.addView(buildBundledDiagnosticCard(bundledInitramfsAvailable))
        }
        rootColumn.addView(NcvUi.spacer(this, 16))
        rootColumn.addView(buildImportCard())
        selectedImage?.let {
            rootColumn.addView(NcvUi.spacer(this, 12))
            rootColumn.addView(buildSelectedImageCard(it))
        }
        rootColumn.addView(NcvUi.spacer(this, 18))
        rootColumn.addView(
            NcvUi.button(
                this,
                when {
                    selectedImage == null -> "Choose an image to continue  →"
                    selectedImage?.kind == ImageKind.LINUX_KERNEL -> "Try experimental kernel boot  →"
                    selectedImage?.let { isSuperImage(it) } == true -> "Open ROM workspace  →"
                    else -> "Open guest diagnostics  →"
                },
                primary = true
            ) {
                val image = selectedImage
                if (image == null) openPicker() else openGuestScreen(image)
            },
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)
        )
        rootColumn.addView(NcvUi.spacer(this, 14))
        rootColumn.addView(buildRequirementsCard())
        rootColumn.addView(NcvUi.spacer(this, 20))
        rootColumn.addView(NcvUi.text(this, "NCV  •  DEVELOPMENT BUILD  •  v0.1.20-dev", 10f, 0xFF647992.toInt(), true).apply {
            gravity = Gravity.CENTER
            letterSpacing = 0.08f
        })
        setContentView(scroll)
    }

    private fun buildBrandRow(): View {
        val row = NcvUi.horizontal(this)
        val mark = TextView(this).apply {
            text = "N"
            textSize = 21f
            setTextColor(NcvUi.BG)
            typeface = Typeface.create("sans-serif-black", Typeface.NORMAL)
            gravity = Gravity.CENTER
            background = GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(NcvUi.CYAN, NcvUi.BLUE)).apply {
                cornerRadius = NcvUi.dp(this@MainActivity, 15f).toFloat()
            }
        }
        row.addView(mark, LinearLayout.LayoutParams(NcvUi.dp(this, 44f), NcvUi.dp(this, 44f)))
        row.addView(NcvUi.rowSpacer(this, 11))
        val names = NcvUi.vertical(this)
        names.addView(NcvUi.text(this, "NCV", 17f, NcvUi.TEXT, true))
        names.addView(NcvUi.text(this, "VIRTUAL LAB", 9f, NcvUi.MUTED, true).apply { letterSpacing = 0.17f })
        row.addView(names, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        row.addView(NcvUi.pill(this, "PREVIEW", NcvUi.AMBER, 0x22FFC879))
        return row
    }


    private fun hasBundledAsset(path: String): Boolean = runCatching {
        assets.open(path).use { }
        true
    }.getOrDefault(false)

    private fun buildBundledDiagnosticCard(hasInitramfs: Boolean): View {
        val card = NcvUi.card(this, 17)
        card.addView(NcvUi.pill(this, "OPTIONAL BUILD ASSET", NcvUi.GREEN, 0x2275E0B0))
        card.addView(NcvUi.spacer(this, 9))
        card.addView(NcvUi.text(this, "Bundled Linux diagnostic", 18f, NcvUi.TEXT, true))
        card.addView(NcvUi.spacer(this, 5))
        card.addView(NcvUi.text(
            this,
            if (hasInitramfs) "This APK contains the build-generated ARM64 Linux Image and test initramfs. It is a diagnostic guest, not Android and not a GSI."
            else "This APK contains a build-generated ARM64 Linux Image but no test initramfs. It is a diagnostic guest, not Android and not a GSI.",
            12f,
            NcvUi.MUTED
        ))
        card.addView(NcvUi.spacer(this, 10))
        card.addView(NcvUi.button(this, "Try bundled Linux kernel  →", primary = true) {
            startActivity(Intent(this, GuestConsoleActivity::class.java).apply {
                putExtra(GuestConsoleActivity.EXTRA_URI, "")
                putExtra(GuestConsoleActivity.EXTRA_NAME, "Bundled NCV diagnostic Linux Image")
                putExtra(GuestConsoleActivity.EXTRA_KIND, ImageKind.LINUX_KERNEL.name)
                putExtra(GuestConsoleActivity.EXTRA_ASSET_PATH, "ncv/guest/diagnostic/Image")
                if (hasInitramfs) putExtra(GuestConsoleActivity.EXTRA_INITRD_ASSET_PATH, "ncv/guest/diagnostic/initramfs.cpio.gzip")
            })
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        return card
    }

    private fun buildEngineCard(): View {
        val card = NcvUi.card(this, 17)
        card.background = NcvUi.gradient(this, 0xFF142B42.toInt(), 0xFF142035.toInt(), 22f)
        val row = NcvUi.horizontal(this)
        val icon = TextView(this).apply {
            text = "◎"
            textSize = 25f
            setTextColor(NcvUi.CYAN)
            gravity = Gravity.CENTER
            background = NcvUi.rounded(0x337CDBE8, NcvUi.dp(this@MainActivity, 15f))
        }
        row.addView(icon, LinearLayout.LayoutParams(NcvUi.dp(this, 46f), NcvUi.dp(this, 46f)))
        row.addView(NcvUi.rowSpacer(this, 12))
        val labels = NcvUi.vertical(this)
        labels.addView(NcvUi.text(this, "NCV engine", 15f, NcvUi.TEXT, true))
        labels.addView(NcvUi.text(this, "Software-emulated ARM64", 12f, NcvUi.MUTED))
        row.addView(labels, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        card.addView(row)
        card.addView(NcvUi.spacer(this, 15))
        card.addView(NcvUi.divider(this, 0x332C4765))
        card.addView(NcvUi.spacer(this, 13))
        card.addView(NcvUi.text(this, "CURRENT CAPABILITY", 10f, NcvUi.MUTED, true).apply { letterSpacing = 0.11f })
        card.addView(NcvUi.spacer(this, 7))
        card.addView(NcvUi.text(this, "liblp metadata  ·  logical partition reader  ·  ARM64 diagnostics", 13f, NcvUi.TEXT))
        card.addView(NcvUi.spacer(this, 8))
        card.addView(NcvUi.text(this, "ROM partition bytes can be read through real extents without changing the image. Android init, ART, Binder services and the launcher are not yet running inside NCV.", 12f, NcvUi.AMBER))
        return card
    }

    private fun buildImportCard(): View {
        val card = NcvUi.card(this, 17)
        val top = NcvUi.horizontal(this)
        val icon = TextView(this).apply {
            text = "↑"
            textSize = 26f
            setTextColor(NcvUi.CYAN)
            gravity = Gravity.CENTER
            background = NcvUi.rounded(0x1F7CDBE8, NcvUi.dp(this@MainActivity, 13f))
        }
        top.addView(icon, LinearLayout.LayoutParams(NcvUi.dp(this, 44f), NcvUi.dp(this, 44f)))
        top.addView(NcvUi.rowSpacer(this, 12))
        val labels = NcvUi.vertical(this)
        labels.addView(NcvUi.text(this, "Android ROM container", 16f, NcvUi.TEXT, true))
        labels.addView(NcvUi.text(this, "super.img / system.img / Linux Image", 12f, NcvUi.MUTED))
        top.addView(labels, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        card.addView(top)
        card.addView(NcvUi.spacer(this, 13))
        card.addView(NcvUi.text(this, "Choose super.img to open the ROM workspace and inspect its real logical partitions. The selected file is read-only; NCV does not request broad storage access. Other image types remain available for low-level diagnostics.", 13f, NcvUi.MUTED))
        card.addView(NcvUi.spacer(this, 16))
        card.addView(NcvUi.button(this, if (selectedImage == null) "Choose image file" else "Choose another image", primary = false) {
            openPicker()
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        return card
    }

    private fun buildSelectedImageCard(image: ImageMetadata): View {
        val card = NcvUi.card(this, 17)
        val titleRow = NcvUi.horizontal(this)
        val kindColor = when (image.kind) {
            ImageKind.LINUX_KERNEL -> NcvUi.GREEN
            ImageKind.GSI_SYSTEM_IMAGE -> NcvUi.CYAN
            ImageKind.ANDROID_BOOT_IMAGE, ImageKind.ANDROID_PARTITION_IMAGE -> NcvUi.AMBER
            else -> NcvUi.MUTED
        }
        titleRow.addView(NcvUi.text(this, "SELECTED IMAGE", 10f, NcvUi.MUTED, true), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        titleRow.addView(NcvUi.pill(this, image.kind.name.replace('_', ' '), kindColor, 0x22263855))
        card.addView(titleRow)
        card.addView(NcvUi.spacer(this, 13))
        card.addView(NcvUi.text(this, image.displayName, 16f, NcvUi.TEXT, true))
        card.addView(NcvUi.spacer(this, 5))
        card.addView(NcvUi.text(this, "${image.formatDescription}  ·  ${NcvUi.formatBytes(image.sizeBytes)}", 12f, kindColor))
        card.addView(NcvUi.spacer(this, 10))
        card.addView(NcvUi.text(this, image.details, 12f, NcvUi.MUTED))
        card.addView(NcvUi.spacer(this, 13))
        card.addView(NcvUi.divider(this))
        card.addView(NcvUi.spacer(this, 12))
        card.addView(NcvUi.button(this, "Forget imported file", primary = false) {
            selectedImage = null
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().remove(PREF_LAST_URI).apply()
            render()
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        return card
    }

    private fun buildRequirementsCard(): View {
        val card = NcvUi.card(this, 17)
        card.addView(NcvUi.text(this, "REAL ANDROID EXECUTION MILESTONES", 10f, NcvUi.MUTED, true).apply { letterSpacing = 0.08f })
        card.addView(NcvUi.spacer(this, 13))
        addRequirement(card, "01", "super.img reader", "liblp metadata plus read-only logical-partition byte reads")
        addRequirement(card, "02", "Generic guest kernel", "NCV must supply and successfully boot its own Android-compatible kernel/initramfs; not wired to super.img yet")
        addRequirement(card, "03", "Android userspace", "init, Bionic, Binder, ART and system services must actually execute")
        addRequirement(card, "04", "Real Android display", "SurfaceFlinger/HWC-compatible buffers, input, storage and vendor-HAL compatibility still required")
        return card
    }

    private fun addRequirement(card: LinearLayout, number: String, heading: String, caption: String) {
        val row = NcvUi.horizontal(this)
        row.gravity = Gravity.TOP
        val badge = TextView(this).apply {
            text = number
            textSize = 10f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(NcvUi.CYAN)
            gravity = Gravity.CENTER
            background = NcvUi.rounded(0x227CDBE8, NcvUi.dp(this@MainActivity, 8f))
        }
        row.addView(badge, LinearLayout.LayoutParams(NcvUi.dp(this, 31f), NcvUi.dp(this, 31f)))
        row.addView(NcvUi.rowSpacer(this, 10))
        val labels = NcvUi.vertical(this)
        labels.addView(NcvUi.text(this, heading, 13f, NcvUi.TEXT, true))
        labels.addView(NcvUi.spacer(this, 3))
        labels.addView(NcvUi.text(this, caption, 11f, NcvUi.MUTED))
        row.addView(labels, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        card.addView(row)
        card.addView(NcvUi.spacer(this, 13))
    }

    private fun openPicker() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
        }
        try {
            startActivityForResult(intent, REQUEST_PICK_IMAGE)
        } catch (error: Exception) {
            Toast.makeText(this, "Could not open the document picker: ${error.message ?: "unknown error"}", Toast.LENGTH_LONG).show()
        }
    }

    @Deprecated("Kept compatible with the dependency-free platform Activity API")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_PICK_IMAGE || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        try {
            val persistFlags = (data.flags and Intent.FLAG_GRANT_READ_URI_PERMISSION)
            if (persistFlags != 0) contentResolver.takePersistableUriPermission(uri, persistFlags)
        } catch (_: Exception) {
            // The document provider may grant a transient permission only; current selection still works.
        }
        try {
            val inspected = ImageInspector.inspect(this, uri)
            selectedImage = inspected
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(PREF_LAST_URI, uri.toString()).apply()
            render()
            // A super.img opens the ROM-first workspace; other images retain their diagnostic flow.
            openGuestScreen(inspected)
        } catch (error: Exception) {
            Toast.makeText(this, "Could not inspect image: ${error.message ?: "unknown error"}", Toast.LENGTH_LONG).show()
        }
    }

    private fun isSuperImage(image: ImageMetadata): Boolean =
        image.kind == ImageKind.ANDROID_PARTITION_IMAGE && image.displayName.lowercase().contains("super")

    private fun openGuestScreen(image: ImageMetadata) {
        if (isSuperImage(image)) {
            startActivity(Intent(this, RomWorkspaceActivity::class.java).apply {
                putExtra(RomWorkspaceActivity.EXTRA_URI, image.uri.toString())
                putExtra(RomWorkspaceActivity.EXTRA_NAME, image.displayName)
            })
            return
        }
        startActivity(Intent(this, GuestConsoleActivity::class.java).apply {
            putExtra(GuestConsoleActivity.EXTRA_URI, image.uri.toString())
            putExtra(GuestConsoleActivity.EXTRA_NAME, image.displayName)
            putExtra(GuestConsoleActivity.EXTRA_KIND, image.kind.name)
            putExtra(GuestConsoleActivity.EXTRA_FORMAT, image.formatDescription)
        })
    }
}
