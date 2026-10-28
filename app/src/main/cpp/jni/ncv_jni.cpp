// NCV - JNI sınırı. Kural: C++ exception JNI dışına ÇIKMAZ; hatalar kodla döner. Handle = kayıt defteri id'si (ham pointer değil).
// Dönüş kodları: >= 0 Status değeri (0 = Ok);  -100 geçersiz handle;  -101 beklenmeyen exception.
#include <jni.h>

#include <array>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <memory>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/ncv_version.h"
#include "../core/ncv_vm.h"
#include "../selftest/ncv_selftest.h"

namespace {

constexpr jint kBadHandle = -100;
constexpr jint kException = -101;
constexpr jint kMaxPayloadBytes = 256 * 1024 * 1024;  // compressed/raw kernel input cap

std::mutex g_mu;
std::unordered_map<int64_t, std::shared_ptr<ncv::Vm>> g_vms;
int64_t g_nextId = 1;

std::shared_ptr<ncv::Vm> lookup(jlong h) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_vms.find(h);
  return it == g_vms.end() ? nullptr : it->second;
}

template <typename F>
jint withVm(jlong h, F&& f) {
  try {
    std::shared_ptr<ncv::Vm> vm = lookup(h);
    if (!vm) return kBadHandle;
    return static_cast<jint>(f(*vm));
  } catch (...) {
    return kException;
  }
}

jbyteArray toByteArray(JNIEnv* env, const std::string& s) {
  jbyteArray arr = env->NewByteArray(static_cast<jsize>(s.size()));
  if (arr != nullptr && !s.empty())
    env->SetByteArrayRegion(arr, 0, static_cast<jsize>(s.size()), reinterpret_cast<const jbyte*>(s.data()));
  return arr;
}

bool readJavaImage(JNIEnv* env, jbyteArray array, bool required, std::vector<uint8_t>* out) {
  if (!out) return false;
  out->clear();
  if (array == nullptr) return !required;
  const jsize length = env->GetArrayLength(array);
  if (length <= 0 || length > kMaxPayloadBytes) return false;
  try { out->resize(static_cast<size_t>(length)); }
  catch (...) { return false; }
  env->GetByteArrayRegion(array, 0, length, reinterpret_cast<jbyte*>(out->data()));
  return !env->ExceptionCheck();
}

ncv::Status extractAndroidInitramfsFromJava(JNIEnv* env, jbyteArray bootArray,
                                             jbyteArray vendorArray, jbyteArray initBootArray,
                                             std::vector<uint8_t>* initrd,
                                             std::string* commandLine) {
  std::vector<uint8_t> bootBytes, vendorBytes, initBootBytes;
  if (!readJavaImage(env, bootArray, true, &bootBytes) ||
      !readJavaImage(env, vendorArray, false, &vendorBytes) ||
      !readJavaImage(env, initBootArray, false, &initBootBytes)) {
    return ncv::Status::InvalidArgument;
  }
  return ncv::extractAndroidInitramfs(
      bootBytes.data(), bootBytes.size(),
      vendorBytes.empty() ? nullptr : vendorBytes.data(), vendorBytes.size(),
      initBootBytes.empty() ? nullptr : initBootBytes.data(), initBootBytes.size(),
      initrd, commandLine);
}

// Java'ya yalnızca ASCII (geçerli modified-UTF-8) döndür.
std::string asciiOnly(const std::string& in) {
  std::string out;
  for (unsigned char c : in) out.push_back((c == '\n' || (c >= 32 && c < 127)) ? static_cast<char>(c) : '?');
  return out;
}

std::string stringFromJava(JNIEnv* env, jstring value) {
  if (!env || !value) return {};
  const char* chars = env->GetStringUTFChars(value, nullptr);
  if (!chars) return {};
  std::string result(chars);
  env->ReleaseStringUTFChars(value, chars);
  return result;
}

std::string instructionHint(uint32_t insn) {
  std::ostringstream out;
  out << "opcode=0x" << std::hex << std::setw(8) << std::setfill('0') << insn << " class=";
  if ((insn & 0xFFFFF0FFu) == 0xD50340DFu) return out.str() + "PSTATE.DAIFSet";
  if ((insn & 0xFFFFF0FFu) == 0xD50340FFu) return out.str() + "PSTATE.DAIFClr";
  if ((insn & 0xFFFFF01Fu) == 0xD503301Fu) return out.str() + "barrier (DSB/DMB/ISB)";
  if ((insn & 0xFFFFF01Fu) == 0xD503201Fu) return out.str() + "hint (WFI/WFE/YIELD/NOP/CSDB family)";
  if ((insn & 0xFFF00000u) == 0xD5300000u || (insn & 0xFFF00000u) == 0xD5100000u) {
    const bool read = (insn & (1u << 21)) != 0;
    const unsigned op0=2u+((insn>>19)&1u), op1=(insn>>16)&7u, crn=(insn>>12)&15u, crm=(insn>>8)&15u, op2=(insn>>5)&7u, rt=insn&31u;
    if (op0==2u && op1==0u && crn==0u && crm==2u && op2==2u)
      return out.str() + (read ? "MRS MDSCR_EL1 (debug register modeled RAZ/WI), Rt=X" : "MSR MDSCR_EL1 (debug write ignored), Rt=X") + std::to_string(rt);
    if (op0==3u && op1==0u && crn==0u && crm==0u && op2==6u)
      return out.str() + (read ? "MRS REVIDR_EL1 (supported; virtual revision ID=0), Rt=X" : "MSR REVIDR_EL1 (read-only register; should fault), Rt=X") + std::to_string(rt);
    out << "system-register " << (read ? "MRS " : "MSR ") << "S" << op0 << "_" << op1 << "_C" << crn << "_C" << crm << "_" << op2 << ", Rt=X" << rt;
    return out.str();
  }
  if ((insn & 0x3F000000u) == 0x08000000u) return out.str() + "atomic/exclusive load-store family (LDXR/STXR/LSE)";
  if ((insn & 0x1E000000u) == 0x0E000000u) return out.str() + "SIMD/FP/crypto family";
  if ((insn & 0x0A000000u) == 0x08000000u) return out.str() + "load-store family";
  if ((insn & 0x7C000000u) == 0x14000000u) return out.str() + "branch family";
  if ((insn & 0x1F000000u) == 0x11000000u) return out.str() + "integer data-processing family";
  return out.str() + "unclassified A64 instruction; raw opcode is required";
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL Java_com_aerolon_ncv_NativeBridge_nativeGetEngineVersion(JNIEnv* env, jobject) {
  return env->NewStringUTF(NCV_ENGINE_VERSION);
}

// Extract the actual boot/vendor/init_boot ramdisk payloads without loading the OEM kernel.
// This is the key piece for trying Android userspace on NCV's own VirtIO-capable kernel.
JNIEXPORT jbyteArray JNICALL Java_com_aerolon_ncv_NativeBridge_nativeExtractAndroidInitramfs(
    JNIEnv* env, jobject, jbyteArray bootArray, jbyteArray vendorArray, jbyteArray initBootArray) {
  try {
    std::vector<uint8_t> initrd;
    std::string commandLine;
    const ncv::Status status = extractAndroidInitramfsFromJava(
        env, bootArray, vendorArray, initBootArray, &initrd, &commandLine);
    constexpr size_t kMaxAndroidInitrdBytes = 64u * 1024u * 1024u;
    if (status != ncv::Status::Ok || initrd.empty() || initrd.size() > kMaxAndroidInitrdBytes ||
        initrd.size() > static_cast<size_t>(std::numeric_limits<jsize>::max())) return nullptr;
    jbyteArray result = env->NewByteArray(static_cast<jsize>(initrd.size()));
    if (result != nullptr) {
      env->SetByteArrayRegion(result, 0, static_cast<jsize>(initrd.size()),
                              reinterpret_cast<const jbyte*>(initrd.data()));
      if (env->ExceptionCheck()) return nullptr;
    }
    return result;
  } catch (...) {
    return nullptr;
  }
}

JNIEXPORT jstring JNICALL Java_com_aerolon_ncv_NativeBridge_nativeExtractAndroidBootArgs(
    JNIEnv* env, jobject, jbyteArray bootArray, jbyteArray vendorArray, jbyteArray initBootArray) {
  try {
    std::vector<uint8_t> initrd;
    std::string commandLine;
    const ncv::Status status = extractAndroidInitramfsFromJava(
        env, bootArray, vendorArray, initBootArray, &initrd, &commandLine);
    if (status != ncv::Status::Ok) return env->NewStringUTF("");
    return env->NewStringUTF(asciiOnly(commandLine).c_str());
  } catch (...) {
    return env->NewStringUTF("");
  }
}

JNIEXPORT jstring JNICALL Java_com_aerolon_ncv_NativeBridge_nativeSelfTest(JNIEnv* env, jobject) {
  try {
    bool ok = false;
    const std::string rep = ncv::runSelfTest(&ok);
    return env->NewStringUTF(asciiOnly(std::string(ok ? "SELFTEST PASS\n" : "SELFTEST FAIL\n") + rep).c_str());
  } catch (...) {
    return env->NewStringUTF("SELFTEST FAIL\nunexpected native exception\n");
  }
}

// Shared constructor. Dönüş: > 0 handle; <= 0 hata (negatif Status ya da kException).
static jlong createVmHandle(jint ramMiB, jint initialEL, jint compatibilityProfileId = 0) {
  try {
    if (ramMiB < 1 || ramMiB > 2048 || initialEL < 0 || initialEL > 1 || compatibilityProfileId < 0 || compatibilityProfileId > 1)
      return -static_cast<jlong>(ncv::Status::InvalidArgument);
    ncv::BoardConfig cfg;
    cfg.ramSize = static_cast<uint64_t>(ramMiB) << 20;
    cfg.initialEL = static_cast<uint8_t>(initialEL);
    cfg.compatibilityProfileId = static_cast<uint8_t>(compatibilityProfileId);
    ncv::Status st = ncv::Status::Ok;
    std::unique_ptr<ncv::Vm> vm = ncv::Vm::create(cfg, &st);
    if (!vm) return -static_cast<jlong>(st);
    std::lock_guard<std::mutex> lk(g_mu);
    const int64_t id = g_nextId++;
    g_vms.emplace(id, std::shared_ptr<ncv::Vm>(std::move(vm)));
    return id;
  } catch (...) {
    return kException;
  }
}

JNIEXPORT jlong JNICALL Java_com_aerolon_ncv_NativeBridge_nativeCreateVm(JNIEnv*, jobject, jint ramMiB) {
  return createVmHandle(ramMiB, 0);  // Backward-compatible EL0 default
}

JNIEXPORT jlong JNICALL Java_com_aerolon_ncv_NativeBridge_nativeCreateVmWithInitialEL(JNIEnv*, jobject,
                                                                                       jint ramMiB, jint initialEL) {
  return createVmHandle(ramMiB, initialEL);
}

JNIEXPORT jlong JNICALL Java_com_aerolon_ncv_NativeBridge_nativeCreateVmWithProfile(JNIEnv*, jobject,
    jint ramMiB, jint initialEL, jint compatibilityProfileId) {
  return createVmHandle(ramMiB, initialEL, compatibilityProfileId);
}

JNIEXPORT jint JNICALL Java_com_aerolon_ncv_NativeBridge_nativeDestroyVm(JNIEnv*, jobject, jlong h) {
  try {
    std::shared_ptr<ncv::Vm> vm;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      auto it = g_vms.find(h);
      if (it == g_vms.end()) return kBadHandle;
      vm = it->second;
      g_vms.erase(it);
    }
    vm->stop();  // Created'da InvalidState dönebilir; yok sayılır
    vm.reset();  // son referans düşünce destructor thread'i toplar
    return 0;
  } catch (...) {
    return kException;
  }
}

JNIEXPORT jint JNICALL Java_com_aerolon_ncv_NativeBridge_nativeLoadPayload(JNIEnv* env, jobject, jlong h,
                                                                           jbyteArray data, jlong addr) {
  try {
    if (data == nullptr) return static_cast<jint>(ncv::Status::InvalidArgument);
    const jsize len = env->GetArrayLength(data);
    if (len < 0 || len > kMaxPayloadBytes) return static_cast<jint>(ncv::Status::InvalidArgument);
    std::vector<uint8_t> buf(static_cast<size_t>(len));
    if (len > 0) env->GetByteArrayRegion(data, 0, len, reinterpret_cast<jbyte*>(buf.data()));
    if (env->ExceptionCheck()) return kException;
    return withVm(h, [&](ncv::Vm& vm) {
      return vm.loadPayload(ncv::GuestAddr(static_cast<uint64_t>(addr)), buf.data(), buf.size());
    });
  } catch (...) {
    return kException;
  }
}

JNIEXPORT jlong JNICALL Java_com_aerolon_ncv_NativeBridge_nativeLoadLinuxImage(JNIEnv* env, jobject,
                                                                                       jlong h, jbyteArray data) {
  try {
    if (data == nullptr) return -static_cast<jlong>(ncv::Status::InvalidArgument);
    const jsize len = env->GetArrayLength(data);
    if (len < 64 || len > kMaxPayloadBytes) return -static_cast<jlong>(ncv::Status::InvalidArgument);
    std::vector<uint8_t> buf(static_cast<size_t>(len));
    env->GetByteArrayRegion(data, 0, len, reinterpret_cast<jbyte*>(buf.data()));
    if (env->ExceptionCheck()) return kException;
    std::shared_ptr<ncv::Vm> vm = lookup(h);
    if (!vm) return kBadHandle;
    ncv::GuestAddr entry;
    const ncv::Status status = vm->loadLinuxImage(buf.data(), buf.size(), &entry);
    if (status != ncv::Status::Ok) return -static_cast<jlong>(status);
    return static_cast<jlong>(entry.value);
  } catch (...) {
    return kException;
  }
}

JNIEXPORT jlongArray JNICALL Java_com_aerolon_ncv_NativeBridge_nativeLoadAndroidBootBundle(
    JNIEnv* env, jobject, jlong h, jbyteArray bootArray, jbyteArray vendorArray, jbyteArray initBootArray) {
  // Result fields: status, entry, initrdAddress, initrdSize, headerVersion,
  // selectedDtbSize, bootDtbSize, vendorDtbSize, selectedDtbFromVendorBoot,
  // legacy kernel_addr, ramdisk_addr, tags_addr, dtb_addr.
  constexpr jsize kAndroidBootResultCount = 13;
  std::array<jlong, kAndroidBootResultCount> values = {
      static_cast<jlong>(ncv::Status::InvalidArgument), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  try {
    if (bootArray == nullptr) goto done;
    {
      const jsize bootLen = env->GetArrayLength(bootArray);
      if (bootLen <= 0 || bootLen > kMaxPayloadBytes) goto done;
      std::vector<uint8_t> bootBytes(static_cast<size_t>(bootLen));
      env->GetByteArrayRegion(bootArray, 0, bootLen, reinterpret_cast<jbyte*>(bootBytes.data()));
      if (env->ExceptionCheck()) { values[0] = kException; goto done; }
      std::vector<uint8_t> vendorBytes;
      if (vendorArray != nullptr) {
        const jsize vendorLen = env->GetArrayLength(vendorArray);
        if (vendorLen <= 0 || vendorLen > kMaxPayloadBytes) goto done;
        vendorBytes.resize(static_cast<size_t>(vendorLen));
        env->GetByteArrayRegion(vendorArray, 0, vendorLen, reinterpret_cast<jbyte*>(vendorBytes.data()));
        if (env->ExceptionCheck()) { values[0] = kException; goto done; }
      }
      std::vector<uint8_t> initBootBytes;
      if (initBootArray != nullptr) {
        const jsize initBootLen = env->GetArrayLength(initBootArray);
        if (initBootLen <= 0 || initBootLen > kMaxPayloadBytes) goto done;
        initBootBytes.resize(static_cast<size_t>(initBootLen));
        env->GetByteArrayRegion(initBootArray, 0, initBootLen, reinterpret_cast<jbyte*>(initBootBytes.data()));
        if (env->ExceptionCheck()) { values[0] = kException; goto done; }
      }
      std::shared_ptr<ncv::Vm> vm = lookup(h);
      if (!vm) { values[0] = kBadHandle; goto done; }
      ncv::AndroidBootLoadInfo info;
      const ncv::Status status = vm->loadAndroidBootBundle(
          bootBytes.data(), bootBytes.size(), vendorBytes.empty() ? nullptr : vendorBytes.data(),
          vendorBytes.size(), initBootBytes.empty() ? nullptr : initBootBytes.data(), initBootBytes.size(), &info);
      values[0] = static_cast<jlong>(status);
      if (status == ncv::Status::Ok) {
        values[1] = static_cast<jlong>(info.entry);
        values[2] = static_cast<jlong>(info.initrdAddress);
        values[3] = static_cast<jlong>(info.initrdSize);
        values[4] = static_cast<jlong>(info.headerVersion);
        values[5] = static_cast<jlong>(info.selectedDtbSize);
        values[6] = static_cast<jlong>(info.bootDtbSize);
        values[7] = static_cast<jlong>(info.vendorDtbSize);
        values[8] = static_cast<jlong>(info.selectedDtbFromVendorBoot ? 1 : 0);
        values[9] = static_cast<jlong>(info.kernelHeaderAddress);
        values[10] = static_cast<jlong>(info.ramdiskHeaderAddress);
        values[11] = static_cast<jlong>(info.tagsHeaderAddress);
        values[12] = static_cast<jlong>(info.dtbHeaderAddress);
      }
    }
  } catch (...) { values[0] = kException; }
done:
  jlongArray result = env->NewLongArray(kAndroidBootResultCount);
  if (result != nullptr) {
    env->SetLongArrayRegion(result, 0, kAndroidBootResultCount, values.data());
  }
  return result;
}

JNIEXPORT jstring JNICALL Java_com_aerolon_ncv_NativeBridge_nativeGetBootArgs(JNIEnv* env, jobject, jlong h) {
  try {
    std::shared_ptr<ncv::Vm> vm = lookup(h);
    if (!vm) return env->NewStringUTF("");
    const std::string args = vm->loadedBootArgs();
    return env->NewStringUTF(asciiOnly(args).c_str());
  } catch (...) { return env->NewStringUTF(""); }
}

JNIEXPORT jstring JNICALL Java_com_aerolon_ncv_NativeBridge_nativeGetRuntimeDiagnostics(JNIEnv* env, jobject, jlong h) {
  try {
    std::shared_ptr<ncv::Vm> vm = lookup(h);
    if (!vm) return env->NewStringUTF("bad-handle");
    const std::string snapshot = asciiOnly(vm->runtimeDiagnostics());
    return env->NewStringUTF(snapshot.c_str());
  } catch (...) { return env->NewStringUTF("runtime-diagnostics-unavailable"); }
}

JNIEXPORT jint JNICALL Java_com_aerolon_ncv_NativeBridge_nativeAttachBlockFd(JNIEnv* env, jobject, jlong h,
                                                                                jint fd, jboolean readOnly, jstring label) {
  const std::string labelValue = label ? stringFromJava(env, label) : std::string("disk");
  return withVm(h, [&](ncv::Vm& vm) { return vm.attachBlockFd(fd, readOnly == JNI_TRUE, nullptr, labelValue); });
}

JNIEXPORT jint JNICALL Java_com_aerolon_ncv_NativeBridge_nativeAttachSuperContainerFd(
    JNIEnv* env, jobject, jlong h, jint fd, jstring label) {
  const std::string labelValue = label ? stringFromJava(env, label) : std::string("super");
  return withVm(h, [&](ncv::Vm& vm) { return vm.attachSuperContainerFd(fd, labelValue); });
}

JNIEXPORT jint JNICALL Java_com_aerolon_ncv_NativeBridge_nativeAttachLogicalPartitionFd(
    JNIEnv* env, jobject, jlong h, jint fd, jlong logicalSizeBytes,
    jlongArray logicalOffsetsArray, jlongArray lengthsArray, jlongArray physicalOffsetsArray,
    jintArray sourceIndicesArray, jstring label) {
  try {
    if (logicalSizeBytes <= 0 || logicalOffsetsArray == nullptr || lengthsArray == nullptr ||
        physicalOffsetsArray == nullptr || sourceIndicesArray == nullptr) return -static_cast<jint>(ncv::Status::InvalidArgument);
    const jsize count = env->GetArrayLength(logicalOffsetsArray);
    if (count <= 0 || count > 262144 || env->GetArrayLength(lengthsArray) != count ||
        env->GetArrayLength(physicalOffsetsArray) != count || env->GetArrayLength(sourceIndicesArray) != count)
      return -static_cast<jint>(ncv::Status::InvalidArgument);
    std::vector<jlong> logicalOffsets(static_cast<size_t>(count));
    std::vector<jlong> lengths(static_cast<size_t>(count));
    std::vector<jlong> physicalOffsets(static_cast<size_t>(count));
    std::vector<jint> sourceIndices(static_cast<size_t>(count));
    env->GetLongArrayRegion(logicalOffsetsArray, 0, count, logicalOffsets.data());
    env->GetLongArrayRegion(lengthsArray, 0, count, lengths.data());
    env->GetLongArrayRegion(physicalOffsetsArray, 0, count, physicalOffsets.data());
    env->GetIntArrayRegion(sourceIndicesArray, 0, count, sourceIndices.data());
    if (env->ExceptionCheck()) return kException;
    std::vector<ncv::LogicalBlockExtent> extents;
    extents.reserve(static_cast<size_t>(count));
    for (jsize i = 0; i < count; ++i) {
      if (logicalOffsets[static_cast<size_t>(i)] < 0 || lengths[static_cast<size_t>(i)] <= 0 || sourceIndices[static_cast<size_t>(i)] < 0)
        return -static_cast<jint>(ncv::Status::InvalidArgument);
      ncv::LogicalBlockExtent extent;
      extent.logicalOffsetBytes = static_cast<uint64_t>(logicalOffsets[static_cast<size_t>(i)]);
      extent.lengthBytes = static_cast<uint64_t>(lengths[static_cast<size_t>(i)]);
      extent.sourceDeviceIndex = static_cast<uint32_t>(sourceIndices[static_cast<size_t>(i)]);
      const jlong physical = physicalOffsets[static_cast<size_t>(i)];
      if (physical == -1) extent.zero = true;
      else if (physical < 0) return -static_cast<jint>(ncv::Status::InvalidArgument);
      else extent.physicalOffsetBytes = static_cast<uint64_t>(physical);
      extents.push_back(extent);
    }
    const std::string labelValue = label ? stringFromJava(env, label) : std::string("logical-partition");
    return withVm(h, [&](ncv::Vm& vm) {
      return vm.attachLogicalPartitionFd(fd, static_cast<uint64_t>(logicalSizeBytes), extents, labelValue);
    });
  } catch (...) { return kException; }
}

JNIEXPORT jstring JNICALL Java_com_aerolon_ncv_NativeBridge_nativeGetBlockDevicesDiagnostics(JNIEnv* env, jobject, jlong h) {
  try {
    std::shared_ptr<ncv::Vm> vm = lookup(h);
    if (!vm) return env->NewStringUTF("bad-handle");
    const std::string report = asciiOnly(vm->blockDevicesDiagnostics());
    return env->NewStringUTF(report.c_str());
  } catch (...) { return env->NewStringUTF("block-device diagnostics unavailable"); }
}

JNIEXPORT jstring JNICALL Java_com_aerolon_ncv_NativeBridge_nativeGetCompatibilityReport(JNIEnv* env, jobject, jlong h) {
  try {
    std::shared_ptr<ncv::Vm> vm = lookup(h);
    if (!vm) return env->NewStringUTF("No VM handle.");
    const std::string report = asciiOnly(vm->compatibilityReport());
    return env->NewStringUTF(report.c_str());
  } catch (...) {
    return env->NewStringUTF("Compatibility report unavailable.");
  }
}

JNIEXPORT jstring JNICALL Java_com_aerolon_ncv_NativeBridge_nativeGetMtkProbeReport(JNIEnv* env, jobject, jlong h) {
  try {
    std::shared_ptr<ncv::Vm> vm = lookup(h);
    if (!vm) return env->NewStringUTF("No VM handle.");
    const std::string report = asciiOnly(vm->mmioProbeReport());
    return env->NewStringUTF(report.c_str());
  } catch (...) {
    return env->NewStringUTF("MTK probe report unavailable.");
  }
}

JNIEXPORT jint JNICALL Java_com_aerolon_ncv_NativeBridge_nativeGetBlockDeviceCount(JNIEnv*, jobject, jlong h) {
  try {
    std::shared_ptr<ncv::Vm> vm = lookup(h);
    return vm ? static_cast<jint>(vm->blockDeviceCount()) : kBadHandle;
  } catch (...) { return kException; }
}

JNIEXPORT jint JNICALL Java_com_aerolon_ncv_NativeBridge_nativeSetEntry(JNIEnv*, jobject, jlong h, jlong pc) {
  return withVm(h, [&](ncv::Vm& vm) { return vm.setEntry(ncv::GuestAddr(static_cast<uint64_t>(pc))); });
}
JNIEXPORT jlong JNICALL Java_com_aerolon_ncv_NativeBridge_nativePrepareLinuxBootDtb(JNIEnv* env, jobject, jlong h, jstring bootargs, jlong initrdAddr, jlong initrdSize) {
  try {
    if (bootargs == nullptr || initrdAddr < 0 || initrdSize < 0) return -static_cast<jlong>(ncv::Status::InvalidArgument);
    const char* chars = env->GetStringUTFChars(bootargs, nullptr);
    if (chars == nullptr) return kException;
    const std::string args(chars);
    env->ReleaseStringUTFChars(bootargs, chars);
    std::shared_ptr<ncv::Vm> vm = lookup(h);
    if (!vm) return kBadHandle;
    ncv::GuestAddr addr;
    const ncv::Status status = vm->prepareLinuxBootDtb(args, &addr, static_cast<uint64_t>(initrdAddr), static_cast<uint64_t>(initrdSize));
    if (status != ncv::Status::Ok) return -static_cast<jlong>(status);
    return static_cast<jlong>(addr.value);
  } catch (...) {
    return kException;
  }
}
JNIEXPORT jlong JNICALL Java_com_aerolon_ncv_NativeBridge_nativePrepareAndroidBootDtb(JNIEnv* env, jobject, jlong h, jstring bootargs, jlong initrdAddr, jlong initrdSize) {
  try {
    if (bootargs == nullptr || initrdAddr < 0 || initrdSize < 0) return -static_cast<jlong>(ncv::Status::InvalidArgument);
    const char* chars = env->GetStringUTFChars(bootargs, nullptr);
    if (chars == nullptr) return kException;
    const std::string args(chars);
    env->ReleaseStringUTFChars(bootargs, chars);
    std::shared_ptr<ncv::Vm> vm = lookup(h);
    if (!vm) return kBadHandle;
    ncv::GuestAddr addr;
    const ncv::Status status = vm->prepareAndroidBootDtb(args, &addr, static_cast<uint64_t>(initrdAddr), static_cast<uint64_t>(initrdSize));
    if (status != ncv::Status::Ok) return -static_cast<jlong>(status);
    return static_cast<jlong>(addr.value);
  } catch (...) { return kException; }
}

JNIEXPORT jint JNICALL Java_com_aerolon_ncv_NativeBridge_nativeStart(JNIEnv*, jobject, jlong h) {
  return withVm(h, [](ncv::Vm& vm) { return vm.start(); });
}
JNIEXPORT jint JNICALL Java_com_aerolon_ncv_NativeBridge_nativePause(JNIEnv*, jobject, jlong h) {
  return withVm(h, [](ncv::Vm& vm) { return vm.pause(); });
}
JNIEXPORT jint JNICALL Java_com_aerolon_ncv_NativeBridge_nativeResume(JNIEnv*, jobject, jlong h) {
  return withVm(h, [](ncv::Vm& vm) { return vm.resume(); });
}
JNIEXPORT jint JNICALL Java_com_aerolon_ncv_NativeBridge_nativeStop(JNIEnv*, jobject, jlong h) {
  return withVm(h, [](ncv::Vm& vm) { return vm.stop(); });
}
JNIEXPORT jint JNICALL Java_com_aerolon_ncv_NativeBridge_nativeReset(JNIEnv*, jobject, jlong h) {
  return withVm(h, [](ncv::Vm& vm) { return vm.reset(); });
}

// VmState sayısı (0 Created, 1 Running, 2 Paused, 3 Stopped, 4 Faulted) veya -100 / -101.
JNIEXPORT jint JNICALL Java_com_aerolon_ncv_NativeBridge_nativeGetState(JNIEnv*, jobject, jlong h) {
  return withVm(h, [](ncv::Vm& vm) { return static_cast<int32_t>(vm.state()); });
}

// take=true: okunanı tüketir. Ham baytlar döner (Kotlin tarafında decode edilir). Geçersiz handle -> boş dizi.
JNIEXPORT jbyteArray JNICALL Java_com_aerolon_ncv_NativeBridge_nativeGetConsole(JNIEnv* env, jobject, jlong h,
                                                                                jboolean take) {
  try {
    std::shared_ptr<ncv::Vm> vm = lookup(h);
    if (!vm) return toByteArray(env, std::string());
    return toByteArray(env, take ? vm->takeConsoleOutput() : vm->consoleOutput());
  } catch (...) {
    return toByteArray(env, std::string());
  }
}

JNIEXPORT jintArray JNICALL Java_com_aerolon_ncv_NativeBridge_nativeGetFramebufferArgb(JNIEnv* env, jobject, jlong h) {
  try {
    std::shared_ptr<ncv::Vm> vm = lookup(h);
    if (!vm) return env->NewIntArray(0);
    const std::vector<uint32_t> pixels = vm->framebufferArgb();
    if (pixels.size() > static_cast<size_t>(std::numeric_limits<jsize>::max())) return env->NewIntArray(0);
    jintArray result = env->NewIntArray(static_cast<jsize>(pixels.size()));
    if (result && !pixels.empty()) env->SetIntArrayRegion(result, 0, static_cast<jsize>(pixels.size()), reinterpret_cast<const jint*>(pixels.data()));
    return result;
  } catch (...) { return env->NewIntArray(0); }
}

JNIEXPORT jstring JNICALL Java_com_aerolon_ncv_NativeBridge_nativeGetStopInfo(JNIEnv* env, jobject, jlong h) {
  try {
    std::shared_ptr<ncv::Vm> vm = lookup(h);
    if (!vm) return env->NewStringUTF("bad-handle");
    const ncv::StopInfo s = vm->lastStop();
    char buf[256];
    std::snprintf(buf, sizeof(buf), "reason=%s pc=0x%llx insn=0x%08x bus=%s addr=0x%llx imm=0x%x",
                  ncv::stopReasonName(s.reason), static_cast<unsigned long long>(s.pc), s.insn,
                  ncv::statusName(s.busStatus), static_cast<unsigned long long>(s.faultAddr), s.imm);
    std::string report(buf);
    if (s.reason == ncv::StopReason::UnsupportedInstruction && s.insn != 0)
      report += "\n" + instructionHint(s.insn);
    return env->NewStringUTF(asciiOnly(report).c_str());
  } catch (...) {
    return env->NewStringUTF("exception");
  }
}

}  // extern "C"
