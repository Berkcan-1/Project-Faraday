import java.io.File
import java.security.MessageDigest
import java.util.Properties
import org.gradle.api.GradleException

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.aerolon.ncv"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.aerolon.ncv"
        minSdk = 31
        targetSdk = 36
        versionCode = 12
        versionName = "0.1.24-dev"

        ndk { abiFilters += listOf("arm64-v8a") }
        externalNativeBuild {
            cmake { cppFlags += "-std=c++17" }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }

    packaging {
        jniLibs { useLegacyPackaging = false }
    }
}


// NCV automatic guest-artifact pipeline.
// Android's own boot.img/init_boot.img/vendor_boot.img and system/super images are
// deliberately NOT built here; the developer supplies those from the matching AOSP build.

val ncvLocalProperties = Properties().apply {
    val propsFile = rootProject.file("local.properties")
    if (propsFile.isFile) propsFile.inputStream().use { load(it) }
}

fun ncvConfiguredValue(gradleKey: String, envKey: String, localKey: String, defaultValue: String = ""): String =
    providers.gradleProperty(gradleKey).orNull?.takeIf { it.isNotBlank() }
        ?: System.getenv(envKey)?.takeIf { it.isNotBlank() }
        ?: ncvLocalProperties.getProperty(localKey)?.trim()?.takeIf { it.isNotBlank() }
        ?: defaultValue

val ncvLinuxKernelSource = ncvConfiguredValue("ncvLinuxKernelSource", "NCV_LINUX_KERNEL_SOURCE", "ncv.linuxKernelSource")
val ncvBusyBoxSource = ncvConfiguredValue("ncvBusyBoxSource", "NCV_BUSYBOX_SOURCE", "ncv.busyboxSource")
val ncvCrossCompile = ncvConfiguredValue("ncvCrossCompile", "NCV_CROSS_COMPILE", "ncv.crossCompile", "aarch64-linux-gnu-")
val ncvBuildJobs = ncvConfiguredValue("ncvBuildJobs", "NCV_BUILD_JOBS", "ncv.buildJobs", Runtime.getRuntime().availableProcessors().coerceAtMost(8).toString())
val ncvForceGuestRebuild = ncvConfiguredValue("ncvForceGuestRebuild", "NCV_FORCE_GUEST_REBUILD", "ncv.forceGuestRebuild", "false").toBoolean()
val ncvPackageGuestArtifacts = ncvConfiguredValue("ncvPackageGuestArtifacts", "NCV_PACKAGE_GUEST_ARTIFACTS", "ncv.packageGuestArtifacts", "true").toBoolean()
val ncvRequireDiagnosticGuestArtifacts = ncvConfiguredValue("ncvRequireDiagnosticGuestArtifacts", "NCV_REQUIRE_DIAGNOSTIC_GUEST_ARTIFACTS", "ncv.requireDiagnosticGuestArtifacts", "true").toBoolean()

val ncvGuestBuildDir = layout.buildDirectory.dir("ncv-guest")
val ncvKernelOutputDir = ncvGuestBuildDir.map { it.dir("kernel-out") }
val ncvKernelImage = ncvKernelOutputDir.map { it.file("arch/arm64/boot/Image") }
val ncvKernelBuildStamp = ncvKernelOutputDir.map { it.file(".ncv-build-stamp") }
val ncvBusyBoxOutputDir = ncvGuestBuildDir.map { it.dir("busybox-out") }
val ncvBusyBoxBinary = ncvBusyBoxOutputDir.map { it.file("busybox") }
val ncvBusyBoxBuildStamp = ncvBusyBoxOutputDir.map { it.file(".ncv-build-stamp") }
val ncvInitramfsImage = ncvGuestBuildDir.map { it.file("ncv-test-initramfs.cpio.gz") }
val ncvGeneratedAssetsDir = layout.buildDirectory.dir("generated/ncvAssets/main")

// Generated assets become ordinary APK assets. The directory is build output, never source-controlled.
android.sourceSets.getByName("main").assets.srcDir(ncvGeneratedAssetsDir)

val buildNcvDiagnosticKernel = tasks.register("buildNcvDiagnosticKernel") {
    group = "NCV"
    description = "Optionally cross-builds a diagnostic ARM64 Linux kernel for NCV; it is not an Android boot.img."
    inputs.property("kernelSourcePath", ncvLinuxKernelSource)
    inputs.property("crossCompile", ncvCrossCompile)
    inputs.property("jobs", ncvBuildJobs)
    inputs.property("forceRebuild", ncvForceGuestRebuild)
    inputs.file(rootProject.file("guest-board/ncv_linux_bringup.config"))
    inputs.file(rootProject.file("tools/build_ncv_kernel.sh"))
    inputs.file(rootProject.file("tools/ncv_llvm_toolchain.sh"))
    inputs.file(rootProject.file("tools/prepare_ncv_kernel_config_fragment.sh"))
    inputs.file(rootProject.file("guest-board/README.md"))
    outputs.files(ncvKernelImage, ncvKernelBuildStamp)
    outputs.upToDateWhen { !ncvForceGuestRebuild && ncvKernelImage.get().asFile.isFile && ncvKernelImage.get().asFile.length() > 0L && ncvKernelBuildStamp.get().asFile.isFile }
    onlyIf("Enable ncv.packageGuestArtifacts and set ncv.linuxKernelSource to build/package the diagnostic guest kernel") {
        ncvPackageGuestArtifacts && ncvLinuxKernelSource.isNotBlank()
    }
    doLast {
        val source = file(ncvLinuxKernelSource)
        if (!source.isDirectory || !source.resolve("Makefile").isFile) {
            throw GradleException("ncv.linuxKernelSource is not a Linux kernel source tree: $source")
        }
        val image = ncvKernelImage.get().asFile
        val stamp = ncvKernelBuildStamp.get().asFile
        val configFragment = rootProject.file("guest-board/ncv_linux_bringup.config")
        val compatHelper = rootProject.file("tools/prepare_ncv_kernel_config_fragment.sh")
        val kernelBuilder = rootProject.file("tools/build_ncv_kernel.sh")
        val llvmToolchainResolver = rootProject.file("tools/ncv_llvm_toolchain.sh")
        val kernelKconfigFiles = listOf(
            "drivers/md/Kconfig",
            "drivers/md/Makefile",
            "drivers/android/Kconfig",
            "drivers/staging/android/Kconfig",
            "security/selinux/Kconfig"
        )
        val kernelKconfigSignature = kernelKconfigFiles.joinToString(";") { relative ->
            val candidate = source.resolve(relative)
            "$relative=${if (candidate.isFile) ncvSha256(candidate) else "missing"}"
        }
        val buildSignature = listOf(
            "source=${source.canonicalPath}",
            "cross=${ncvCrossCompile}",
            "jobs=${ncvBuildJobs}",
            "fragmentSha256=${ncvSha256(configFragment)}",
            "compatHelperSha256=${ncvSha256(compatHelper)}",
            "builderSha256=${ncvSha256(kernelBuilder)}",
            "llvmToolchainResolverSha256=${ncvSha256(llvmToolchainResolver)}",
            "kernelKconfigSignature=$kernelKconfigSignature"
        ).joinToString("\n")
        if (!ncvForceGuestRebuild && image.isFile && image.length() > 0L && stamp.isFile && stamp.readText() == buildSignature) {
            logger.lifecycle("NCV diagnostic kernel is already built at ${image.absolutePath}; Gradle will reuse it. Set ncv.forceGuestRebuild=true to force a rebuild.")
        } else {
            exec {
                workingDir(rootProject.projectDir)
                commandLine("bash", rootProject.file("tools/build_ncv_kernel.sh").absolutePath, source.absolutePath, ncvKernelOutputDir.get().asFile.absolutePath)
                environment("CROSS_COMPILE", ncvCrossCompile)
                environment("JOBS", ncvBuildJobs)
                environment("LC_ALL", "C")
            }
            stamp.parentFile.mkdirs()
            stamp.writeText(buildSignature)
        }
        if (!image.isFile || image.length() == 0L) throw GradleException("NCV diagnostic kernel Image was not created: $image")
    }
}

val buildNcvStaticBusyBox = tasks.register("buildNcvStaticBusyBox") {
    group = "NCV"
    description = "Optionally cross-builds static AArch64 BusyBox for the diagnostic initramfs."
    inputs.property("busyBoxSourcePath", ncvBusyBoxSource)
    inputs.property("crossCompile", ncvCrossCompile)
    inputs.property("jobs", ncvBuildJobs)
    inputs.property("forceRebuild", ncvForceGuestRebuild)
    inputs.file(rootProject.file("tools/build_ncv_busybox.sh"))
    outputs.files(ncvBusyBoxBinary, ncvBusyBoxBuildStamp)
    outputs.upToDateWhen { !ncvForceGuestRebuild && ncvBusyBoxBinary.get().asFile.isFile && ncvBusyBoxBinary.get().asFile.length() > 0L && ncvBusyBoxBuildStamp.get().asFile.isFile }
    onlyIf("Enable ncv.packageGuestArtifacts and set ncv.busyboxSource to build/package diagnostic BusyBox") {
        ncvPackageGuestArtifacts && ncvBusyBoxSource.isNotBlank()
    }
    doLast {
        val source = file(ncvBusyBoxSource)
        if (!source.isDirectory || !source.resolve("Makefile").isFile) {
            throw GradleException("ncv.busyboxSource is not a BusyBox source tree: $source")
        }
        val binary = ncvBusyBoxBinary.get().asFile
        val stamp = ncvBusyBoxBuildStamp.get().asFile
        val configFragment = rootProject.file("guest-board/ncv_linux_bringup.config")
        val compatHelper = rootProject.file("tools/prepare_ncv_kernel_config_fragment.sh")
        val kernelBuilder = rootProject.file("tools/build_ncv_kernel.sh")
        val llvmToolchainResolver = rootProject.file("tools/ncv_llvm_toolchain.sh")
        val kernelKconfigFiles = listOf(
            "drivers/md/Kconfig",
            "drivers/md/Makefile",
            "drivers/android/Kconfig",
            "drivers/staging/android/Kconfig",
            "security/selinux/Kconfig"
        )
        val kernelKconfigSignature = kernelKconfigFiles.joinToString(";") { relative ->
            val candidate = source.resolve(relative)
            "$relative=${if (candidate.isFile) ncvSha256(candidate) else "missing"}"
        }
        val buildSignature = listOf(
            "source=${source.canonicalPath}",
            "cross=${ncvCrossCompile}",
            "jobs=${ncvBuildJobs}",
            "fragmentSha256=${ncvSha256(configFragment)}",
            "compatHelperSha256=${ncvSha256(compatHelper)}",
            "builderSha256=${ncvSha256(kernelBuilder)}",
            "llvmToolchainResolverSha256=${ncvSha256(llvmToolchainResolver)}",
            "kernelKconfigSignature=$kernelKconfigSignature"
        ).joinToString("\n")
        if (!ncvForceGuestRebuild && binary.isFile && binary.length() > 0L && stamp.isFile && stamp.readText() == buildSignature) {
            logger.lifecycle("NCV static BusyBox is already built at ${binary.absolutePath}; Gradle will reuse it. Set ncv.forceGuestRebuild=true to rebuild.")
        } else {
            exec {
                workingDir(rootProject.projectDir)
                commandLine("bash", rootProject.file("tools/build_ncv_busybox.sh").absolutePath, source.absolutePath, ncvBusyBoxOutputDir.get().asFile.absolutePath)
                environment("CROSS_COMPILE", ncvCrossCompile)
                environment("JOBS", ncvBuildJobs)
                environment("LC_ALL", "C")
            }
            stamp.parentFile.mkdirs()
            stamp.writeText(buildSignature)
        }
        if (!binary.isFile || binary.length() == 0L) throw GradleException("NCV static BusyBox binary was not created: $binary")
    }
}

val buildNcvTestInitramfs = tasks.register("buildNcvTestInitramfs") {
    group = "NCV"
    description = "Packages the optional BusyBox /init diagnostic initramfs."
    dependsOn(buildNcvStaticBusyBox)
    inputs.file(ncvBusyBoxBinary).optional()
    inputs.file(rootProject.file("tools/make_ncv_test_initramfs.sh"))
    outputs.file(ncvInitramfsImage)
    onlyIf("The diagnostic initramfs is enabled when ncv.packageGuestArtifacts and ncv.busyboxSource are configured") {
        ncvPackageGuestArtifacts && ncvBusyBoxSource.isNotBlank()
    }
    doLast {
        val binary = ncvBusyBoxBinary.get().asFile
        if (!binary.isFile) throw GradleException("BusyBox artifact is missing: $binary")
        exec {
            workingDir(rootProject.projectDir)
            commandLine("bash", rootProject.file("tools/make_ncv_test_initramfs.sh").absolutePath, binary.absolutePath, ncvInitramfsImage.get().asFile.absolutePath)
            environment("LC_ALL", "C")
        }
        if (!ncvInitramfsImage.get().asFile.isFile) throw GradleException("NCV test initramfs was not created")
    }
}

fun ncvSha256(file: File): String {
    val digest = MessageDigest.getInstance("SHA-256")
    file.inputStream().buffered().use { input ->
        val buffer = ByteArray(64 * 1024)
        while (true) {
            val count = input.read(buffer)
            if (count < 0) break
            digest.update(buffer, 0, count)
        }
    }
    return digest.digest().joinToString("") { "%02x".format(it) }
}

val prepareNcvApkAssets = tasks.register("prepareNcvApkAssets") {
    group = "NCV"
    description = "Prepares manifest/board assets and includes optional diagnostic guest artifacts in the APK."
    dependsOn(buildNcvDiagnosticKernel, buildNcvTestInitramfs)
    inputs.property("kernelConfigured", ncvLinuxKernelSource.isNotBlank())
    inputs.property("busyBoxConfigured", ncvBusyBoxSource.isNotBlank())
    inputs.property("packageGuestArtifacts", ncvPackageGuestArtifacts)
    inputs.property("requireDiagnosticGuestArtifacts", ncvRequireDiagnosticGuestArtifacts)
    inputs.file(rootProject.file("guest-board/ncv_linux_bringup.config"))
    inputs.file(rootProject.file("tools/build_ncv_kernel.sh"))
    inputs.file(rootProject.file("tools/ncv_llvm_toolchain.sh"))
    inputs.file(rootProject.file("tools/prepare_ncv_kernel_config_fragment.sh"))
    inputs.file(rootProject.file("guest-board/README.md"))
    inputs.file(ncvKernelImage).optional()
    inputs.file(ncvInitramfsImage).optional()
    outputs.dir(ncvGeneratedAssetsDir)
    doLast {
        val out = ncvGeneratedAssetsDir.get().asFile
        delete(out)
        val boardDir = out.resolve("ncv/guest-board")
        val diagDir = out.resolve("ncv/guest/diagnostic")
        boardDir.mkdirs()
        rootProject.file("guest-board/ncv_linux_bringup.config").copyTo(boardDir.resolve("ncv_linux_bringup.config"), overwrite = true)
        rootProject.file("guest-board/README.md").copyTo(boardDir.resolve("README.md"), overwrite = true)

        val packaged = mutableListOf<String>()
        val kernel = ncvKernelImage.get().asFile
        val initramfs = ncvInitramfsImage.get().asFile
        var kernelIncluded = false
        var initramfsIncluded = false
        if (ncvPackageGuestArtifacts && ncvLinuxKernelSource.isNotBlank() && kernel.isFile && kernel.length() > 0L) {
            diagDir.mkdirs()
            val copied = diagDir.resolve("Image")
            kernel.copyTo(copied, overwrite = true)
            kernelIncluded = true
            packaged += "\"diagnosticLinuxImage\": {\"asset\": \"ncv/guest/diagnostic/Image\", \"sizeBytes\": ${copied.length()}, \"sha256\": \"${ncvSha256(copied)}\"}"
        } else {
            packaged += "\"diagnosticLinuxImage\": null"
        }
        if (ncvPackageGuestArtifacts && ncvBusyBoxSource.isNotBlank() && initramfs.isFile && initramfs.length() > 0L) {
            diagDir.mkdirs()
            // AAPT treats a final .gz suffix specially: it may gunzip the payload and strip the suffix.
            // Keep the payload gzip-compressed but use .gzip as the APK asset suffix so bytes and manifest hashes survive.
            val copied = diagDir.resolve("initramfs.cpio.gzip")
            initramfs.copyTo(copied, overwrite = true)
            initramfsIncluded = true
            packaged += "\"diagnosticInitramfs\": {\"asset\": \"ncv/guest/diagnostic/initramfs.cpio.gzip\", \"sizeBytes\": ${copied.length()}, \"sha256\": \"${ncvSha256(copied)}\"}"
        } else {
            packaged += "\"diagnosticInitramfs\": null"
        }
        val manifest = """{
  "schemaVersion": 1,
  "engine": "libncv.so",
  "nativeAbi": "arm64-v8a",
  "androidBootImagesBuiltByGradle": false,
  "androidBootImagesProvidedAtRuntime": true,
  "diagnosticGuestArtifactPackagingAllowed": ${ncvPackageGuestArtifacts},
  "diagnosticGuestArtifactsRequired": ${ncvRequireDiagnosticGuestArtifacts},
  "diagnosticGuestArtifactsReady": ${kernelIncluded && initramfsIncluded},
  ${packaged.joinToString(",\n  ")}
}
"""
        val manifestFile = out.resolve("ncv/guest/manifest.json")
        manifestFile.parentFile.mkdirs()
        manifestFile.writeText(manifest)
        logger.lifecycle("NCV APK guest assets prepared. Diagnostic kernel included: $kernelIncluded; diagnostic initramfs included: $initramfsIncluded.")
        if (ncvPackageGuestArtifacts && ncvRequireDiagnosticGuestArtifacts && !(kernelIncluded && initramfsIncluded)) {
            throw GradleException(
                "NCV diagnostic boot assets are required but were not packaged. " +
                    "Set ncv.linuxKernelSource and ncv.busyboxSource in local.properties to real source trees, " +
                    "ensure the AArch64 cross compiler/build tools are installed, and rebuild. " +
                    "To intentionally build a UI-only APK, set ncv.requireDiagnosticGuestArtifacts=false; " +
                    "the NCV storage boot button will stay disabled in that APK."
            )
        }
        if (!kernelIncluded && !initramfsIncluded) {
            logger.lifecycle("No optional Linux diagnostic artifacts configured. This build is UI/native-engine only; diagnostic storage boot is unavailable.")
        }
    }
}

val verifyNcvNativeSources = tasks.register("verifyNcvNativeSources") {
    group = "NCV"
    description = "Checks that every source referenced by the Android CMake build exists before assembling the APK."
    val cmakeFile = file("src/main/cpp/CMakeLists.txt")
    inputs.file(cmakeFile)
    doLast {
        val text = cmakeFile.readText()
        val sourceRoot = file("src/main/cpp")
        val missing = Regex("^\\s+([A-Za-z0-9_./-]+\\.cpp)\\)?\\s*$", RegexOption.MULTILINE)
            .findAll(text)
            .map { it.groupValues[1] }
            .filter { !sourceRoot.resolve(it).isFile }
            .toList()
        if (missing.isNotEmpty()) throw GradleException("NCV CMake refers to missing C++ source(s): ${missing.joinToString()}")
        if (!file("src/main/cpp/jni/ncv_jni.cpp").isFile) throw GradleException("Missing NCV JNI bridge source")
        logger.lifecycle("NCV native source preflight passed; CMake/NDK will compile libncv.so during the Android build.")
    }
}

// Explicitly order generated guest assets before AGP merges the debug asset tree.
// preBuild dependency alone is not relied on as the ordering edge for mergeDebugAssets.
tasks.matching { it.name == "mergeDebugAssets" }.configureEach {
    dependsOn(prepareNcvApkAssets)
}

tasks.named("preBuild").configure {
    dependsOn(verifyNcvNativeSources, prepareNcvApkAssets)
}
