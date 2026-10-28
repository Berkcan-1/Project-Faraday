package com.aerolon.ncv

import android.app.Activity
import android.content.Intent
import android.content.ClipData
import android.content.ClipboardManager
import android.graphics.Typeface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.view.Gravity
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.io.RandomAccessFile
import java.util.concurrent.Executors

/**
 * Guest view. A GSI partition currently opens a capability/report screen, not a fake Android desktop.
 * The native engine's experimental raw ARM64 Linux Image direct-boot path can be attempted separately.
 */
class GuestConsoleActivity : Activity() {
    companion object {
        const val EXTRA_URI = "ncv.image.uri"
        const val EXTRA_NAME = "ncv.image.name"
        const val EXTRA_KIND = "ncv.image.kind"
        const val EXTRA_FORMAT = "ncv.image.format"
        const val EXTRA_ASSET_PATH = "ncv.image.asset_path"
        const val EXTRA_INITRD_ASSET_PATH = "ncv.image.initrd_asset_path"
        const val EXTRA_SUPER_URI = "ncv.guest.super.uri"
        const val EXTRA_SUPER_NAME = "ncv.guest.super.name"
        const val EXTRA_BOOT_URI = "ncv.guest.boot.uri"
        const val EXTRA_BOOT_NAME = "ncv.guest.boot.name"
        const val EXTRA_VENDOR_BOOT_URI = "ncv.guest.vendor_boot.uri"
        const val EXTRA_VENDOR_BOOT_NAME = "ncv.guest.vendor_boot.name"
        const val EXTRA_INIT_BOOT_URI = "ncv.guest.init_boot.uri"
        const val EXTRA_INIT_BOOT_NAME = "ncv.guest.init_boot.name"
        private const val MAX_CONSOLE_CHARS = 24000
        // Keep a much larger bounded session history for the one-tap copy action.
        private const val MAX_COPY_LOG_CHARS = 2_000_000
        private const val PICK_SYSTEM_IMAGE = 5201
        private const val PICK_BOOT_IMAGE = 5202
        private const val PICK_VENDOR_BOOT_IMAGE = 5203
        private const val PICK_VENDOR_PARTITION = 5204
        private const val PICK_INIT_BOOT_IMAGE = 5205
        private const val PICK_SUPER_IMAGE = 5206
        private const val PICK_INITRD_IMAGE = 5207
        private const val MAX_BOOT_COMPONENT_BYTES = 128L * 1024 * 1024
        private const val MAX_KERNEL_IMAGE_BYTES = 256L * 1024 * 1024
        private const val PRIVATE_USERDATA_SIZE_BYTES = 1024L * 1024L * 1024L // 1 GiB app-private raw disk
        private const val MAX_INITRD_BYTES = 64L * 1024L * 1024L
        private const val EXPERIMENTAL_INITRD_ADDRESS = 0x48000000L
        private const val NCV_DIAGNOSTIC_KERNEL_ASSET = "ncv/guest/diagnostic/Image"
        private const val NCV_DIAGNOSTIC_INITRD_ASSET = "ncv/guest/diagnostic/initramfs.cpio.gzip"
        private const val EXPECTED_NATIVE_ENGINE = "0.1.24-dev-llvm-path-autodetect-20261028"
    }

    private val mainHandler = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private var vm: NcvVm? = null
    @Volatile private var disposed = false
    private lateinit var imageUri: android.net.Uri
    private var imageAssetPath: String? = null
    private var embeddedInitrdAssetPath: String? = null
    private lateinit var imageName: String
    private lateinit var imageKind: ImageKind
    private lateinit var screenStatus: TextView
    private lateinit var screenDetail: TextView
    private lateinit var framebufferView: GuestFramebufferView
    private lateinit var terminal: TextView
    private val fullSessionLog = StringBuilder()
    private var sessionLogTruncated = false
    private lateinit var pauseButton: android.widget.Button
    private lateinit var stopButton: android.widget.Button
    private var pollEnabled = false
    private var lastRuntimeReportAtMs = 0L
    @Volatile private var guestStartedAtMs = 0L
    private var guestSawUartOutput = false
    private var guestHasVirtioMmioActivity = false
    private var guestHasBlockReads = false
    private var guestHasFramebufferWrites = false
    private var storageGateWarningLogged = false
    private var initGateWarningLogged = false
    private var graphicsGateWarningLogged = false
    private var runtimeBootArgsLogged = false
    private var bootAttemptInProgress = false
    private var selectedSystemUri: android.net.Uri? = null
    private var selectedSuperUri: android.net.Uri? = null
    private var selectedBootUri: android.net.Uri? = null
    private var selectedVendorBootUri: android.net.Uri? = null
    private var selectedInitBootUri: android.net.Uri? = null
    private var selectedVendorPartitionUri: android.net.Uri? = null
    private var selectedSystemName: String? = null
    private var selectedSuperName: String? = null
    private var selectedBootName: String? = null
    private var selectedVendorBootName: String? = null
    private var selectedInitBootName: String? = null
    private var selectedVendorName: String? = null
    private var parsedSuperMetadata: SuperImageSummary? = null
    private var superMetadataError: String? = null
    private val queuedSuperAnalyses = mutableSetOf<String>()
    private var selectedInitrdUri: android.net.Uri? = null
    private var selectedInitrdName: String? = null
    private lateinit var bootProfileSummary: TextView
    private lateinit var systemImageButton: Button
    private lateinit var superImageButton: Button
    private lateinit var userdataButton: Button
    private lateinit var bootImageButton: Button
    private lateinit var vendorBootImageButton: Button
    private lateinit var initBootImageButton: Button
    private lateinit var vendorPartitionButton: Button
    private lateinit var startAndroidButton: Button
    private lateinit var startNcvKernelAndroidButton: Button
    private lateinit var startNcvSuperBringupButton: Button
    private lateinit var diagnosticBuildStatus: TextView
    private lateinit var initrdButton: Button
    private lateinit var startLinuxButton: Button
    private lateinit var linuxBootSummary: TextView
    private lateinit var compatProfileSpinner: Spinner
    private lateinit var guestRamSpinner: Spinner
    private lateinit var dtbModeSpinner: Spinner
    private var selectedGuestRamMiB: Int = 2048
    private var selectedCompatProfileId: Int = 1
    // OEM Android path defaults to the phone DTB + MTK probe fallback. The diagnostic Linux path always builds a clean NCV DTB.
    private var selectedDtbModeId: Int = 1
    private val diagnosticGuestAssetsAvailable: Boolean by lazy(LazyThreadSafetyMode.NONE) {
        isBundledAssetReadable(NCV_DIAGNOSTIC_KERNEL_ASSET) && isBundledAssetReadable(NCV_DIAGNOSTIC_INITRD_ASSET)
    }

    private val pollRunnable = object : Runnable {
        override fun run() {
            if (!pollEnabled || disposed) return
            val current = vm
            if (current == null) {
                mainHandler.postDelayed(this, 350)
                return
            }
            try {
                val output = current.console(take = true)
                if (output.isNotEmpty()) {
                    guestSawUartOutput = true
                    appendTerminal(output)
                }
                when (val state = current.state()) {
                    VmState.RUNNING -> {
                        if (::framebufferView.isInitialized) framebufferView.setArgbPixels(current.framebufferArgb())
                        val now = SystemClock.elapsedRealtime()
                        updateRunningGuestStatus(now)
                        if (!runtimeBootArgsLogged) {
                            appendTerminal("[NCV] Effective kernel command line: ${current.bootArgs()}\n")
                            runtimeBootArgsLogged = true
                        }
                        if (now - lastRuntimeReportAtMs >= 5000L) {
                            val runtimeSnapshot = current.runtimeDiagnostics()
                            val blockSnapshot = current.blockDevicesDiagnostics()
                            appendTerminal("[NCV] Runtime snapshot: $runtimeSnapshot\n")
                            appendTerminal("$blockSnapshot\n")
                            val mmioReads = Regex("mmio_reads=(\\d+)").findAll(blockSnapshot)
                                .mapNotNull { it.groupValues.getOrNull(1)?.toLongOrNull() }.toList()
                            val diskReads = Regex("read_requests=(\\d+)").findAll(blockSnapshot)
                                .mapNotNull { it.groupValues.getOrNull(1)?.toLongOrNull() }.toList()
                            guestHasVirtioMmioActivity = guestHasVirtioMmioActivity || mmioReads.any { it > 0L }
                            guestHasBlockReads = guestHasBlockReads || diskReads.any { it > 0L }
                            val fbWrites = Regex("fb_write_ops=(\\d+)").find(runtimeSnapshot)?.groupValues?.getOrNull(1)?.toLongOrNull()
                            guestHasFramebufferWrites = guestHasFramebufferWrites || (fbWrites != null && fbWrites > 0L)
                            updateRunningGuestStatus(now)
                            if (now - guestStartedAtMs >= 20_000L) {
                                if (!storageGateWarningLogged && mmioReads.isNotEmpty() && mmioReads.all { it == 0L }) {
                                    appendTerminal("[NCV STORAGE GATE] After 20 seconds the guest has made zero virtio-MMIO register reads. The guest kernel has not probed these block devices; dm-linear/init cannot proceed through them yet. Check DTB discovery and whether this kernel was built with virtio-mmio support.\n")
                                    storageGateWarningLogged = true
                                } else if (!storageGateWarningLogged && diskReads.isNotEmpty() && diskReads.all { it == 0L } && mmioReads.any { it > 0L }) {
                                    appendTerminal("[NCV STORAGE GATE] virtio-MMIO was probed, but no block read request reached the backing image yet. The guest has not demonstrated usable super-image I/O.\n")
                                    storageGateWarningLogged = true
                                }
                                if (!graphicsGateWarningLogged && (fbWrites == null || fbWrites == 0L)) {
                                    appendTerminal("[NCV GRAPHICS GATE] The guest has not written to NCV's simple framebuffer. This says nothing about Mali/HWC support; the Android graphics pipeline is not emulated yet.\n")
                                    graphicsGateWarningLogged = true
                                }
                                if (!initGateWarningLogged && !guestSawUartOutput && mmioReads.isNotEmpty() && mmioReads.all { it == 0L }) {
                                    appendTerminal("[NCV INIT GATE] No UART output and no virtio-MMIO probes were observed after 20 seconds. Guest CPU activity alone does not prove that Android init or userspace has started.\n")
                                    initGateWarningLogged = true
                                }
                            }
                            if (selectedCompatProfileId == 1) {
                                appendTerminal("[NCV] MTK MMIO probe snapshot: ${current.mtkProbeReport()}\n")
                            }
                            lastRuntimeReportAtMs = now
                        }
                        mainHandler.postDelayed(this, 350)
                    }
                    VmState.PAUSED -> {
                        if (::framebufferView.isInitialized) framebufferView.setArgbPixels(current.framebufferArgb())
                        screenStatus.text = "GUEST PAUSED"
                        screenStatus.setTextColor(NcvUi.AMBER)
                        mainHandler.postDelayed(this, 600)
                    }
                    VmState.STOPPED, VmState.FAULTED -> {
                        screenStatus.text = if (state == VmState.FAULTED) "GUEST FAULTED" else "GUEST STOPPED"
                        screenStatus.setTextColor(if (state == VmState.FAULTED) NcvUi.RED else NcvUi.AMBER)
                        appendTerminal("\n\n[NCV] $state\n${current.stopInfo()}\n")
                        appendTerminal("[NCV] ${current.compatibilityReport()}\n")
                        pauseButton.isEnabled = false
                        pauseButton.alpha = 0.45f
                        stopButton.isEnabled = true
                        stopButton.alpha = 1f
                        stopButton.text = "Back to library"
                        stopButton.setOnClickListener { finish() }
                        pollEnabled = false
                        vm = null
                        bootAttemptInProgress = false
                        refreshBootProfileSummary()
                        worker.execute { runCatching { current.close() } }
                    }
                    else -> mainHandler.postDelayed(this, 600)
                }
            } catch (error: Exception) {
                screenStatus.text = "MONITOR ERROR"
                screenStatus.setTextColor(NcvUi.RED)
                appendTerminal("\n[NCV monitor error] ${error.message ?: error.javaClass.simpleName}\n")
                mainHandler.postDelayed(this, 1000)
            }
        }
    }

    private fun updateRunningGuestStatus(now: Long) {
        val elapsed = if (guestStartedAtMs > 0L) now - guestStartedAtMs else 0L
        when {
            guestHasFramebufferWrites -> {
                screenStatus.text = "KERNEL WRITES FRAMEBUFFER · ANDROID UNCONFIRMED"
                screenStatus.setTextColor(NcvUi.CYAN)
                screenDetail.text = "The guest has written pixels to NCV's simple framebuffer. This is real guest activity, but it does not prove SurfaceFlinger or the Android desktop has started."
            }
            guestHasBlockReads -> {
                screenStatus.text = "GUEST STORAGE I/O ACTIVE · ANDROID NOT REACHED"
                screenStatus.setTextColor(NcvUi.AMBER)
                screenDetail.text = "The guest kernel is issuing real block reads. The next gate is whether first-stage init can mount the required Android partitions and start userspace."
            }
            guestSawUartOutput -> {
                screenStatus.text = "GUEST KERNEL CONSOLE ACTIVE · ANDROID UNCONFIRMED"
                screenStatus.setTextColor(NcvUi.CYAN)
                screenDetail.text = "Real guest UART output was received. Android userspace and graphics are still not confirmed."
            }
            elapsed >= 20_000L && !guestHasVirtioMmioActivity -> {
                screenStatus.text = "KERNEL EXECUTING · STORAGE NOT PROBED"
                screenStatus.setTextColor(NcvUi.AMBER)
                screenDetail.text = "The CPU is executing ARM64 instructions, but the guest has not read any virtio-MMIO device registers. This is the current boot blocker; no Android UI code has been reached."
            }
            else -> {
                screenStatus.text = "KERNEL EXECUTING · ANDROID NOT REACHED"
                screenStatus.setTextColor(NcvUi.AMBER)
                screenDetail.text = "The vCPU is active. NCV will report actual UART, storage, and framebuffer activity; a running CPU alone does not prove an Android boot."
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.statusBarColor = NcvUi.BG
        window.navigationBarColor = NcvUi.BG
        imageUri = android.net.Uri.parse(intent.getStringExtra(EXTRA_URI).orEmpty())
        imageAssetPath = intent.getStringExtra(EXTRA_ASSET_PATH)
        embeddedInitrdAssetPath = intent.getStringExtra(EXTRA_INITRD_ASSET_PATH)
        imageName = intent.getStringExtra(EXTRA_NAME) ?: "Selected image"
        if (embeddedInitrdAssetPath != null) selectedInitrdName = "Bundled diagnostic initramfs"
        imageKind = runCatching { ImageKind.valueOf(intent.getStringExtra(EXTRA_KIND) ?: "UNKNOWN") }.getOrDefault(ImageKind.UNKNOWN)
        when (imageKind) {
            ImageKind.GSI_SYSTEM_IMAGE -> { selectedSystemUri = imageUri; selectedSystemName = imageName }
            ImageKind.ANDROID_BOOT_IMAGE -> {
                when {
                    imageName.lowercase().contains("vendor_boot") -> { selectedVendorBootUri = imageUri; selectedVendorBootName = imageName }
                    imageName.lowercase().contains("init_boot") -> { selectedInitBootUri = imageUri; selectedInitBootName = imageName }
                    else -> { selectedBootUri = imageUri; selectedBootName = imageName }
                }
            }
            ImageKind.ANDROID_PARTITION_IMAGE -> {
                when {
                    imageName.lowercase().contains("super") -> { selectedSuperUri = imageUri; selectedSuperName = imageName }
                    imageName.lowercase().contains("vendor") -> { selectedVendorPartitionUri = imageUri; selectedVendorName = imageName }
                    else -> { selectedSystemUri = imageUri; selectedSystemName = imageName }
                }
            }
            else -> Unit
        }
        // ROM Workspace can hand off a complete selection without forcing the user to pick the
        // already-selected super/boot files a second time.
        intent.getStringExtra(EXTRA_SUPER_URI)?.takeIf { it.isNotBlank() }?.let {
            selectedSuperUri = android.net.Uri.parse(it)
            selectedSuperName = intent.getStringExtra(EXTRA_SUPER_NAME) ?: imageName
            selectedSystemUri = null
            selectedSystemName = null
            selectedVendorPartitionUri = null
            selectedVendorName = null
        }
        intent.getStringExtra(EXTRA_BOOT_URI)?.takeIf { it.isNotBlank() }?.let {
            selectedBootUri = android.net.Uri.parse(it)
            selectedBootName = intent.getStringExtra(EXTRA_BOOT_NAME) ?: "boot.img"
        }
        intent.getStringExtra(EXTRA_VENDOR_BOOT_URI)?.takeIf { it.isNotBlank() }?.let {
            selectedVendorBootUri = android.net.Uri.parse(it)
            selectedVendorBootName = intent.getStringExtra(EXTRA_VENDOR_BOOT_NAME) ?: "vendor_boot.img"
        }
        intent.getStringExtra(EXTRA_INIT_BOOT_URI)?.takeIf { it.isNotBlank() }?.let {
            selectedInitBootUri = android.net.Uri.parse(it)
            selectedInitBootName = intent.getStringExtra(EXTRA_INIT_BOOT_NAME) ?: "init_boot.img"
        }
        buildScreen()
        refreshBootProfileSummary()

        if (imageKind == ImageKind.LINUX_KERNEL) {
            showLinuxKernelReadyState()
        } else if (selectedBootUri != null && (selectedSuperUri != null || selectedSystemUri != null)) {
            showGuestRuntimeConfiguredState()
        } else {
            showNotBootableState()
        }
        selectedSuperUri?.let { analyzeSuperImageAsync(it, selectedSuperName ?: imageName) }
    }

    private fun buildScreen() {
        val scroll = ScrollView(this).apply {
            setBackgroundColor(NcvUi.BG)
            clipToPadding = false
            setPadding(0, 0, 0, NcvUi.dp(this@GuestConsoleActivity, 20f))
        }
        val page = NcvUi.vertical(this)
        page.setPadding(NcvUi.dp(this, 20f), NcvUi.dp(this, 18f), NcvUi.dp(this, 20f), NcvUi.dp(this, 12f))
        scroll.addView(page, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT))

        val top = NcvUi.horizontal(this)
        top.addView(NcvUi.button(this, "←  Library", primary = false) { finish() }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        top.addView(NcvUi.rowSpacer(this, 10))
        top.addView(NcvUi.text(this, "GUEST RUNTIME", 10f, NcvUi.MUTED, true).apply { letterSpacing = 0.1f }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        top.gravity = Gravity.CENTER_VERTICAL
        page.addView(top)
        page.addView(NcvUi.spacer(this, 22))
        page.addView(NcvUi.text(this, "Guest Runtime", 28f, NcvUi.TEXT, true))
        page.addView(NcvUi.spacer(this, 6))
        page.addView(NcvUi.text(this, imageName, 13f, NcvUi.MUTED))
        page.addView(NcvUi.spacer(this, 18))

        val screenCard = NcvUi.card(this, 12)
        val display = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            background = NcvUi.gradient(this@GuestConsoleActivity, 0xFF0F1B2B.toInt(), 0xFF080B13.toInt(), 18f)
            setPadding(NcvUi.dp(this@GuestConsoleActivity, 20f), NcvUi.dp(this@GuestConsoleActivity, 24f), NcvUi.dp(this@GuestConsoleActivity, 20f), NcvUi.dp(this@GuestConsoleActivity, 24f))
        }
        display.addView(NcvUi.pill(this, "NCV  /  GUEST DISPLAY", NcvUi.CYAN, 0x227CDBE8))
        display.addView(NcvUi.spacer(this, 18))
        display.addView(NcvUi.text(this, "GUEST FRAMEBUFFER · 640 × 480", 10f, NcvUi.MUTED, true))
        display.addView(NcvUi.spacer(this, 8))
        framebufferView = GuestFramebufferView(this)
        display.addView(framebufferView, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, NcvUi.dp(this, 180f)))
        display.addView(NcvUi.spacer(this, 12))
        screenStatus = NcvUi.text(this, "CHECKING IMAGE", 13f, NcvUi.AMBER, true).apply {
            gravity = Gravity.CENTER
            letterSpacing = 0.08f
        }
        display.addView(screenStatus)
        display.addView(NcvUi.spacer(this, 8))
        screenDetail = NcvUi.text(this, "The guest display will report the actual engine state.", 12f, NcvUi.MUTED).apply {
            gravity = Gravity.CENTER
        }
        display.addView(screenDetail)
        screenCard.addView(display, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, NcvUi.dp(this, 390f)))
        page.addView(screenCard)
        page.addView(NcvUi.spacer(this, 14))
        val profileCard = buildAndroidBootProfileCard()
        profileCard.visibility = if (imageKind == ImageKind.GSI_SYSTEM_IMAGE ||
            imageKind == ImageKind.ANDROID_BOOT_IMAGE || imageKind == ImageKind.ANDROID_PARTITION_IMAGE) View.VISIBLE else View.GONE
        page.addView(profileCard)
        val linuxCard = buildLinuxBootCard()
        linuxCard.visibility = if (imageKind == ImageKind.LINUX_KERNEL) View.VISIBLE else View.GONE
        page.addView(linuxCard)
        page.addView(NcvUi.spacer(this, 14))

        val terminalCard = NcvUi.card(this, 15)
        val terminalHeader = NcvUi.horizontal(this)
        terminalHeader.addView(NcvUi.text(this, "SERIAL CONSOLE", 10f, NcvUi.MUTED, true), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        terminalHeader.addView(NcvUi.pill(this, "UART", NcvUi.GREEN, 0x2275E0B0))
        terminalCard.addView(terminalHeader)
        terminalCard.addView(NcvUi.spacer(this, 8))
        terminalCard.addView(NcvUi.button(this, "Tümünü kopyala", primary = false) {
            copyAllSessionLog()
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        terminalCard.addView(NcvUi.spacer(this, 12))
        terminal = NcvUi.text(this, "[NCV] Session console initialized.\n", 11f, NcvUi.GREEN, mono = true).apply {
            gravity = Gravity.TOP or Gravity.START
            setPadding(NcvUi.dp(this@GuestConsoleActivity, 12f), NcvUi.dp(this@GuestConsoleActivity, 12f), NcvUi.dp(this@GuestConsoleActivity, 12f), NcvUi.dp(this@GuestConsoleActivity, 12f))
            background = NcvUi.rounded(0xFF050A10.toInt(), NcvUi.dp(this@GuestConsoleActivity, 12f))
            minHeight = NcvUi.dp(this@GuestConsoleActivity, 140f)
        }
        terminalCard.addView(terminal, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        page.addView(terminalCard)
        page.addView(NcvUi.spacer(this, 12))

        val controls = NcvUi.horizontal(this)
        pauseButton = NcvUi.button(this, "Pause", primary = false) { pauseOrResume() }
        stopButton = NcvUi.button(this, "Stop session", primary = false) { stopSession() }
        controls.addView(pauseButton, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        controls.addView(NcvUi.rowSpacer(this, 10))
        controls.addView(stopButton, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        page.addView(controls)
        page.addView(NcvUi.spacer(this, 12))
        page.addView(NcvUi.text(this, "This view shows pixels the guest writes to NCV’s simple framebuffer. It is not a Mali GPU, Android HWC, or proof that the Android desktop has booted.", 11f, NcvUi.MUTED))
        setContentView(scroll)
    }

    private fun buildLinuxBootCard(): LinearLayout {
        val card = NcvUi.card(this, 16)
        card.addView(NcvUi.text(this, "LINUX KERNEL BRING-UP", 10f, NcvUi.MUTED, true).apply { letterSpacing = 0.1f })
        card.addView(NcvUi.spacer(this, 7))
        card.addView(NcvUi.text(this, "Select a real ARM64 Linux Image. An optional gzip-compressed cpio initramfs can provide /init and a shell. This tests kernel entry and the NCV UART/DTB; it does not boot Android or a GSI.", 12f, NcvUi.MUTED))
        card.addView(NcvUi.spacer(this, 12))
        linuxBootSummary = NcvUi.text(this, "Kernel: $imageName\nInitramfs: not selected", 11f, NcvUi.TEXT, mono = true)
        card.addView(linuxBootSummary)
        card.addView(NcvUi.spacer(this, 10))
        initrdButton = NcvUi.button(this, "Select initramfs.cpio.gz (optional)", primary = false) {
            openComponentPicker(PICK_INITRD_IMAGE)
        }
        card.addView(initrdButton, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(NcvUi.spacer(this, 8))
        startLinuxButton = NcvUi.button(this, "Attempt Linux kernel boot →", primary = true) {
            beginExperimentalKernelBoot()
        }
        card.addView(startLinuxButton, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        return card
    }

    private fun refreshLinuxBootSummary() {
        if (!::linuxBootSummary.isInitialized) return
        linuxBootSummary.text = "Kernel: $imageName\nInitramfs: ${selectedInitrdName ?: "not selected"}\nMax initramfs: ${ImageInspector.formatBytes(MAX_INITRD_BYTES)}"
        initrdButton.text = selectedInitrdName?.let { "Initramfs: $it · Change" } ?: "Select initramfs.cpio.gz (optional)"
        val canStart = !bootAttemptInProgress && vm == null
        initrdButton.isEnabled = canStart
        startLinuxButton.isEnabled = canStart
        startLinuxButton.alpha = if (canStart) 1f else 0.55f
    }

    private fun showLinuxKernelReadyState() {
        screenStatus.text = "ARM64 KERNEL READY · NOT STARTED"
        screenStatus.setTextColor(NcvUi.CYAN)
        screenDetail.text = "Optionally attach a cpio.gz initramfs, then start the kernel attempt. Only actual UART output can confirm progress."
        appendTerminal("[NCV] ARM64 Linux Image selected: $imageName\n[READY] Kernel loader and NCV DTB path are available.\n[NEXT] Optional: select an initramfs.cpio.gz containing /init, then attempt boot.\n[LIMIT] No real kernel boot has yet been validated; this is not Android/GSI boot.\n")
        pauseButton.isEnabled = false
        pauseButton.alpha = 0.45f
        stopButton.text = "Back to library"
        stopButton.setOnClickListener { finish() }
        refreshLinuxBootSummary()
    }

    private fun showGuestRuntimeConfiguredState() {
        screenStatus.text = "GUEST PROFILE CONFIGURED · NOT STARTED"
        screenStatus.setTextColor(NcvUi.CYAN)
        screenDetail.text = "The selected boot bundle is ready for an explicit kernel-entry attempt. Android init, logical system partitions, and graphical userspace remain unverified."
        appendTerminal(
            "[NCV] Guest Runtime profile received from ROM Workspace.\n" +
            "[PROFILE] super.img: ${selectedSuperName ?: "not selected"}\n" +
            "[PROFILE] boot.img: ${selectedBootName ?: "not selected"}\n" +
            "[PROFILE] vendor_boot.img: ${selectedVendorBootName ?: "not selected"}\n" +
            "[PROFILE] init_boot.img: ${selectedInitBootName ?: "not selected"}\n" +
            "[READY] Inputs selected; no guest has started yet.\n" +
            "[LIMIT] super.img will be exposed via a read-only synthetic GPT partition named super; guest virtio/GPT support, by-name discovery, dm-linear, and Android userspace are not verified end-to-end.\n"
        )
        pauseButton.isEnabled = false
        pauseButton.alpha = 0.45f
        stopButton.text = "Back to ROM Workspace"
        stopButton.setOnClickListener { finish() }
        refreshBootProfileSummary()
    }

    private fun showNotBootableState() {
        pollEnabled = false
        when (imageKind) {
            ImageKind.GSI_SYSTEM_IMAGE -> {
                screenStatus.text = "GSI IMPORTED · BOOT PROFILE NEEDED"
                screenStatus.setTextColor(NcvUi.AMBER)
                screenDetail.text = "Select boot.img plus system.img or super.img. A boot.img-only profile is allowed when it embeds a non-empty ramdisk; vendor_boot.img and init_boot.img are selected when the build layout needs them. Actual board compatibility is still required."
                appendTerminal(
                    "[NCV] Image: $imageName\n" +
                    "[NCV] Type: Android system/GSI partition\n" +
                    "[READY] Native boot-container parser and virtio block backend are available.\n" +
                    "[NEXT] Select a compatible boot.img; vendor_boot/init_boot are required only when the chosen image layout places needed ramdisk/DTB content there.\n" +
                    "[LIMIT] Android board compatibility, complete userspace boot, and graphical display are not validated.\n\n" +
                    "No guest was started yet. The imported image was not modified.\n"
                )
            }
            ImageKind.ANDROID_BOOT_IMAGE -> {
                screenStatus.text = "ANDROID BOOT COMPONENT SELECTED"
                screenStatus.setTextColor(NcvUi.CYAN)
                screenDetail.text = "Add system.img or super.img. boot.img-only is accepted when the embedded generic ramdisk is present; vendor_boot/init_boot may be needed by the image layout."
                appendTerminal("[NCV] Boot component: $imageName\n[READY] Native boot-container parser is available.\n[NEXT] Select the GSI system partition and matching vendor_boot when required.\n")
            }
            ImageKind.ANDROID_PARTITION_IMAGE -> {
                screenStatus.text = "PARTITION SELECTED · PROFILE NEEDED"
                screenStatus.setTextColor(NcvUi.AMBER)
                screenDetail.text = if (imageName.lowercase().contains("super")) "This may be a dynamic super container. Use it instead of a standalone system.img." else "Use this partition as system.img or vendor.img, then select compatible boot components."
                appendTerminal("[NCV] Partition image: $imageName\n[READY] Read-only virtio block backend is available.\n[NEXT] Select compatible boot components and required partitions.\n")
            }
            ImageKind.ARCHIVE -> {
                screenStatus.text = "BUNDLE NOT PARSED"
                screenStatus.setTextColor(NcvUi.AMBER)
                screenDetail.text = "Select boot and partition images individually; ZIP bundle extraction is not implemented."
                appendTerminal("[NCV] Archive: $imageName\n[BLOCKED] Automatic archive extraction is not implemented.\nNo guest was started.\n")
            }
            else -> {
                screenStatus.text = "UNKNOWN IMAGE FORMAT"
                screenStatus.setTextColor(NcvUi.RED)
                screenDetail.text = "Select boot.img and a system.img/super.img. Extra boot images are optional for parsing but may be required by that build’s ramdisk/DTB layout."
                appendTerminal("[NCV] $imageName\n[BLOCKED] This file is not a recognized kernel or Android partition component.\nNo guest was started.\n")
            }
        }
        pauseButton.isEnabled = false
        pauseButton.alpha = 0.45f
        stopButton.text = "Back to library"
        stopButton.setOnClickListener { finish() }
        stopButton.setTextColor(NcvUi.TEXT)
    }

    private fun buildAndroidBootProfileCard(): LinearLayout {
        val card = NcvUi.card(this, 16)
        card.addView(NcvUi.text(this, "ANDROID BOOT PROFILE", 10f, NcvUi.MUTED, true).apply { letterSpacing = 0.1f })
        card.addView(NcvUi.spacer(this, 7))
        card.addView(NcvUi.text(this, "Use matching files from one Android build. A boot.img-only attempt is allowed when boot.img contains a non-empty ramdisk. vendor_boot.img often supplies the vendor ramdisk and DTB; init_boot.img is needed only when the generic ramdisk was split out. Missing components can still block a real Android boot.", 12f, NcvUi.MUTED))
        card.addView(NcvUi.spacer(this, 12))
        card.addView(NcvUi.text(this, "GUEST RAM CAPACITY · SPARSE BACKING", 10f, NcvUi.MUTED, true))
        card.addView(NcvUi.spacer(this, 6))
        guestRamSpinner = Spinner(this).apply {
            adapter = ArrayAdapter(this@GuestConsoleActivity, android.R.layout.simple_spinner_item, listOf(
                "512 MiB · conservative bring-up",
                "1024 MiB · 1 GiB",
                "2048 MiB · 2 GiB (default)"
            )).also { it.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item) }
            setSelection(2)
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onNothingSelected(parent: AdapterView<*>?) = Unit
                override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                    selectedGuestRamMiB = when (position) { 0 -> 512; 1 -> 1024; else -> 2048 }
                }
            }
        }
        card.addView(guestRamSpinner, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(NcvUi.spacer(this, 5))
        card.addView(NcvUi.text(this, "2 GiB is guest-visible capacity, not an immediate 2 GiB host allocation. NCV backs RAM in 4 KiB pages as the guest uses it; host memory use can still grow during kernel initialization.", 11f, NcvUi.AMBER))
        card.addView(NcvUi.spacer(this, 12))
        card.addView(NcvUi.text(this, "VIRTUAL BOARD / COMPATIBILITY PROFILE", 10f, NcvUi.MUTED, true))
        card.addView(NcvUi.spacer(this, 6))
        compatProfileSpinner = Spinner(this).apply {
            adapter = ArrayAdapter(this@GuestConsoleActivity, android.R.layout.simple_spinner_item, listOf(
                "NCV Generic ARM64 · strict unmapped-access faults",
                "MediaTek probe mode · experimental zero/no-op MMIO fallback"
            )).also { it.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item) }
            setSelection(selectedCompatProfileId)
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onNothingSelected(parent: AdapterView<*>?) = Unit
                override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                    selectedCompatProfileId = position.coerceIn(0, 1)
                }
            }
        }
        card.addView(compatProfileSpinner, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(NcvUi.spacer(this, 8))
        card.addView(NcvUi.text(this, "MTK probe mode only records otherwise-unmapped guest physical accesses: reads return zero and writes are ignored and counted. NCV has generic virtual-board devices (GICv2/ARM timer, PL031 RTC, virtio block/RNG with guest-RAM DMA, PL011 UART and a simple framebuffer), but these are not the MT6768 register map. MTK clock/reset controllers, PMIC, watchdog, IOMMU, Mali GPU, camera/audio/modem/Wi-Fi and proprietary vendor HAL behavior are still not emulated; this profile cannot make an arbitrary Infinix stock ROM compatible by itself.", 11f, NcvUi.AMBER))
        card.addView(NcvUi.spacer(this, 12))
        card.addView(NcvUi.text(this, "KERNEL DEVICE-TREE STRATEGY", 10f, NcvUi.MUTED, true))
        card.addView(NcvUi.spacer(this, 6))
        dtbModeSpinner = Spinner(this).apply {
            adapter = ArrayAdapter(this@GuestConsoleActivity, android.R.layout.simple_spinner_item, listOf(
                "NCV virtual board DTB · clean VirtIO bring-up (recommended next test)",
                "Hybrid phone DTB · keep Infinix vendor nodes (experimental)"
            )).also { it.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item) }
            setSelection(selectedDtbModeId)
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onNothingSelected(parent: AdapterView<*>?) = Unit
                override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                    selectedDtbModeId = position.coerceIn(0, 1)
                    refreshBootProfileSummary()
                }
            }
        }
        card.addView(dtbModeSpinner, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(NcvUi.spacer(this, 5))
        card.addView(NcvUi.text(this, "super.img is storage, not a bootable kernel. boot.img supplies the ARM64 kernel and ramdisk; this selector only changes the device tree presented to that real kernel. The clean NCV tree removes incompatible phone-device descriptions but cannot add missing VirtIO drivers or emulate MT6768 hardware.", 11f, NcvUi.AMBER))
        card.addView(NcvUi.spacer(this, 12))
        bootProfileSummary = NcvUi.text(this, "No boot profile selected.", 11f, NcvUi.TEXT, mono = true)
        card.addView(bootProfileSummary)
        card.addView(NcvUi.spacer(this, 12))
        superImageButton = NcvUi.button(this, "Select super.img (dynamic partitions)", primary = false) { openComponentPicker(PICK_SUPER_IMAGE) }
        card.addView(superImageButton, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(NcvUi.spacer(this, 7))
        systemImageButton = NcvUi.button(this, "Select system.img (standalone)", primary = false) { openComponentPicker(PICK_SYSTEM_IMAGE) }
        card.addView(systemImageButton, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(NcvUi.spacer(this, 7))
        bootImageButton = NcvUi.button(this, "Select boot.img", primary = false) { openComponentPicker(PICK_BOOT_IMAGE) }
        card.addView(bootImageButton, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(NcvUi.spacer(this, 7))
        vendorBootImageButton = NcvUi.button(this, "Select vendor_boot.img", primary = false) { openComponentPicker(PICK_VENDOR_BOOT_IMAGE) }
        card.addView(vendorBootImageButton, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(NcvUi.spacer(this, 7))
        initBootImageButton = NcvUi.button(this, "Select init_boot.img (Android 13+)", primary = false) { openComponentPicker(PICK_INIT_BOOT_IMAGE) }
        card.addView(initBootImageButton, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(NcvUi.spacer(this, 7))
        vendorPartitionButton = NcvUi.button(this, "Select vendor.img (optional)", primary = false) { openComponentPicker(PICK_VENDOR_PARTITION) }
        card.addView(vendorPartitionButton, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(NcvUi.spacer(this, 7))
        userdataButton = NcvUi.button(this, "Create / use private userdata.img (1 GiB)", primary = false) { preparePrivateUserdata() }
        card.addView(userdataButton, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(NcvUi.spacer(this, 10))
        card.addView(NcvUi.text(this,
            "Recommended experiment: run NCV's VirtIO-capable Linux kernel with the real ramdisk extracted from your matching Android boot bundle and the original super.img. This may reach Android first-stage init, but does not emulate the Infinix SoC, Mali/HWC or guarantee a graphical boot. The stock Infinix kernel path remains available below as a separate hardware-compatibility experiment.",
            11f, NcvUi.AMBER))
        card.addView(NcvUi.spacer(this, 8))
        startNcvKernelAndroidButton = NcvUi.button(this, "Try Android userspace · NCV kernel + original ramdisk →", primary = true) {
            startAndroidBootAttempt(useNcvKernel = true)
        }
        card.addView(startNcvKernelAndroidButton, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(NcvUi.spacer(this, 7))
        startAndroidButton = NcvUi.button(this, "Stock Infinix kernel experiment →", primary = false) {
            startAndroidBootAttempt(useNcvKernel = false)
        }
        card.addView(startAndroidButton, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        card.addView(NcvUi.spacer(this, 12))
        diagnosticBuildStatus = NcvUi.text(this, "Checking bundled NCV diagnostic boot assets…", 11f, NcvUi.MUTED)
        card.addView(diagnosticBuildStatus)
        card.addView(NcvUi.spacer(this, 8))
        startNcvSuperBringupButton = NcvUi.button(this, "Storage test only · generic Linux (not Android) →", primary = false) {
            beginNcvGenericSuperBringup()
        }
        card.addView(startNcvSuperBringupButton, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        return card
    }

    private fun isBundledAssetReadable(path: String): Boolean = runCatching {
        assets.open(path).use { input -> input.read() >= 0 }
    }.getOrDefault(false)

    private fun nativeEngineMatchesExpected(): Boolean = runCatching {
        NativeBridge.nativeGetEngineVersion() == EXPECTED_NATIVE_ENGINE
    }.getOrDefault(false)

    private fun nativeEngineMarker(): String = runCatching {
        NativeBridge.nativeGetEngineVersion()
    }.getOrDefault("native engine unavailable")

    private fun refreshBootProfileSummary() {
        if (!::bootProfileSummary.isInitialized) return
        bootProfileSummary.text = buildString {
            append("ENGINE     ").append(nativeEngineMarker()).append('\n')
            append("ENGINE GATE ").append(if (nativeEngineMatchesExpected()) "current build" else "STALE · reinstall exact rebuilt APK").append('\n')
            append("BASE DISK  ").append(selectedSuperName?.let { "super: $it" } ?: selectedSystemName ?: "Not selected").append('\n')
            append("BOOT       ").append(selectedBootName ?: "Not selected").append('\n')
            append("VENDOR BOOT ").append(selectedVendorBootName ?: "Not selected · optional for boot.img-only attempt").append('\n')
            append("INIT BOOT   ").append(selectedInitBootName ?: "Only if generic ramdisk is separate").append('\n')
            append("VENDOR     ").append(if (selectedSuperUri != null) "inside super / not separately attached" else selectedVendorName ?: "Optional · not selected").append('\n')
            append("USERDATA   ").append(if (hasPrivateUserdata()) "private persistent disk · ${ImageInspector.formatBytes(privateUserdataFile.length())}" else "not created").append('\n')
            append("PROFILE    ").append(if (selectedCompatProfileId == 1) "MTK probe fallback (experimental)" else "NCV generic ARM64 (strict)").append('\n')
            append("DTB MODE   ").append(if (selectedDtbModeId == 1) "hybrid Infinix vendor DTB" else "clean NCV virtual-board DTB").append('\n')
            append("GUEST RAM  ").append(selectedGuestRamMiB).append(" MiB · sparse-backed")
            append("\nDIAG BOOT  ").append(if (diagnosticGuestAssetsAvailable) "kernel + BusyBox initramfs bundled" else "MISSING ASSETS · rebuild required")
            parsedSuperMetadata?.let { append("\nLP TABLE   ").append("${it.partitions.size} logical partitions parsed; guest dm-linear not confirmed") }
            superMetadataError?.let { if (selectedSuperUri != null) append("\nLP TABLE   ").append("scan unavailable: $it") }
        }
        superImageButton.text = if (selectedSuperName == null) "Select super.img (dynamic partitions)" else "Super: ${selectedSuperName} · Change"
        systemImageButton.text = if (selectedSystemName == null) "Select system.img (standalone)" else "System: ${selectedSystemName} · Change"
        bootImageButton.text = if (selectedBootName == null) "Select boot.img" else "Boot: ${selectedBootName} · Change"
        vendorBootImageButton.text = if (selectedVendorBootName == null) "Select matching vendor_boot.img" else "Vendor boot: ${selectedVendorBootName} · Change"
        initBootImageButton.text = if (selectedInitBootName == null) "Select init_boot.img (only when needed)" else "Init boot: ${selectedInitBootName} · Change"
        vendorPartitionButton.text = if (selectedVendorName == null) "Select vendor.img (optional)" else "Vendor: ${selectedVendorName} · Change"
        userdataButton.text = if (hasPrivateUserdata()) "Private userdata ready · ${ImageInspector.formatBytes(privateUserdataFile.length())}" else "Create / use private userdata.img (1 GiB)"
        val sessionBusy = bootAttemptInProgress || vm != null
        if (::diagnosticBuildStatus.isInitialized) {
            diagnosticBuildStatus.text = when {
                !nativeEngineMatchesExpected() -> "STALE NATIVE ENGINE · This APK is not running the expected 0.1.23 C++ engine. Rebuild cleanly and install the exact verified APK."
                diagnosticGuestAssetsAvailable -> "NCV KERNEL READY · VirtIO-capable Linux Image is packaged; Android path extracts the real boot ramdisk."
                else -> "NCV KERNEL MISSING · Rebuild with ncv.linuxKernelSource and ncv.busyboxSource configured."
            }
            diagnosticBuildStatus.setTextColor(if (nativeEngineMatchesExpected() && diagnosticGuestAssetsAvailable) NcvUi.GREEN else NcvUi.RED)
        }
        if (::startNcvSuperBringupButton.isInitialized) {
            val canRunDiagnostic = selectedSuperUri != null && !sessionBusy && diagnosticGuestAssetsAvailable && nativeEngineMatchesExpected()
            startNcvSuperBringupButton.isEnabled = canRunDiagnostic
            startNcvSuperBringupButton.alpha = if (canRunDiagnostic) 1f else 0.55f
            startNcvSuperBringupButton.text = when {
                !diagnosticGuestAssetsAvailable -> "Diagnostic kernel missing · rebuild APK"
                selectedSuperUri == null -> "Select super.img to enable storage boot test"
                sessionBusy -> "Guest session active"
                else -> "Storage test only · generic Linux (not Android) →"
            }
        }
        val baseSelected = selectedSuperUri != null || selectedSystemUri != null
        // Best-effort Android userspace path: generic NCV kernel + real ramdisk + super container.
        val canAttemptNcvAndroid = selectedSuperUri != null && selectedBootUri != null &&
            !sessionBusy && diagnosticGuestAssetsAvailable && nativeEngineMatchesExpected()
        startNcvKernelAndroidButton.isEnabled = canAttemptNcvAndroid
        startNcvKernelAndroidButton.alpha = if (canAttemptNcvAndroid) 1f else 0.55f
        startNcvKernelAndroidButton.text = when {
            !nativeEngineMatchesExpected() -> "Stale native engine · clean rebuild required"
            !diagnosticGuestAssetsAvailable -> "NCV kernel missing · rebuild APK"
            selectedSuperUri == null -> "Select super.img to try Android userspace"
            selectedBootUri == null -> "Select matching boot.img to try Android userspace"
            sessionBusy -> "Guest session active"
            else -> "Try Android userspace · NCV kernel + original ramdisk →"
        }
        // Keep the original OEM-kernel path as an explicitly secondary experiment.
        val canAttempt = baseSelected && selectedBootUri != null && !sessionBusy && nativeEngineMatchesExpected()
        startAndroidButton.isEnabled = canAttempt
        startAndroidButton.alpha = if (canAttempt) 1f else 0.55f
        startAndroidButton.text = when {
            !nativeEngineMatchesExpected() -> "Stale native engine · clean rebuild required"
            selectedBootUri == null -> "Select boot.img for stock-kernel experiment"
            selectedSuperUri == null && selectedSystemUri == null -> "Select super.img or system.img first"
            sessionBusy -> "Guest session active"
            else -> "Stock Infinix kernel experiment (device model incomplete) →"
        }
        val canEditProfile = !sessionBusy
        startNcvSuperBringupButton.visibility = if (selectedSuperUri != null) View.VISIBLE else View.GONE
        val canRunDiagnostic = canEditProfile && selectedSuperUri != null &&
            diagnosticGuestAssetsAvailable && nativeEngineMatchesExpected()
        startNcvSuperBringupButton.isEnabled = canRunDiagnostic
        startNcvSuperBringupButton.alpha = if (canRunDiagnostic) 1f else 0.55f
        superImageButton.isEnabled = canEditProfile
        systemImageButton.isEnabled = canEditProfile
        bootImageButton.isEnabled = canEditProfile
        vendorBootImageButton.isEnabled = canEditProfile
        compatProfileSpinner.isEnabled = canEditProfile
        guestRamSpinner.isEnabled = canEditProfile
        dtbModeSpinner.isEnabled = canEditProfile
        initBootImageButton.isEnabled = canEditProfile
        vendorPartitionButton.isEnabled = canEditProfile && selectedSuperUri == null
        userdataButton.isEnabled = canEditProfile
        refreshLinuxBootSummary()
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
        if (requestCode !in PICK_SYSTEM_IMAGE..PICK_INITRD_IMAGE || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        try {
            val flags = (data.flags and Intent.FLAG_GRANT_READ_URI_PERMISSION)
            if (flags != 0) contentResolver.takePersistableUriPermission(uri, flags)
        } catch (_: Exception) {
            // Some document providers grant only a temporary URI permission.
        }
        val metadata = runCatching { ImageInspector.inspect(this, uri) }.getOrElse {
            Toast.makeText(this, "Could not inspect the selected image: ${it.message}", Toast.LENGTH_LONG).show()
            return
        }
        when (requestCode) {
            PICK_SYSTEM_IMAGE -> {
                selectedSystemUri = uri; selectedSystemName = metadata.displayName
                selectedSuperUri = null; selectedSuperName = null
            }
            PICK_SUPER_IMAGE -> {
                selectedSuperUri = uri; selectedSuperName = metadata.displayName
                selectedSystemUri = null; selectedSystemName = null
                selectedVendorPartitionUri = null; selectedVendorName = null
                parsedSuperMetadata = null; superMetadataError = null
            }
            PICK_BOOT_IMAGE -> { selectedBootUri = uri; selectedBootName = metadata.displayName }
            PICK_VENDOR_BOOT_IMAGE -> { selectedVendorBootUri = uri; selectedVendorBootName = metadata.displayName }
            PICK_VENDOR_PARTITION -> { selectedVendorPartitionUri = uri; selectedVendorName = metadata.displayName }
            PICK_INIT_BOOT_IMAGE -> { selectedInitBootUri = uri; selectedInitBootName = metadata.displayName }
            PICK_INITRD_IMAGE -> { selectedInitrdUri = uri; selectedInitrdName = metadata.displayName }
        }
        refreshBootProfileSummary()
        refreshLinuxBootSummary()
        appendTerminal("[NCV] Profile component selected: ${metadata.displayName} (${metadata.formatDescription})\n")
        if (requestCode == PICK_SUPER_IMAGE) analyzeSuperImageAsync(uri, metadata.displayName)
    }

    private val privateUserdataFile: File by lazy { File(File(filesDir, "ncv"), "userdata.img") }

    private fun hasPrivateUserdata(): Boolean = privateUserdataFile.isFile && privateUserdataFile.length() >= 512L

    private fun preparePrivateUserdata() {
        if (bootAttemptInProgress || vm != null) return
        try {
            val parent = privateUserdataFile.parentFile
            if (parent != null && !parent.exists() && !parent.mkdirs()) {
                throw IOException("Could not create NCV's private storage directory.")
            }
            RandomAccessFile(privateUserdataFile, "rw").use { disk ->
                // ftruncate normally creates a sparse file; preserve existing data and never shrink it.
                if (disk.length() < PRIVATE_USERDATA_SIZE_BYTES) disk.setLength(PRIVATE_USERDATA_SIZE_BYTES)
            }
            if (privateUserdataFile.length() < PRIVATE_USERDATA_SIZE_BYTES) {
                throw IOException("The private userdata image could not be sized to 1 GiB.")
            }
            appendTerminal("[NCV] Persistent userdata image ready: ${ImageInspector.formatBytes(privateUserdataFile.length())}\n")
            Toast.makeText(this, "Private userdata ready · 1 GiB", Toast.LENGTH_LONG).show()
            refreshBootProfileSummary()
        } catch (error: Throwable) {
            Toast.makeText(this, "Could not prepare userdata: ${error.message}", Toast.LENGTH_LONG).show()
            appendTerminal("[NCV ERROR] userdata: ${error.message ?: error.javaClass.simpleName}\n")
        }
    }

    /** Read the Android dynamic-partition table without starting the guest or modifying the image. */
    private fun analyzeSuperImageAsync(uri: android.net.Uri, name: String) {
        val key = uri.toString()
        if (!queuedSuperAnalyses.add(key)) return
        appendTerminal("[NCV LP] Inspecting dynamic-partition metadata for $name…\n")
        worker.execute {
            try {
                val summary = SuperImageAnalyzer.inspect(this, uri)
                val listed = summary.partitions.joinToString("\n") { partition ->
                    val flags = buildList {
                        if (partition.readOnly) add("ro")
                        if (partition.usesExternalBlockDevice) add("external-device")
                        if (partition.hasZeroExtents) add("zero-extents")
                    }.joinToString(", ").ifBlank { "linear" }
                    "  ${partition.name.padEnd(24)} ${ImageInspector.formatBytes(partition.sizeBytes).padStart(10)}  extents=${partition.extentCount}  [$flags]"
                }
                val report = buildString {
                    append("\n[NCV LP] Dynamic-partition metadata parsed successfully.\n")
                    append("[NCV LP] liblp version ${summary.metadataMajorVersion}.${summary.metadataMinorVersion}; metadata slot ${summary.metadataSlot}; encoded sparse=${summary.sparseEncoded}.\n")
                    append("[NCV LP] Logical image size: ${ImageInspector.formatBytes(summary.logicalImageSizeBytes)}.\n")
                    append("[NCV LP] Logical partitions (${summary.partitions.size}):\n")
                    append(listed).append('\n')
                    append("[NCV LP LIMIT] This is metadata inspection only. NCV still attaches the entire super image as one disk; it does not yet expose system/system_ext/product/vendor as separate /dev/block/mapper devices.\n")
                }
                mainHandler.post {
                    if (disposed) return@post
                    parsedSuperMetadata = summary
                    superMetadataError = null
                    refreshBootProfileSummary()
                    if (selectedSuperUri?.toString() == key && !bootAttemptInProgress && vm == null && ::screenDetail.isInitialized) {
                        val names = summary.partitions.take(10).joinToString(", ") { "${it.name} (${ImageInspector.formatBytes(it.sizeBytes)})" }
                        screenDetail.text = "Dynamic-partition metadata parsed: ${summary.partitions.size} logical partitions. $names${if (summary.partitions.size > 10) ", …" else ""}. This profile still does not mount those partitions into a booted Android userspace."
                    }
                    appendTerminal(report)
                }
            } catch (error: Throwable) {
                val message = error.message ?: error.javaClass.simpleName
                mainHandler.post {
                    if (disposed) return@post
                    superMetadataError = message
                    refreshBootProfileSummary()
                    if (selectedSuperUri?.toString() == key && !bootAttemptInProgress && vm == null && ::screenDetail.isInitialized) {
                        screenDetail.text = "Could not read the dynamic-partition table: $message. No image contents were modified."
                    }
                    appendTerminal("\n[NCV LP WARNING] Could not parse dynamic-partition metadata for $name: $message\n[NCV LP] The selected image remains read-only and has not been modified.\n")
                }
            }
        }
    }

    private fun appendTerminalFromWorker(value: String) {
        mainHandler.post { if (!disposed && ::terminal.isInitialized) appendTerminal(value) }
    }

    /**
     * Boots the packaged NCV-compatible generic Linux kernel (not the phone OEM kernel),
     * attaches the selected super.img as a read-only GPT disk, and attaches host-side logical
     * extent views for a real guest block-I/O bring-up. This intentionally does NOT report Android
     * as booted; the diagnostic initramfs checks /dev/vda and the logical views over UART.
     */
    private fun beginNcvGenericSuperBringup() {
        if (bootAttemptInProgress || vm != null) return
        val superUri = selectedSuperUri
        if (superUri == null) {
            Toast.makeText(this, "Select super.img first.", Toast.LENGTH_LONG).show()
            return
        }
        if (!nativeEngineMatchesExpected()) {
            screenStatus.text = "STALE NCV NATIVE ENGINE"
            screenStatus.setTextColor(NcvUi.RED)
            screenDetail.text = "The installed native library is older than this UI. Clean-rebuild and install the exact verified APK before running another guest."
            appendTerminal("\n[NCV BUILD GATE] Stale native engine detected; no guest was started. Expected $EXPECTED_NATIVE_ENGINE.\n")
            refreshBootProfileSummary()
            return
        }
        if (!diagnosticGuestAssetsAvailable) {
            screenStatus.text = "NCV DIAGNOSTIC BOOT ASSETS MISSING"
            screenStatus.setTextColor(NcvUi.RED)
            screenDetail.text = "This installed APK is missing ncv/guest/diagnostic/Image and/or initramfs.cpio.gzip. The APK uses a .gzip suffix to prevent AAPT from stripping the gzip extension. Source paths are build-time inputs only; they do not add files to an already-built APK. Rebuild, verify the APK contents, then install that exact APK. The OEM kernel path is a different experimental test."
            appendTerminal("\n[NCV SUPER BRING-UP BLOCKED] Missing packaged diagnostic asset(s).\n")
            appendTerminal("[NCV SUPER BRING-UP HINT] Check local.properties, then run tools/build_verify_ncv_super_bringup.sh --install from this exact project. The script verifies the generated assets and installed APK build marker before install.\n")
            Toast.makeText(this, "Diagnostic kernel assets missing from this APK. Rebuild first.", Toast.LENGTH_LONG).show()
            refreshBootProfileSummary()
            return
        }
        bootAttemptInProgress = true
        lastRuntimeReportAtMs = 0L
        guestStartedAtMs = 0L
        guestSawUartOutput = false
        guestHasVirtioMmioActivity = false
        guestHasBlockReads = false
        guestHasFramebufferWrites = false
        storageGateWarningLogged = false
        initGateWarningLogged = false
        graphicsGateWarningLogged = false
        runtimeBootArgsLogged = false
        refreshBootProfileSummary()
        screenStatus.text = "PREPARING NCV KERNEL + SUPER"
        screenStatus.setTextColor(NcvUi.CYAN)
        screenDetail.text = "Storage bring-up only: this uses the generic NCV Linux kernel, not the selected OEM boot.img. It cannot display the Infinix logo."
        appendTerminal("\n[NCV SUPER BRING-UP] Engine: ${runCatching { NativeBridge.nativeGetEngineVersion() }.getOrDefault("unknown")}\n")
        appendTerminal("[NCV SUPER BRING-UP] Starting the packaged NCV generic Linux kernel with the selected super.img attached read-only.\n")
        appendTerminal("[NCV SUPER BRING-UP] This is the next real prerequisite: prove UART, VirtIO block discovery, and actual reads before attempting Android init.\n")
        appendTerminal("[NCV SUPER BRING-UP] Expected assets: $NCV_DIAGNOSTIC_KERNEL_ASSET and $NCV_DIAGNOSTIC_INITRD_ASSET.\n")
        pauseButton.isEnabled = false
        pauseButton.alpha = 0.45f
        stopButton.isEnabled = true
        stopButton.text = "Cancel / back"
        stopButton.setOnClickListener { stopSession() }
        worker.execute {
            var createdVm: NcvVm? = null
            try {
                val kernelBytes = readBundledAssetBounded(NCV_DIAGNOSTIC_KERNEL_ASSET, MAX_KERNEL_IMAGE_BYTES)
                val initrdBytes = readBundledAssetBounded(NCV_DIAGNOSTIC_INITRD_ASSET, MAX_INITRD_BYTES)
                val initrdAddress = EXPERIMENTAL_INITRD_ADDRESS
                val activeVm = NcvVm.create(ramMiB = selectedGuestRamMiB, initialEL = 1, compatibilityProfileId = 0)
                createdVm = activeVm
                val entry = activeVm.loadLinuxImage(kernelBytes)
                activeVm.loadPayload(initrdBytes, initrdAddress)
                appendTerminalFromWorker("[NCV SUPER BRING-UP] Generic Linux Image loaded; entry=0x${entry.toString(16)}.\n")
                appendTerminalFromWorker("[NCV SUPER BRING-UP] Diagnostic initramfs loaded at 0x${initrdAddress.toString(16)} (${initrdBytes.size} bytes).\n")

                contentResolver.openFileDescriptor(superUri, "r")?.use { descriptor ->
                   // activeVm.attachSuperContainerFd(descriptor.fd, label = "super")
                } ?: throw IOException("NCV could not open the selected super.img as a read-only document.")
                appendTerminalFromWorker("[NCV SUPER BRING-UP] Attached super.img as a read-only synthetic GPT disk.\n")

                try {
                    SuperImageAnalyzer.open(this, superUri).use { openedSuper ->
                        val preferredOrder = listOf("system", "vendor", "product", "system_ext", "odm", "vendor_dlkm", "system_dlkm")
                        val candidates = openedSuper.layout.summary.partitions
                            .filter { partition ->
                                partition.sizeBytes > 0L && !partition.disabled && !partition.usesExternalBlockDevice &&
                                    partition.extentCount > 0 && partition.extents.isNotEmpty() &&
                                    partition.extents.all { it.sourceDeviceIndex == 0 }
                            }
                            .sortedWith(compareBy<LogicalPartitionInfo> {
                                val baseName = it.name.removeSuffix("_a").removeSuffix("_b")
                                val priority = preferredOrder.indexOf(baseName)
                                if (priority < 0) Int.MAX_VALUE else priority
                            }.thenBy { it.name })
                            .take(10)
                        for (partition in candidates) {
                            activeVm.attachLogicalPartitionFd(openedSuper.rawFileDescriptor, partition)
                            appendTerminalFromWorker("[NCV SUPER BRING-UP] Exposed read-only extent view ${partition.name} (${ImageInspector.formatBytes(partition.sizeBytes)}).\n")
                        }
                        appendTerminalFromWorker("[NCV SUPER BRING-UP] Metadata parsed: ${openedSuper.layout.summary.partitions.size} logical partition entries. Host-side extent views are not guest dm-linear devices.\n")
                    }
                } catch (metadataError: Throwable) {
                    appendTerminalFromWorker("[NCV SUPER BRING-UP WARNING] Could not attach logical extent views: ${metadataError.message ?: metadataError.javaClass.simpleName}. The raw super disk remains attached for the guest read test.\n")
                }

                if (hasPrivateUserdata()) {
                    ParcelFileDescriptor.open(privateUserdataFile, ParcelFileDescriptor.MODE_READ_WRITE).use { descriptor ->
                        activeVm.attachBlockFd(descriptor.fd, readOnly = false, label = "userdata")
                    }
                    appendTerminalFromWorker("[NCV SUPER BRING-UP] Attached private userdata as an optional read-write test disk.\n")
                }

                val bootArgs = "console=ttyAMA0 earlycon=pl011,mmio32,0x09000000 loglevel=8 ignore_loglevel panic=-1 rdinit=/init"
                val dtb = activeVm.prepareLinuxBootDtb(bootArgs, initrdAddress, initrdBytes.size.toLong())
                activeVm.start()
                if (disposed) {
                    runCatching { activeVm.stop() }; runCatching { activeVm.close() }
                    return@execute
                }
                runOnUiThread {
                    if (disposed) {
                        Thread({ runCatching { activeVm.stop() }; runCatching { activeVm.close() } }, "ncv-vm-cleanup").start()
                        return@runOnUiThread
                    }
                    vm = activeVm
                    createdVm = null
                    bootAttemptInProgress = false
                    guestStartedAtMs = SystemClock.elapsedRealtime()
                    screenStatus.text = "NCV KERNEL + SUPER · ANDROID NOT STARTED"
                    screenStatus.setTextColor(NcvUi.AMBER)
                    screenDetail.text = "The packaged NCV Linux kernel is running against read-only super.img and logical extent views. Look for [NCV-GUEST] BLOCK_READ_OK in UART; Android init is not part of this diagnostic image."
                    pauseButton.isEnabled = true
                    pauseButton.alpha = 1f
                    stopButton.isEnabled = true
                    stopButton.text = "Stop session"
                    stopButton.setOnClickListener { stopSession() }
                    refreshBootProfileSummary()
                    appendTerminal("[NCV SUPER BRING-UP] Engine: ${runCatching { NativeBridge.nativeGetEngineVersion() }.getOrDefault("unknown")}\n")
                    appendTerminal("[NCV SUPER BRING-UP] DTB: 0x${dtb.toString(16)}; guest RAM: ${selectedGuestRamMiB} MiB.\n")
                    appendTerminal("[NCV SUPER BRING-UP] Virtio block devices attached: ${activeVm.blockDeviceCount()}. Waiting for actual Linux UART and block reads.\n")
                    appendTerminal("[NCV SUPER BRING-UP] This test does not start the Android framework, ART, Binder services, SurfaceFlinger, or boot animation.\n")
                    appendTerminal("[NCV] ${activeVm.compatibilityReport()}\n")
                    pollEnabled = true
                    mainHandler.post(pollRunnable)
                }
            } catch (error: Throwable) {
                createdVm?.let { runCatching { it.stop() }; runCatching { it.close() } }
                runOnUiThread {
                    if (!disposed) {
                        bootAttemptInProgress = false
                        screenStatus.text = "NCV SUPER BRING-UP BLOCKED"
                        screenStatus.setTextColor(NcvUi.RED)
                        screenDetail.text = error.message ?: error.javaClass.simpleName
                        appendTerminal("\n[NCV SUPER BRING-UP ERROR] ${error.message ?: error.javaClass.simpleName}\n")
                        appendTerminal("[NCV SUPER BRING-UP HINT] The APK must include the NCV diagnostic Linux Image and initramfs. Configure ncv.linuxKernelSource and ncv.busyboxSource in local.properties, then rebuild the APK; this requires a Linux kernel source tree and AArch64 cross compiler.\n")
                        pauseButton.isEnabled = false
                        pauseButton.alpha = 0.45f
                        stopButton.text = "Back to ROM Workspace"
                        stopButton.isEnabled = true
                        stopButton.setOnClickListener { finish() }
                        refreshBootProfileSummary()
                    }
                }
            }
        }
    }

    private fun startAndroidBootAttempt(useNcvKernel: Boolean = false) {
        if (bootAttemptInProgress || vm != null) return
        val bootUri = selectedBootUri
        val baseDiskUri = selectedSuperUri ?: selectedSystemUri
        val baseDiskName = selectedSuperName ?: selectedSystemName ?: "base partition"
        if (bootUri == null || baseDiskUri == null) {
            Toast.makeText(this, "Select boot.img and either super.img or system.img first.", Toast.LENGTH_LONG).show()
            return
        }
        val vendorBootUri = selectedVendorBootUri
        val initBootUri = selectedInitBootUri
        val compatibilityProfileId = selectedCompatProfileId
        val dtbModeId = selectedDtbModeId
        val guestRamMiB = selectedGuestRamMiB
        val vendorUri = if (selectedSuperUri == null) selectedVendorPartitionUri else null
        bootAttemptInProgress = true
        lastRuntimeReportAtMs = 0L
        guestStartedAtMs = 0L
        guestSawUartOutput = false
        guestHasVirtioMmioActivity = false
        guestHasBlockReads = false
        guestHasFramebufferWrites = false
        storageGateWarningLogged = false
        initGateWarningLogged = false
        graphicsGateWarningLogged = false
        runtimeBootArgsLogged = false
        refreshBootProfileSummary()
        screenStatus.text = "PREPARING GUEST KERNEL"
        screenStatus.setTextColor(NcvUi.CYAN)
        screenDetail.text = "Validating boot headers, preparing initramfs, attaching the selected image as a read-only virtio disk, and checking optional persistent userdata."
        val nativeBuild = runCatching { NativeBridge.nativeGetEngineVersion() }
            .getOrElse { "unavailable: ${it.javaClass.simpleName}" }
        if (!nativeEngineMatchesExpected()) {
            bootAttemptInProgress = false
            refreshBootProfileSummary()
            appendTerminal("\n[NCV BUILD GATE] Refusing to start a guest with stale native engine '$nativeBuild'. Expected '$EXPECTED_NATIVE_ENGINE'. Clean-rebuild the APK and install that exact APK.\n")
            screenStatus.text = "STALE NCV NATIVE ENGINE"
            screenStatus.setTextColor(NcvUi.RED)
            screenDetail.text = "The installed libncv.so is older than the Kotlin UI. No guest was started."
            return
        }
        if (useNcvKernel && !diagnosticGuestAssetsAvailable) {
            bootAttemptInProgress = false
            refreshBootProfileSummary()
            appendTerminal("\n[NCV ANDROID INIT] Blocked: this APK has no verified NCV generic kernel asset.\n")
            return
        }
        appendTerminal("\n[NCV] Native engine build: $nativeBuild\n")
        appendTerminal(if (useNcvKernel)
            "[NCV ANDROID INIT] Starting NCV's VirtIO-capable kernel with the real ramdisk extracted from the selected Android boot bundle.\n"
        else "[NCV OEM KERNEL] Starting the kernel extracted from the selected Android boot image.\n")
        appendTerminal("[NCV] Guest RAM capacity: $guestRamMiB MiB; 4 KiB lazy page backing. Software-emulated vCPU.\n")
        appendTerminal(if (useNcvKernel)
            "[NCV BOOT MODEL] super.img is storage; the kernel comes from NCV's packaged Linux Image and the original Android ramdisk is extracted from the selected boot bundle.\n"
        else if (selectedSuperUri != null)
            "[NCV BOOT MODEL] super.img is the dynamic-partition storage container; the stock kernel and ramdisk are taken from boot.img (plus matching boot components if required).\n"
        else "[NCV BOOT MODEL] system.img is storage; the selected boot.img supplies the stock kernel and ramdisk.\n")
        appendTerminal(if (useNcvKernel)
            "[NCV ANDROID INIT] Kernel: NCV generic Linux Image; ramdisk: extracted from the selected Android boot bundle; DTB: clean NCV board. Phone DTB is intentionally not passed to this generic kernel.\n"
        else "[NCV BOOT MODEL] Device-tree strategy: ${if (dtbModeId == 1) "HYBRID_VENDOR_DTB" else "NCV_GENERIC_DTB"}.\n")
        appendTerminal(if (useNcvKernel)
            "[NCV] Compatibility profile: strict NCV virtual board. The user-selected MTK fallback profile is ignored for this path.\n"
        else if (compatibilityProfileId == 1)
            "[NCV] Compatibility profile: MTK probe mode (unknown MMIO reads return zero; writes ignored and traced). This is not an MT6768/MT6769 SoC model.\n"
        else "[NCV] Compatibility profile: strict NCV virtual board.\n")
        if (vendorBootUri == null) appendTerminal("[NCV INFO] vendor_boot.img not selected. For a legacy boot.img v0-v2 with an embedded ramdisk/DTB this may be expected; confirm the actual partition set rather than adding it blindly.\n")
        if (initBootUri == null) appendTerminal("[NCV INFO] init_boot.img not selected. Android 12 can keep the generic ramdisk in boot.img; init_boot is not automatically required.\n")
        if (!useNcvKernel) appendTerminal("[NCV LIMIT] The MTK probe profile is not an MT6768/MT6769 implementation: ignored MTK MMIO writes and zero-valued reads are only diagnostic fallbacks.\n")
        appendTerminal("[NCV LP] super.img will be wrapped in a read-only GPT carrier with one PARTNAME=super partition, so the guest may discover it as vda1 if its virtio-blk/GPT drivers are present.\n")
        appendTerminal("[NCV LP] Host-side read-only logical-partition views are also attached for diagnostics; Android must still create its own guest dm-linear devices from super metadata.\n")
        appendTerminal("[NCV LIMIT] GPT exposure does not guarantee a working first-stage mount: the ramdisk fstab, boot_devices, AVB/verity, and Android's dynamic-partition discovery still need to work on this virtual board.\n")
        appendTerminal("[NCV LIMIT] Simple framebuffer is not Mali/HWC/SurfaceFlinger; no Android boot animation is expected until device-compatible graphics and userspace boot are implemented.\n")
        worker.execute {
            var createdVm: NcvVm? = null
            try {
                val bootBytes = ImageInspector.readBounded(this, bootUri, MAX_BOOT_COMPONENT_BYTES)
                val vendorBootBytes = vendorBootUri?.let { ImageInspector.readBounded(this, it, MAX_BOOT_COMPONENT_BYTES) }
                val initBootBytes = initBootUri?.let { ImageInspector.readBounded(this, it, MAX_BOOT_COMPONENT_BYTES) }
                // Keep the two kernel strategies separate. The stock OEM kernel expects MTK hardware;
                // the NCV path uses a purpose-built kernel and only reuses the real Android ramdisk.
                val ncvKernelBytes = if (useNcvKernel) {
                    readBundledAssetBounded(NCV_DIAGNOSTIC_KERNEL_ASSET, MAX_KERNEL_IMAGE_BYTES)
                } else null
                val androidRamdiskBytes = if (useNcvKernel) {
                    NativeBridge.nativeExtractAndroidInitramfs(bootBytes, vendorBootBytes, initBootBytes)
                        ?: throw IOException("Could not extract the Android ramdisk from the selected boot bundle. Check that boot/vendor_boot/init_boot match the same firmware build.")
                } else null
                val androidBootArgs = if (useNcvKernel) {
                    NativeBridge.nativeExtractAndroidBootArgs(bootBytes, vendorBootBytes, initBootBytes)
                } else ""
                val activeVm = NcvVm.create(
                    ramMiB = guestRamMiB,
                    initialEL = 1,
                    compatibilityProfileId = if (useNcvKernel) 0 else compatibilityProfileId
                )
                createdVm = activeVm
                val info: AndroidBootLoadInfo? = if (useNcvKernel) null
                    else activeVm.loadAndroidBootBundle(bootBytes, vendorBootBytes, initBootBytes)
                val initrdAddress: Long
                val initrdSize: Long
                val entry: Long
                if (useNcvKernel) {
                    val kernel = ncvKernelBytes ?: throw IOException("Bundled NCV kernel asset is missing.")
                    val ramdisk = androidRamdiskBytes ?: throw IOException("Android ramdisk extraction failed.")
                    entry = activeVm.loadLinuxImage(kernel)
                    initrdAddress = EXPERIMENTAL_INITRD_ADDRESS
                    initrdSize = ramdisk.size.toLong()
                    activeVm.loadPayload(ramdisk, initrdAddress)
                    appendTerminalFromWorker("[NCV ANDROID INIT] NCV kernel Image loaded at 0x${entry.toString(16)} (${ImageInspector.formatBytes(kernel.size.toLong())}).\n")
                    appendTerminalFromWorker("[NCV ANDROID INIT] Real Android ramdisk extracted and loaded at 0x${initrdAddress.toString(16)} (${ImageInspector.formatBytes(initrdSize)}).\n")
                    if (androidBootArgs.isBlank()) appendTerminalFromWorker("[NCV ANDROID INIT WARNING] Could not read the original boot command line; using NCV console defaults.\n")
                } else {
                    val stockInfo = info ?: throw IOException("Stock Android boot bundle parser returned no metadata.")
                    entry = stockInfo.entry
                    initrdAddress = stockInfo.initrdAddress
                    initrdSize = stockInfo.initrdSize
                }
                appendTerminalFromWorker("[NCV RAM] Guest-visible capacity=${guestRamMiB} MiB; lazy 4 KiB page backing.\n")
                if (useNcvKernel) {
                    appendTerminalFromWorker("[NCV ANDROID INIT] Stock boot header parsed for ramdisk only; its OEM kernel and phone DTB are intentionally not loaded.\n")
                } else {
                    val stockInfo = info ?: throw IOException("Stock Android boot metadata unavailable.")
                    appendTerminalFromWorker(
                        "[NCV] boot.img header v${stockInfo.headerVersion}; " +
                        "boot.img DTB=${ImageInspector.formatBytes(stockInfo.bootDtbSize)}; " +
                        "vendor_boot DTB=${ImageInspector.formatBytes(stockInfo.vendorDtbSize)}; " +
                        "hybrid-DTB candidate=${ImageInspector.formatBytes(stockInfo.selectedDtbSize)} from " +
                        "${if (stockInfo.selectedDtbFromVendorBoot) "vendor_boot.img" else "boot.img or none"}.\n"
                    )
                    if (stockInfo.kernelHeaderAddress != 0L) {
                        appendTerminalFromWorker("[NCV] boot.img header kernel_addr=0x${stockInfo.kernelHeaderAddress.toString(16)}; ARM64 Image-derived load/entry=0x${entry.toString(16)}\n")
                        if (stockInfo.kernelHeaderAddress != entry) {
                            appendTerminalFromWorker("[NCV WARNING] Legacy header kernel_addr differs from the ARM64 Image-derived address. The loader uses the ARM64 Image text_offset rule.\n")
                        }
                    } else appendTerminalFromWorker("[NCV INFO] This header format has no legacy kernel_addr field.\n")
                    if (stockInfo.ramdiskHeaderAddress != 0L) appendTerminalFromWorker("[NCV] boot.img header ramdisk_addr=0x${stockInfo.ramdiskHeaderAddress.toString(16)}; loaded at 0x${initrdAddress.toString(16)}\n")
                    if (stockInfo.tagsHeaderAddress != 0L) appendTerminalFromWorker("[NCV] boot.img header tags_addr=0x${stockInfo.tagsHeaderAddress.toString(16)}\n")
                    if (stockInfo.dtbHeaderAddress != 0L) appendTerminalFromWorker("[NCV] boot.img header dtb_addr=0x${stockInfo.dtbHeaderAddress.toString(16)} (adapted by NCV)\n")
                }
                contentResolver.openFileDescriptor(baseDiskUri, "r")?.use { descriptor ->
                    if (selectedSuperUri != null) {
                        activeVm.attachSuperContainerFd(descriptor.fd, label = "super")
                        appendTerminalFromWorker(
                            "[NCV] super.img attached through a synthetic GPT carrier as virtio disk 0. " +
                                    "If the guest kernel has virtio-blk + GPT support, it should discover a partition labelled 'super' (typically /dev/vda1);\n"
                        )
                    } else {
                        activeVm.attachBlockFd(descriptor.fd, readOnly = true, label = baseDiskName)
                        appendTerminalFromWorker("[NCV] Base system image attached read-only as virtio disk 0 (/dev/block/vda expected in Linux): $baseDiskName\n")
                    }
                } ?: throw IllegalStateException("NCV could not open the selected base partition image.")
                appendTerminalFromWorker("[NCV STORAGE GATE] Virtio block ports attached=${activeVm.blockDeviceCount()}. The guest must probe virtio-mmio before any super-image read or Android dm-linear setup can happen. If mmio_reads and read_requests remain zero, inspect DTB discovery and the kernel's virtio driver configuration first.\n")
                // --- YENİ 5'Lİ VIRTIO EŞLEME SIRASI (fstab.ncv ile birebir uyumlu) ---
                val superUriForViews = selectedSuperUri
                if (superUriForViews != null) {
                    SuperImageAnalyzer.open(this, superUriForViews).use { openedSuper ->
                        val partitions = openedSuper.layout.summary.partitions

                        // 1. /dev/vda -> system
                        val systemPart = partitions.firstOrNull { it.name == "system_a" || it.name == "system" }
                        if (systemPart != null) {
                            activeVm.attachLogicalPartitionFd(openedSuper.rawFileDescriptor, systemPart)
                            appendTerminalFromWorker("[NCV DISK 0] /dev/vda -> ${systemPart.name} (${ImageInspector.formatBytes(systemPart.sizeBytes)})\n")
                        } else {
                            throw IOException("super.img içinde system veya system_a bulunamadı!")
                        }

                        // 2. /dev/vdb -> Generic AOSP vendor.img
                        // Telefonun /data/local/tmp/vendor.img dosyasını açıyoruz
                        val vendorFile = File("/data/local/tmp/vendor.img")
                        if (vendorFile.exists() && vendorFile.canRead()) {
                            ParcelFileDescriptor.open(vendorFile, ParcelFileDescriptor.MODE_READ_ONLY).use { vFd ->
                                activeVm.attachBlockFd(vFd.fd, readOnly = true, label = "vendor")
                            }
                            appendTerminalFromWorker("[NCV DISK 1] /dev/vdb -> AOSP Generic vendor.img (${ImageInspector.formatBytes(vendorFile.length())})\n")
                        } else {
                            // Eğer /data/local/tmp'de yoksa kullanıcının seçtiği vendor.img varsa onu bağla
                            val vUri = selectedVendorPartitionUri
                            if (vUri != null) {
                                contentResolver.openFileDescriptor(vUri, "r")?.use { vFd ->
                                    activeVm.attachBlockFd(vFd.fd, readOnly = true, label = "vendor")
                                }
                                appendTerminalFromWorker("[NCV DISK 1] /dev/vdb -> Selected vendor.img\n")
                            } else {
                                throw IOException("AOSP vendor.img bulunamadı! Lütfen /data/local/tmp/vendor.img konumuna atın veya arayüzden seçin.")
                            }
                        }

                        // 3. /dev/vdc -> product
                        val productPart = partitions.firstOrNull { it.name == "product_a" || it.name == "product" }
                        if (productPart != null) {
                            activeVm.attachLogicalPartitionFd(openedSuper.rawFileDescriptor, productPart)
                            appendTerminalFromWorker("[NCV DISK 2] /dev/vdc -> ${productPart.name} (${ImageInspector.formatBytes(productPart.sizeBytes)})\n")
                        } else {
                            appendTerminalFromWorker("[NCV DISK 2 WARNING] product bölümü bulunamadı, boş geçiliyor.\n")
                        }

                        // 4. /dev/vdd -> system_ext
                        val systemExtPart = partitions.firstOrNull { it.name == "system_ext_a" || it.name == "system_ext" }
                        if (systemExtPart != null) {
                            activeVm.attachLogicalPartitionFd(openedSuper.rawFileDescriptor, systemExtPart)
                            appendTerminalFromWorker("[NCV DISK 3] /dev/vdd -> ${systemExtPart.name} (${ImageInspector.formatBytes(systemExtPart.sizeBytes)})\n")
                        } else {
                            appendTerminalFromWorker("[NCV DISK 3 WARNING] system_ext bölümü bulunamadı, boş geçiliyor.\n")
                        }
                    }
                } else {
                    // super.img değil de doğrudan tek bir system.img seçildiyse:
                    contentResolver.openFileDescriptor(baseDiskUri, "r")?.use { descriptor ->
                        activeVm.attachBlockFd(descriptor.fd, readOnly = true, label = "system")
                    } ?: throw IllegalStateException("Taban system imajı açılamadı.")
                }

                // 5. /dev/vde -> userdata (1 GiB Ext4)
                if (hasPrivateUserdata()) {
                    ParcelFileDescriptor.open(privateUserdataFile, ParcelFileDescriptor.MODE_READ_WRITE).use { descriptor ->
                        activeVm.attachBlockFd(descriptor.fd, readOnly = false, label = "userdata")
                    }
                    appendTerminalFromWorker("[NCV DISK 4] /dev/vde -> userdata.img attached (read-write)\n")
                } else {
                    appendTerminalFromWorker("[NCV WARNING] Yazılabilir userdata diski yok!\n")
                }
                val stockInfo = info
                if (useNcvKernel) {
                    appendTerminalFromWorker("[NCV ANDROID INIT] Using a clean NCV board DTB; the phone's original device tree is not compatible with this kernel.\n")
                } else {
                    appendTerminalFromWorker(
                        "[NCV] DTB available for hybrid adaptation: " +
                        if ((stockInfo?.selectedDtbSize ?: 0L) > 0L) "${ImageInspector.formatBytes(stockInfo!!.selectedDtbSize)} from ${if (stockInfo.selectedDtbFromVendorBoot) "vendor_boot.img" else "boot.img"}.\n"
                        else "no boot.img/vendor_boot.img DTB found; generic NCV DTB is the only available choice.\n"
                    )
                }
                val baseBootArgs = if (useNcvKernel) androidBootArgs else (stockInfo?.commandLine ?: "")
                val filteredBootArgs = baseBootArgs.trim().split(Regex("\\s+")).filter { token ->
                    val key = token.substringBefore('=')
                    key !in setOf("console", "earlycon", "earlyprintk", "androidboot.console", "kernel.console", "kernel.earlycon")
                }.joinToString(" ")
                val dtbBootArgs = buildString {
                    append(filteredBootArgs)
                    if (!contains("console=")) append(" console=ttyAMA0,115200")
                    if (!contains("earlycon=")) append(" earlycon=pl011,mmio32,0x09000000")
                    if (!contains("lpj=")) append(" lpj=10000")
                    if (!contains("androidboot.hardware=")) append(" androidboot.hardware=ncv")
                    if (!contains("androidboot.fstab_suffix=")) append(" androidboot.fstab_suffix=ncv")
                    if (!contains("loglevel=")) append(" loglevel=8 ignore_loglevel")
                    if (useNcvKernel && !contains("init=")) append(" init=/init")
                    if (!contains("panic=")) append(" panic=-1")
                }.trim()
                appendTerminalFromWorker("[NCV] Effective kernel command line: $dtbBootArgs\n")
                val dtb = if (!useNcvKernel && dtbModeId == 1 && (stockInfo?.selectedDtbSize ?: 0L) > 0L) {
                    try {
                        activeVm.prepareAndroidBootDtb(dtbBootArgs, initrdAddress, initrdSize).also {
                            appendTerminalFromWorker("[NCV DTB] HYBRID_VENDOR_DTB selected for the stock OEM-kernel experiment. This DTB does not emulate missing MTK hardware.\n")
                        }
                    } catch (hybridError: Throwable) {
                        appendTerminalFromWorker("[NCV DTB WARNING] Hybrid DTB adaptation failed (${hybridError.message}); falling back to a clean NCV generic DTB.\n")
                        activeVm.prepareLinuxBootDtb(dtbBootArgs, initrdAddress, initrdSize)
                    }
                } else {
                    if (!useNcvKernel && dtbModeId == 1) appendTerminalFromWorker("[NCV DTB WARNING] No boot DTB was available; selected generic NCV DTB instead.\n")
                    activeVm.prepareLinuxBootDtb(dtbBootArgs, initrdAddress, initrdSize).also {
                        appendTerminalFromWorker("[NCV DTB] Clean NCV virtual-board DTB installed.\n")
                    }
                }
                appendTerminalFromWorker("[NCV ARM64 ABI] Before entry: EL1; SCTLR_EL1 RES1 bits set with MMU/data cache/instruction cache disabled; DAIF masked; X0=DTB physical address; X1-X3=0.\n")
                activeVm.start()
                val successfulVm = activeVm
                if (disposed) {
                    runCatching { successfulVm.stop() }; runCatching { successfulVm.close() }
                    return@execute
                }
                runOnUiThread {
                    if (disposed) {
                        Thread({ runCatching { successfulVm.stop() }; runCatching { successfulVm.close() } }, "ncv-vm-cleanup").start()
                        return@runOnUiThread
                    }
                    vm = successfulVm
                    guestStartedAtMs = SystemClock.elapsedRealtime()
                    bootAttemptInProgress = false
                    screenStatus.text = if (useNcvKernel) "ANDROID INIT ATTEMPT RUNNING" else "OEM KERNEL ENTRY · ANDROID UNVERIFIED"
                    screenStatus.setTextColor(NcvUi.AMBER)
                    screenDetail.text = if (useNcvKernel) {
                        "NCV's generic kernel is running with the original Android ramdisk and read-only super.img. Watch for first-stage init / dynamic-partition messages; the Infinix graphics/HAL stack is not emulated."
                    } else {
                        "The stock Infinix kernel has entered the NCV virtual board, but MediaTek hardware support is incomplete. This is not proof that Android init started."
                    }
                    pauseButton.isEnabled = true
                    pauseButton.alpha = 1f
                    stopButton.isEnabled = true
                    stopButton.text = "Stop session"
                    stopButton.setOnClickListener { stopSession() }
                    refreshBootProfileSummary()
                    if (useNcvKernel) {
                        appendTerminal("[NCV ANDROID INIT] Kernel entry: 0x${entry.toString(16)}\n[NCV ANDROID INIT] Original Android initramfs: 0x${initrdAddress.toString(16)} + ${initrdSize} bytes\n[NCV ANDROID INIT] Super carrier and logical-partition views attached; waiting for actual guest init/mapper output.\n")
                    } else {
                        val stockInfo = info ?: throw IllegalStateException("Stock boot metadata unavailable after start.")
                        appendTerminal("[NCV OEM KERNEL] Android boot image v${stockInfo.headerVersion} parsed.\n[NCV OEM KERNEL] Kernel entry: 0x${entry.toString(16)}\n[NCV OEM KERNEL] Initramfs: 0x${initrdAddress.toString(16)} + ${initrdSize} bytes\n")
                    }
                    appendTerminal("[NCV] Virtio block devices attached: ${successfulVm.blockDeviceCount()}\n[NCV] NCV board DTB prepared at 0x${dtb.toString(16)}.\n[NCV] CPU thread started. Guest CPU activity alone is not proof of Android boot.\n[NCV] ${successfulVm.compatibilityReport()}\n")
                    pollEnabled = true
                    mainHandler.post(pollRunnable)
                }
                createdVm = null
            } catch (error: Throwable) {
                createdVm?.let { runCatching { it.stop() }; runCatching { it.close() } }
                runOnUiThread {
                    if (!disposed) {
                        bootAttemptInProgress = false
                        screenStatus.text = "GUEST KERNEL ATTEMPT BLOCKED"
                        screenStatus.setTextColor(NcvUi.RED)
                        screenDetail.text = error.message ?: error.javaClass.simpleName
                        appendTerminal("\n[NCV ERROR] ${error.message ?: error.javaClass.simpleName}\n[NCV] Guest was not left running after setup failure.\n")
                        refreshBootProfileSummary()
                    }
                }
            }
        }
    }


    private fun readBundledAssetBounded(path: String, maxBytes: Long): ByteArray {
        val output = ByteArrayOutputStream()
        assets.open(path).use { input ->
            val buffer = ByteArray(64 * 1024)
            var total = 0L
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                if (count == 0) continue
                total += count
                if (total > maxBytes) throw IOException("Bundled asset exceeds the ${ImageInspector.formatBytes(maxBytes)} safety limit.")
                output.write(buffer, 0, count)
            }
        }
        if (output.size() == 0) throw IOException("Bundled asset is empty: $path")
        return output.toByteArray()
    }

    private fun beginExperimentalKernelBoot() {
        if (bootAttemptInProgress || vm != null) return
        bootAttemptInProgress = true
        refreshLinuxBootSummary()
        screenStatus.text = "VALIDATING ARM64 IMAGE"
        screenStatus.setTextColor(NcvUi.CYAN)
        screenDetail.text = "The native loader will validate the kernel header and optional initramfs before guest code is executed."
        appendTerminal("[NCV] Preparing experimental direct-kernel boot.\n[NCV] Guest RAM: 256 MiB.\n")
        pauseButton.isEnabled = false
        stopButton.isEnabled = true
        stopButton.text = "Cancel / back"
        worker.execute {
            var createdVm: NcvVm? = null
            try {
                val bytes = imageAssetPath?.let { readBundledAssetBounded(it, MAX_KERNEL_IMAGE_BYTES) }
                    ?: ImageInspector.readBounded(this, imageUri, MAX_KERNEL_IMAGE_BYTES)
                val initrdUri = selectedInitrdUri
                val initrdBytes = when {
                    initrdUri != null -> ImageInspector.readBounded(this, initrdUri, MAX_INITRD_BYTES)
                    embeddedInitrdAssetPath != null -> readBundledAssetBounded(embeddedInitrdAssetPath!!, MAX_INITRD_BYTES)
                    else -> null
                }
                val initrdAddress = if (initrdBytes != null) EXPERIMENTAL_INITRD_ADDRESS else 0L
                val activeVm = NcvVm.create(ramMiB = 256, initialEL = 1)
                createdVm = activeVm
                val entry = activeVm.loadLinuxImage(bytes)
                if (initrdBytes != null) {
                    activeVm.loadPayload(initrdBytes, initrdAddress)
                    appendTerminalFromWorker("[NCV] Initramfs loaded at 0x${initrdAddress.toString(16)} (${initrdBytes.size} bytes).\n")
                }
                val bootArgs = buildString {
                    append("console=ttyAMA0 earlycon=pl011,mmio32,0x09000000 panic=-1")
                    if (initrdBytes != null) append(" rdinit=/init")
                }
                val dtb = activeVm.prepareLinuxBootDtb(bootArgs, initrdAddress, initrdBytes?.size?.toLong() ?: 0L)
                activeVm.start()
                if (disposed) {
                    runCatching { activeVm.stop() }
                    runCatching { activeVm.close() }
                    return@execute
                }
                runOnUiThread {
                    if (disposed) {
                        Thread({ runCatching { activeVm.stop() }; runCatching { activeVm.close() } }, "ncv-vm-cleanup").start()
                        return@runOnUiThread
                    }
                    vm = activeVm
                    createdVm = null
                    bootAttemptInProgress = false
                    screenStatus.text = "KERNEL BOOT ATTEMPT RUNNING"
                    screenStatus.setTextColor(NcvUi.AMBER)
                    screenDetail.text = "Kernel entry 0x${entry.toString(16)} · DTB 0x${dtb.toString(16)}. Check UART output; Linux boot is not yet validated."
                    pauseButton.isEnabled = true
                    pauseButton.alpha = 1f
                    stopButton.isEnabled = true
                    stopButton.text = "Stop session"
                    stopButton.setOnClickListener { stopSession() }
                    refreshLinuxBootSummary()
                    appendTerminal("[NCV] Kernel loaded at 0x${entry.toString(16)}\n[NCV] DTB prepared at 0x${dtb.toString(16)}\n[NCV] Initramfs: ${initrdBytes?.let { "0x${initrdAddress.toString(16)} / ${it.size} bytes" } ?: "none"}\n[NCV] CPU thread started. Waiting for actual serial output; this is not proof of successful Linux boot.\n")
                    pollEnabled = true
                    mainHandler.post(pollRunnable)
                }
            } catch (error: Throwable) {
                createdVm?.let { runCatching { it.stop() }; runCatching { it.close() } }
                runOnUiThread {
                    if (!disposed) {
                        bootAttemptInProgress = false
                        screenStatus.text = "BOOT ATTEMPT FAILED"
                        screenStatus.setTextColor(NcvUi.RED)
                        screenDetail.text = error.message ?: error.javaClass.simpleName
                        appendTerminal("\n[NCV ERROR] ${error.message ?: error.javaClass.simpleName}\nNo claim of successful boot was made.\n")
                        pauseButton.isEnabled = false
                        pauseButton.alpha = 0.45f
                        stopButton.text = "Back to library"
                        stopButton.setOnClickListener { finish() }
                        refreshLinuxBootSummary()
                    }
                }
            }
        }
    }

    private fun pauseOrResume() {
        val current = vm ?: return
        pauseButton.isEnabled = false
        worker.execute {
            try {
                val currentState = current.state()
                if (currentState == VmState.RUNNING) current.pause() else if (currentState == VmState.PAUSED) current.resume()
                runOnUiThread {
                    pauseButton.text = if (current.state() == VmState.PAUSED) "Resume" else "Pause"
                    pauseButton.isEnabled = current.state() == VmState.RUNNING || current.state() == VmState.PAUSED
                }
            } catch (error: Throwable) {
                runOnUiThread { Toast.makeText(this, "VM control failed: ${error.message}", Toast.LENGTH_LONG).show() }
            }
        }
    }

    private fun stopSession() {
        val current = vm
        if (current == null) {
            finish()
            return
        }
        vm = null
        stopButton.isEnabled = false
        pauseButton.isEnabled = false
        pollEnabled = false
        mainHandler.removeCallbacks(pollRunnable)
        worker.execute {
            runCatching { current.stop() }
            val compatReport = runCatching { current.compatibilityReport() }.getOrDefault("Compatibility report unavailable.")
            runCatching { current.close() }
            runOnUiThread {
                if (disposed) return@runOnUiThread
                screenStatus.text = "SESSION STOPPED"
                screenStatus.setTextColor(NcvUi.MUTED)
                screenDetail.text = "The experimental guest session has been stopped."
                appendTerminal("\n[NCV] Session stopped by user.\n[NCV] $compatReport\n")
                stopButton.text = "Back to library"
                stopButton.isEnabled = true
                pauseButton.isEnabled = false
                bootAttemptInProgress = false
                stopButton.setOnClickListener { finish() }
                refreshBootProfileSummary()
            }
        }
    }

    private fun appendTerminal(value: String) {
        synchronized(fullSessionLog) {
            fullSessionLog.append(value)
            if (fullSessionLog.length > MAX_COPY_LOG_CHARS) {
                val removeCount = fullSessionLog.length - MAX_COPY_LOG_CHARS
                fullSessionLog.delete(0, removeCount)
                sessionLogTruncated = true
            }
        }
        val combined = terminal.text.toString() + value
        terminal.text = if (combined.length > MAX_CONSOLE_CHARS) combined.takeLast(MAX_CONSOLE_CHARS) else combined
    }

    private fun copyAllSessionLog() {
        val snapshot = synchronized(fullSessionLog) {
            val warning = if (sessionLogTruncated) {
                "[NCV WARNING] The oldest part of the session log was trimmed because the in-app copy buffer reached $MAX_COPY_LOG_CHARS characters.\n\n"
            } else ""
            warning + fullSessionLog.toString()
        }
        if (snapshot.isBlank()) {
            Toast.makeText(this, "Kopyalanacak log bulunamadı.", Toast.LENGTH_SHORT).show()
            return
        }
        val clipboard = getSystemService(CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.setPrimaryClip(ClipData.newPlainText("NCV session log", snapshot))
        Toast.makeText(this, "Oturum logu panoya kopyalandı (${snapshot.length} karakter).", Toast.LENGTH_LONG).show()
    }

    override fun onDestroy() {
        disposed = true
        pollEnabled = false
        mainHandler.removeCallbacks(pollRunnable)
        val current = vm
        vm = null
        if (current != null) {
            worker.execute {
                runCatching { current.stop() }
                runCatching { current.close() }
            }
        }
        worker.shutdown()
        super.onDestroy()
    }
}
