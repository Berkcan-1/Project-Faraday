#include "ncv_vm.h"

#include <system_error>
#include <algorithm>
#include <utility>
#include <limits>
#include <new>
#include <vector>
#include <sstream>
#include <string_view>
#include <cctype>

namespace ncv {
namespace {
// Linux arm64 direct-entry state: SCTLR_EL1 RES1 bits with M/C/I clear.
// The kernel's own head code will enable the MMU/caches after setting up page tables.
constexpr uint64_t kLinuxArm64SctlrEl1Entry = 0x30D00800ull;

void prepareLinuxArm64EntryState(CpuState& state, uint64_t dtbAddress) {
  state.sctlr_el1 = kLinuxArm64SctlrEl1Entry;
  state.daif = 0xF;  // Mask Debug, SError, IRQ, FIQ.
  state.x[0] = dtbAddress;
  state.x[1] = 0;
  state.x[2] = 0;
  state.x[3] = 0;
}

bool alignUp(uint64_t value, uint64_t alignment, uint64_t* out) {
  if (!out || alignment == 0 || (alignment & (alignment - 1)) != 0 || value > UINT64_MAX - (alignment - 1)) return false;
  *out = (value + alignment - 1) & ~(alignment - 1);
  return true;
}

std::string trimCopy(std::string value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
  if (first == value.end()) return {};
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  return std::string(first, last);
}

bool isConsoleBootKey(std::string key) {
  key = trimCopy(std::move(key));
  // These keys point to physical-device consoles, which are not part of NCV's virtual board.
  return key == "console" || key == "earlycon" || key == "earlyprintk" ||
         key == "androidboot.console" || key == "kernel.console" || key == "kernel.earlycon";
}

std::string sanitizeKernelCommandLine(const std::string& input) {
  std::istringstream stream(input);
  std::string token, result;
  while (stream >> token) {
    const size_t equals = token.find('=');
    const std::string key = token.substr(0, equals);
    if (isConsoleBootKey(key)) continue;
    if (!result.empty()) result.push_back(' ');
    result += token;
    if (result.size() > 3500) break;  // leave space for NCV console arguments
  }
  constexpr std::string_view ncvConsole = "console=ttyAMA0 earlycon=pl011,mmio32,0x09000000";
  if (!result.empty()) result.push_back(' ');
  result.append(ncvConsole.data(), ncvConsole.size());
  if (result.size() > 4096) result.resize(4096);
  return result;
}

std::vector<uint8_t> sanitizeBootConfig(const std::vector<uint8_t>& input) {
  if (input.empty()) return {};
  const std::string text(reinterpret_cast<const char*>(input.data()), input.size());
  std::istringstream stream(text);
  std::ostringstream output;
  std::string line;
  while (std::getline(stream, line)) {
    const std::string cleaned = trimCopy(line);
    const size_t equals = cleaned.find('=');
    if (equals != std::string::npos && isConsoleBootKey(cleaned.substr(0, equals))) continue;
    output << line << '\n';
  }
  const std::string sanitized = output.str();
  return std::vector<uint8_t>(sanitized.begin(), sanitized.end());
}
}

const char* vmStateName(VmState s) {
  switch (s) {
    case VmState::Created: return "Created";
    case VmState::Running: return "Running";
    case VmState::Paused: return "Paused";
    case VmState::Stopped: return "Stopped";
    case VmState::Faulted: return "Faulted";
  }
  return "Unknown";
}

Vm::Vm(const BoardConfig& cfg)
    : cfg_(cfg), bus_(cfg.maxRamBytes), rng_(&bus_), cpu_(&bus_, &gic_), entry_(cfg.ramBase), sp_(0) {}

std::unique_ptr<Vm> Vm::create(const BoardConfig& cfg, Status* status) {
  std::unique_ptr<Vm> vm(new Vm(cfg));
  const Status st = vm->init();
  if (status) *status = st;
  if (st != Status::Ok) return nullptr;
  return vm;
}

Status Vm::init() {
  if (cfg_.initialEL > 1 || cfg_.compatibilityProfileId > 1) return Status::InvalidArgument;
  try { blockDevices_.reserve(16); } catch (...) { return Status::OutOfMemory; }
  uart_.setIrqSink([this](bool asserted) { gic_.setLevel(33, asserted); });
  Status st = bus_.addRam(GuestAddr(cfg_.ramBase), cfg_.ramSize);
  if (st != Status::Ok) return st;
  st = bus_.addDevice(GuestAddr(cfg_.uartBase), Pl011Uart::kRegionSize, &uart_);
  if (st != Status::Ok) return st;
  st = bus_.addDevice(GuestAddr(0x09010000ull), Pl031Rtc::kRegionSize, &rtc_);
  if (st != Status::Ok) return st;
  rtc_.setIrqSink([this](bool asserted) { gic_.setLevel(34, asserted); });
  st = bus_.addDevice(GuestAddr(cfg_.gicBase), GicV2::kRegionSize, &gic_);
  if (st != Status::Ok) return st;
  st = bus_.addDevice(GuestAddr(GuestFramebuffer::kBase), GuestFramebuffer::kRegionSize, &framebuffer_);
  if (st != Status::Ok) return st;
  st = bus_.addDevice(GuestAddr(VirtioMmioRng::kBase), VirtioMmioRng::kRegionSize, &rng_);
  if (st != Status::Ok) return st;
  rng_.setIrqSink([this](bool asserted) { gic_.setLevel(63, asserted); });
  if (cfg_.compatibilityProfileId == 1) {
    // Explicitly opt-in diagnostic behavior: unresolved guest physical accesses receive zero/no-op.
    // This allows probe tracing, not full MTK hardware emulation. Default profile remains strict.
    bus_.setUnmappedAccessFallback(&mtkProbeShim_);
  }
  sp_ = (cfg_.ramBase + cfg_.ramSize) & ~0xFull;  // RAM'in tepesi, 16 bayt hizalı
  entry_ = cfg_.ramBase;
  return Status::Ok;
}

Vm::~Vm() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stopRequested_ = true;
  }
  cv_.notify_all();
  joinThread();
}

void Vm::joinThread() {
  if (thread_.joinable()) thread_.join();
}

VmState Vm::state() const {
  std::lock_guard<std::mutex> lk(mu_);
  return state_;
}

StopInfo Vm::lastStop() const {
  std::lock_guard<std::mutex> lk(mu_);
  return lastStop_;
}

Status Vm::loadPayload(GuestAddr addr, const uint8_t* data, uint64_t len) {
  std::lock_guard<std::mutex> lk(mu_);
  if (state_ != VmState::Created) return Status::InvalidState;
  return bus_.loadBytes(addr, data, len);
}

Status Vm::setEntry(GuestAddr pc) {
  std::lock_guard<std::mutex> lk(mu_);
  if (state_ != VmState::Created) return Status::InvalidState;
  entry_ = pc.value;
  return Status::Ok;
}

Status Vm::loadLinuxImage(const uint8_t* data, uint64_t len, GuestAddr* outEntry) {
  std::lock_guard<std::mutex> lk(mu_);
  if (state_ != VmState::Created) return Status::InvalidState;
  if (bootArg0_ != 0) return Status::InvalidState;  // load kernel before placing the final DTB
  Arm64LinuxImageInfo info;
  const Status loaded = loadArm64LinuxImage(bus_, data, len, cfg_.ramBase, cfg_.ramSize, &info);
  if (loaded != Status::Ok) return loaded;
  linuxImageInfo_ = info;
  linuxImageLoaded_ = true;
  entry_ = info.loadAddress;
  cfg_.initialEL = 1;
  if (outEntry) *outEntry = GuestAddr(entry_);
  return Status::Ok;
}

Status Vm::loadAndroidBootBundle(const uint8_t* bootData, uint64_t bootSize,
                                    const uint8_t* vendorData, uint64_t vendorSize,
                                    const uint8_t* initBootData, uint64_t initBootSize,
                                    AndroidBootLoadInfo* outInfo) {
  if (!bootData || !outInfo) return Status::InvalidArgument;
  AndroidBootBundle bundle;
  const Status parsed = extractAndroidBootBundle(bootData, bootSize, vendorData, vendorSize, initBootData, initBootSize, &bundle);
  if (parsed != Status::Ok) return parsed;
  if (bundle.kernel.empty() || bundle.genericRamdisk.empty()) return Status::InvalidArgument;

  // Initramfs format: vendor ramdisk fragments first, followed by the generic boot ramdisk.
  // Strip physical-console overrides so NCV's PL011 console remains selected. Other
  // androidboot.* keys are preserved because vendor/init may depend on them.
  // Android bootconfig, when present (v4 vendor_boot), is appended with its kernel trailer.
  const std::vector<uint8_t> bootConfig = sanitizeBootConfig(bundle.bootConfig);
  std::vector<uint8_t> initrd;
  try {
    const uint64_t baseSize = static_cast<uint64_t>(bundle.vendorRamdisk.size()) + bundle.genericRamdisk.size();
    if (baseSize > (512ull << 20) || bootConfig.size() > (16ull << 20) ||
        baseSize > SIZE_MAX - bootConfig.size() - 32) return Status::OutOfBounds;
    initrd.reserve(static_cast<size_t>(baseSize + bootConfig.size() + 32));
    initrd.insert(initrd.end(), bundle.vendorRamdisk.begin(), bundle.vendorRamdisk.end());
    initrd.insert(initrd.end(), bundle.genericRamdisk.begin(), bundle.genericRamdisk.end());
    if (!bootConfig.empty()) {
      // Linux bootconfig trailer: [parameters][NUL padding][size LE32][checksum LE32][#BOOTCONFIG\n].
      const size_t start = initrd.size();
      initrd.insert(initrd.end(), bootConfig.begin(), bootConfig.end());
      while ((initrd.size() - start) & 3u) initrd.push_back(0);
      const size_t configSize = initrd.size() - start;
      if (configSize > UINT32_MAX) return Status::OutOfBounds;
      uint32_t checksum = 0;
      for (size_t i = start; i < initrd.size(); ++i) checksum += initrd[i];
      const uint32_t size32 = static_cast<uint32_t>(configSize);
      for (unsigned i = 0; i < 4; ++i) initrd.push_back(static_cast<uint8_t>(size32 >> (i * 8)));
      for (unsigned i = 0; i < 4; ++i) initrd.push_back(static_cast<uint8_t>(checksum >> (i * 8)));
      const char magic[] = "#BOOTCONFIG\n";
      initrd.insert(initrd.end(), magic, magic + sizeof(magic) - 1);
    }
  } catch (const std::bad_alloc&) {
    return Status::OutOfMemory;
  } catch (...) {
    return Status::OutOfMemory;
  }
  if (initrd.empty()) return Status::InvalidArgument;

  std::lock_guard<std::mutex> lk(mu_);
  if (state_ != VmState::Created || bootArg0_ != 0 || linuxImageLoaded_) return Status::InvalidState;
  Arm64LinuxImageInfo info;
  const Status loaded = loadArm64LinuxImage(bus_, bundle.kernel.data(), bundle.kernel.size(),
                                             cfg_.ramBase, cfg_.ramSize, &info);
  if (loaded != Status::Ok) return loaded;

  uint64_t ramEnd = 0;
  if (cfg_.ramBase > UINT64_MAX - cfg_.ramSize) return Status::OutOfBounds;
  ramEnd = cfg_.ramBase + cfg_.ramSize;
  uint64_t kernelEnd = 0;
  if (info.loadAddress > UINT64_MAX - info.imageSize) return Status::OutOfBounds;
  kernelEnd = info.loadAddress + info.imageSize;
  uint64_t initrdBase = 0;
  if (kernelEnd > UINT64_MAX - (2ull << 20) ||
      !alignUp(kernelEnd + (2ull << 20), 4096, &initrdBase)) return Status::OutOfBounds;
  constexpr uint64_t kTopReserve = 2ull << 20;
  if (initrdBase < cfg_.ramBase || initrdBase > ramEnd || initrd.size() > ramEnd - initrdBase ||
      ramEnd - (initrdBase + initrd.size()) < kTopReserve) return Status::OutOfBounds;
  if (initrdBase < kernelEnd && initrdBase + initrd.size() > info.loadAddress) return Status::InvalidArgument;
  const Status initrdLoaded = bus_.loadBytes(GuestAddr(initrdBase), initrd.data(), initrd.size());
  if (initrdLoaded != Status::Ok) return initrdLoaded;

  linuxImageInfo_ = info;
  linuxImageLoaded_ = true;
  entry_ = info.loadAddress;
  cfg_.initialEL = 1;
  loadedInitrdBase_ = initrdBase;
  loadedInitrdSize_ = initrd.size();
  // Legacy boot.img v2 can carry its DTB in boot.img. GKI-style images usually put
  // the platform DTB in vendor_boot.img. Prefer the boot image payload when present,
  // otherwise use the matching vendor_boot DTB instead of silently discarding it.
  loadedBootDtb_ = !bundle.bootDtb.empty() ? bundle.bootDtb : bundle.vendorDtb;
  loadedDtbReport_.clear();
  loadedBootArgs_ = sanitizeKernelCommandLine(bundle.commandLine);

  outInfo->entry = entry_;
  outInfo->initrdAddress = initrdBase;
  outInfo->initrdSize = initrd.size();
  outInfo->headerVersion = bundle.headerVersion;
  outInfo->selectedDtbSize = static_cast<uint64_t>(loadedBootDtb_.size());
  outInfo->bootDtbSize = static_cast<uint64_t>(bundle.bootDtb.size());
  outInfo->vendorDtbSize = static_cast<uint64_t>(bundle.vendorDtb.size());
  outInfo->selectedDtbFromVendorBoot = bundle.bootDtb.empty() && !bundle.vendorDtb.empty();
  outInfo->kernelHeaderAddress = bundle.kernelHeaderAddress;
  outInfo->ramdiskHeaderAddress = bundle.ramdiskHeaderAddress;
  outInfo->tagsHeaderAddress = bundle.tagsHeaderAddress;
  outInfo->dtbHeaderAddress = bundle.dtbHeaderAddress;
  outInfo->commandLine = loadedBootArgs_;
  return Status::Ok;
}

Status Vm::attachBlockFd(int fd, bool readOnly, uint64_t* outSectors, const std::string& label) {
  std::lock_guard<std::mutex> lk(mu_);
  if (state_ != VmState::Created || blockDevices_.size() >= 15) return Status::InvalidState;
  std::unique_ptr<VirtioMmioBlock> dev;
  const Status opened = VirtioMmioBlock::openFd(&bus_, fd, readOnly, &dev);
  if (opened != Status::Ok) return opened;
  dev->setLabel(label);
  const size_t index = blockDevices_.size();
  const uint64_t base = 0x0A000000ull + static_cast<uint64_t>(index) * 0x200ull;
  const unsigned irq = 48u + static_cast<unsigned>(index);
  dev->setIrqSink([this, irq](bool level) { gic_.setLevel(irq, level); });
  const Status mapped = bus_.addDevice(GuestAddr(base), VirtioMmioBlock::kRegionSize, dev.get());
  if (mapped != Status::Ok) return mapped;
  if (outSectors) *outSectors = dev->capacitySectors();
  blockDevices_.push_back(std::move(dev));
  return Status::Ok;
}

Status Vm::attachSuperContainerFd(int fd, const std::string& label) {
  std::lock_guard<std::mutex> lk(mu_);
  if (state_ != VmState::Created || blockDevices_.size() >= 15) return Status::InvalidState;
  std::unique_ptr<VirtioMmioBlock> dev;
  const Status opened = VirtioMmioBlock::openSuperContainerFd(&bus_, fd, label, &dev);
  if (opened != Status::Ok) return opened;
  const size_t index = blockDevices_.size();
  const uint64_t base = 0x0A000000ull + static_cast<uint64_t>(index) * 0x200ull;
  const unsigned irq = 48u + static_cast<unsigned>(index);
  dev->setIrqSink([this, irq](bool level) { gic_.setLevel(irq, level); });
  const Status mapped = bus_.addDevice(GuestAddr(base), VirtioMmioBlock::kRegionSize, dev.get());
  if (mapped != Status::Ok) return mapped;
  blockDevices_.push_back(std::move(dev));
  return Status::Ok;
}

Status Vm::attachLogicalPartitionFd(int fd, uint64_t logicalSizeBytes,
                                    const std::vector<LogicalBlockExtent>& extents,
                                    const std::string& label) {
  std::lock_guard<std::mutex> lk(mu_);
  if (state_ != VmState::Created || blockDevices_.size() >= 15) return Status::InvalidState;
  std::unique_ptr<VirtioMmioBlock> dev;
  const Status opened = VirtioMmioBlock::openPartitionView(&bus_, fd, logicalSizeBytes, extents, label, &dev);
  if (opened != Status::Ok) return opened;
  const size_t index = blockDevices_.size();
  const uint64_t base = 0x0A000000ull + static_cast<uint64_t>(index) * 0x200ull;
  const unsigned irq = 48u + static_cast<unsigned>(index);
  dev->setIrqSink([this, irq](bool level) { gic_.setLevel(irq, level); });
  const Status mapped = bus_.addDevice(GuestAddr(base), VirtioMmioBlock::kRegionSize, dev.get());
  if (mapped != Status::Ok) return mapped;
  blockDevices_.push_back(std::move(dev));
  return Status::Ok;
}

size_t Vm::blockDeviceCount() const {
  std::lock_guard<std::mutex> lk(mu_);
  return blockDevices_.size();
}

std::string Vm::blockDevicesDiagnostics() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::ostringstream out;
  out << "[NCV BLOCK IO] guest-visible virtio block devices=" << blockDevices_.size();
  for (size_t i = 0; i < blockDevices_.size(); ++i) {
    out << "\n  /dev/block/vd" << static_cast<char>('a' + static_cast<int>(i)) << " "
        << blockDevices_[i]->diagnostics();
  }
  out << "\n[NCV BLOCK LIMIT] A linear_view is an NCV host-side read-only extent mapping exposed as a virtio disk; it is not a guest /dev/block/mapper dm-linear target.";
  return out.str();
}

std::string Vm::compatibilityReport() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::string report = cfg_.compatibilityProfileId == 1
      ? mtkProbeShim_.report()
      : "Strict NCV virtual-board profile active; unmapped guest physical accesses fault normally.";
  if (!loadedDtbReport_.empty()) report += "\n[NCV] Hybrid boot DTB: " + loadedDtbReport_;
  return report;
}

std::string Vm::mmioProbeReport() const {
  if (cfg_.compatibilityProfileId != 1) return "MTK probe profile disabled.";
  return mtkProbeShim_.report();
}

std::string Vm::loadedBootArgs() const {
  std::lock_guard<std::mutex> lk(mu_);
  return loadedBootArgs_;
}

std::string Vm::runtimeDiagnostics() const {
  std::ostringstream out;
  const uint64_t pc = runtimePc_.load(std::memory_order_acquire);
  const uint32_t insn = runtimeLastInstruction_.load(std::memory_order_acquire);
  const uint32_t same = runtimeSamePcSlices_.load(std::memory_order_acquire);
  const uint64_t esr = runtimeEsr_.load(std::memory_order_acquire);
  const uint8_t ec = static_cast<uint8_t>((esr >> 26) & 0x3Fu);
  const uint8_t dfsc = static_cast<uint8_t>(esr & 0x3Fu);
  out << "state=" << vmStateName(state())
      << " ram_capacity_bytes=" << std::dec << cfg_.ramSize
      << " ram_resident_bytes=" << bus_.residentRamBytes()
      << " fb_read_ops=" << framebuffer_.readOperations()
      << " fb_write_ops=" << framebuffer_.writeOperations()
      << " fb_nonzero_write_ops=" << framebuffer_.nonZeroWriteOperations()
      << " pc=0x" << std::hex << pc
      << " last_insn=0x" << insn
      << " x0=0x" << runtimeX0_.load(std::memory_order_acquire)
      << " x1=0x" << runtimeX1_.load(std::memory_order_acquire)
      << " x2=0x" << runtimeX2_.load(std::memory_order_acquire)
      << " x3=0x" << runtimeX3_.load(std::memory_order_acquire)
      << " sctlr_el1=0x" << runtimeSctlr_.load(std::memory_order_acquire)
      << " esr_el1=0x" << runtimeEsr_.load(std::memory_order_acquire)
      << " far_el1=0x" << runtimeFar_.load(std::memory_order_acquire)
      << " elr_el1=0x" << runtimeElr_.load(std::memory_order_acquire)
      << " spsr_el1=0x" << runtimeSpsr_.load(std::memory_order_acquire)
      << " last_abort_pc=0x" << runtimeLastAbortPc_.load(std::memory_order_acquire)
      << " last_abort_insn=0x" << runtimeLastAbortInsn_.load(std::memory_order_acquire)
      << " last_abort_va=0x" << runtimeLastAbortVa_.load(std::memory_order_acquire)
      << " last_abort_pa=0x" << runtimeLastAbortPa_.load(std::memory_order_acquire)
      << " last_abort_size=" << std::dec << runtimeLastAbortSize_.load(std::memory_order_acquire)
      << " last_abort_access=" << (runtimeLastAbortAccess_.load(std::memory_order_acquire) == 1 ? "write" :
                                     runtimeLastAbortAccess_.load(std::memory_order_acquire) == 2 ? "execute" :
                                     runtimeLastAbortAccess_.load(std::memory_order_acquire) == 0 ? "read" : "none")
      << " last_abort_bus=" << statusName(static_cast<Status>(runtimeLastAbortBusStatus_.load(std::memory_order_acquire)))
      << " last_abort_table_walk=" << (runtimeLastAbortTableWalk_.load(std::memory_order_acquire) ? 1 : 0)
      // Return to hex after the decimal access-size field above. These values
      // have a 0x prefix and must not be rendered as decimal.
      << std::hex
      << " last_abort_x8=0x" << runtimeLastAbortX8_.load(std::memory_order_acquire)
      << " last_abort_tcr_el1=0x" << runtimeLastAbortTcr_.load(std::memory_order_acquire)
      << " last_abort_ttbr0_el1=0x" << runtimeLastAbortTtbr0_.load(std::memory_order_acquire)
      << " last_abort_ttbr1_el1=0x" << runtimeLastAbortTtbr1_.load(std::memory_order_acquire)
      << " last_abort_walk_root=0x" << runtimeLastAbortWalkRoot_.load(std::memory_order_acquire)
      << " last_abort_walk_va_page=0x" << runtimeLastAbortWalkVaPage_.load(std::memory_order_acquire)
      << " last_abort_l1_desc=0x" << runtimeLastAbortL1Desc_.load(std::memory_order_acquire)
      << " last_abort_l2_desc=0x" << runtimeLastAbortL2Desc_.load(std::memory_order_acquire)
      << " last_abort_l3_desc=0x" << runtimeLastAbortL3Desc_.load(std::memory_order_acquire)
      << " last_abort_l3_entry_pa=0x" << runtimeLastAbortL3EntryPa_.load(std::memory_order_acquire)
      << " last_abort_pte_writer_found=" << std::dec << (runtimeLastAbortPteWriterFound_.load(std::memory_order_acquire) ? 1 : 0)
      << " last_abort_pte_writer_pc=0x" << std::hex << runtimeLastAbortPteWriterPc_.load(std::memory_order_acquire)
      << " last_abort_pte_writer_insn=0x" << runtimeLastAbortPteWriterInsn_.load(std::memory_order_acquire)
      << " last_abort_pte_writer_va=0x" << runtimeLastAbortPteWriterVa_.load(std::memory_order_acquire)
      << " last_abort_pte_writer_pa=0x" << runtimeLastAbortPteWriterPa_.load(std::memory_order_acquire)
      << " last_abort_pte_writer_value=0x" << runtimeLastAbortPteWriterValue_.load(std::memory_order_acquire)
      << " last_abort_pte_writer_size=" << std::dec << runtimeLastAbortPteWriterSize_.load(std::memory_order_acquire)
      << " last_abort_walk_ttbr1=" << (runtimeLastAbortWalkTtbr1_.load(std::memory_order_acquire) ? 1 : 0)
      << " last_abort_tlb_hit=" << (runtimeLastAbortTlbHit_.load(std::memory_order_acquire) ? 1 : 0)
      << " last_abort_fresh_walk_attempted=" << (runtimeLastAbortFreshWalkAttempted_.load(std::memory_order_acquire) ? 1 : 0)
      << " last_abort_fresh_walk_ok=" << (runtimeLastAbortFreshWalkOk_.load(std::memory_order_acquire) ? 1 : 0)
      << " last_abort_fresh_pa=0x" << std::hex << runtimeLastAbortFreshPa_.load(std::memory_order_acquire)
      << " last_abort_fresh_root=0x" << runtimeLastAbortFreshRoot_.load(std::memory_order_acquire)
      << " last_abort_fresh_l1_desc=0x" << runtimeLastAbortFreshL1Desc_.load(std::memory_order_acquire)
      << " last_abort_fresh_l2_desc=0x" << runtimeLastAbortFreshL2Desc_.load(std::memory_order_acquire)
      << " last_abort_fresh_l3_desc=0x" << runtimeLastAbortFreshL3Desc_.load(std::memory_order_acquire)
      << " last_abort_fresh_fsc=0x" << runtimeLastAbortFreshFsc_.load(std::memory_order_acquire)
      << " last_abort_fresh_table_walk=" << std::dec << (runtimeLastAbortFreshTableWalk_.load(std::memory_order_acquire) ? 1 : 0)
      << " last_abort_fresh_fault_pa=0x" << std::hex << runtimeLastAbortFreshFaultPa_.load(std::memory_order_acquire)
      << " last_abort_fresh_bus=" << statusName(static_cast<Status>(runtimeLastAbortFreshBusStatus_.load(std::memory_order_acquire)))
      << std::dec
      << " EL=" << runtimeCurrentEl_.load(std::memory_order_acquire)
      << " retired=" << runtimeInstructions_.load(std::memory_order_acquire)
      << " slices=" << runtimeSlices_.load(std::memory_order_acquire)
      << " same_pc_slices=" << same;
  if (ec == 0x25 || ec == 0x24) {
    out << " ESR_decode=data-abort-" << (ec == 0x25 ? "same-EL" : "lower-EL")
        << ",write=" << (((esr >> 6) & 1u) ? 1 : 0) << ",DFSC=0x" << std::hex
        << static_cast<unsigned>(dfsc) << ":";
    if (dfsc == 0x10) out << "sync-external-abort-not-table-walk";
    else if (dfsc >= 0x14 && dfsc <= 0x17)
      out << "sync-external-abort-table-walk-level-" << std::dec << static_cast<unsigned>(dfsc - 0x14);
    else if (dfsc >= 0x04 && dfsc <= 0x07)
      out << "translation-fault-level-" << std::dec << static_cast<unsigned>(dfsc - 0x04);
    else if (dfsc >= 0x09 && dfsc <= 0x0B)
      out << "access-flag-fault-level-" << std::dec << static_cast<unsigned>(dfsc - 0x09 + 1);
    else if (dfsc >= 0x0D && dfsc <= 0x0F)
      out << "permission-fault-level-" << std::dec << static_cast<unsigned>(dfsc - 0x0D + 1);
    else out << "other-or-unimplemented-fault-code";
  }
  if (same >= 5 && insn == 0x14000000u && runtimeSamePc_.load(std::memory_order_acquire) == pc)
    out << " suspected_spin_loop=1 (A64 B . self-branch; kernel may be parked or on an early-failure path)";
  return out.str();
}

Status Vm::prepareLinuxBootDtb(const std::string& bootargs, GuestAddr* outAddr,
                               uint64_t initrdBase, uint64_t initrdSize) {
  std::lock_guard<std::mutex> lk(mu_);
  if (state_ != VmState::Created) return Status::InvalidState;
  LinuxDtbConfig dc;
  dc.ramBase = cfg_.ramBase;
  dc.ramSize = cfg_.ramSize;
  dc.uartBase = cfg_.uartBase;
  dc.gicBase = cfg_.gicBase;
  dc.virtioBlockCount = static_cast<uint32_t>(blockDevices_.size());
  dc.virtioRngBase = VirtioMmioRng::kBase;
  dc.framebufferBase = GuestFramebuffer::kBase;
  dc.framebufferWidth = GuestFramebuffer::kWidth;
  dc.framebufferHeight = GuestFramebuffer::kHeight;
  dc.framebufferStride = GuestFramebuffer::kStride;
  dc.initrdBase = initrdBase;
  dc.initrdSize = initrdSize;
  dc.bootargs = bootargs;
  std::vector<uint8_t> blob;
  const Status generated = buildLinuxDtb(dc, &blob);
  if (generated != Status::Ok) return generated;
  if (cfg_.ramBase > UINT64_MAX - cfg_.ramSize || blob.size() >= cfg_.ramSize) return Status::OutOfMemory;
  const uint64_t ramEnd = cfg_.ramBase + cfg_.ramSize;
  const uint64_t addrValue = (ramEnd - static_cast<uint64_t>(blob.size())) & ~7ull;
  if (addrValue < cfg_.ramBase) return Status::OutOfMemory;
  const uint64_t dtbEnd = addrValue + static_cast<uint64_t>(blob.size());
  if (dtbEnd < addrValue || dtbEnd > ramEnd) return Status::OutOfMemory;

  // Keep a 64 KiB scratch/initial-stack gap below the DTB. The stack grows downward,
  // so starting just below the DTB avoids writing on top of the FDT during early entry.
  constexpr uint64_t kBootStackReserve = 64ull << 10;
  if (addrValue < cfg_.ramBase || addrValue - cfg_.ramBase < kBootStackReserve)
    return Status::OutOfMemory;
  const uint64_t safeTop = addrValue - kBootStackReserve;
  if (initrdSize != 0) {
    if (initrdBase > UINT64_MAX - initrdSize || initrdBase < cfg_.ramBase ||
        initrdBase + initrdSize > safeTop) return Status::InvalidArgument;
    if (linuxImageLoaded_) {
      const uint64_t kernelEnd = linuxImageInfo_.loadAddress + linuxImageInfo_.imageSize;
      if (initrdBase < kernelEnd && initrdBase + initrdSize > linuxImageInfo_.loadAddress)
        return Status::InvalidArgument;
    }
  }
  if (linuxImageLoaded_) {
    const uint64_t kernelEnd = linuxImageInfo_.loadAddress + linuxImageInfo_.imageSize;
    if (kernelEnd > safeTop || (linuxImageInfo_.loadAddress < dtbEnd && kernelEnd > addrValue))
      return Status::InvalidArgument;
  }
  const Status loaded = bus_.loadBytes(GuestAddr(addrValue), blob.data(), blob.size());
  if (loaded != Status::Ok) return loaded;
  sp_ = (addrValue - 16ull) & ~0xFull;  // stack top inside the reserved gap, 16-byte aligned
  cfg_.initialEL = 1;  // Linux Image direct boot enters at EL1 in the current NCV boot profile.
  bootArg0_ = addrValue;
  if (outAddr) *outAddr = GuestAddr(addrValue);
  return Status::Ok;
}

Status Vm::prepareAndroidBootDtb(const std::string& bootargs, GuestAddr* outAddr,
                                 uint64_t initrdBase, uint64_t initrdSize) {
  std::lock_guard<std::mutex> lk(mu_);
  if (state_ != VmState::Created || loadedBootDtb_.empty() || !linuxImageLoaded_ ||
      bootargs.size() > 4096) return Status::InvalidState;
  LinuxDtbConfig dc;
  dc.ramBase = cfg_.ramBase; dc.ramSize = cfg_.ramSize; dc.uartBase = cfg_.uartBase; dc.gicBase = cfg_.gicBase;
  dc.virtioBlockCount = static_cast<uint32_t>(blockDevices_.size());
  dc.virtioRngBase = VirtioMmioRng::kBase; dc.framebufferBase = GuestFramebuffer::kBase;
  dc.framebufferWidth = GuestFramebuffer::kWidth; dc.framebufferHeight = GuestFramebuffer::kHeight;
  dc.framebufferStride = GuestFramebuffer::kStride; dc.initrdBase = initrdBase; dc.initrdSize = initrdSize;
  dc.bootargs = bootargs;
  std::vector<uint8_t> blob; std::string report;
  if (cfg_.ramBase > UINT64_MAX - cfg_.ramSize) return Status::OutOfMemory;
  const uint64_t ramEnd = cfg_.ramBase + cfg_.ramSize;
  constexpr uint64_t kBootStackReserve = 64ull << 10;
  auto computeDtbAddress = [&](size_t blobSize, uint64_t* out) -> bool {
    if (!out || blobSize >= cfg_.ramSize) return false;
    *out = (ramEnd - static_cast<uint64_t>(blobSize)) & ~7ull;
    return *out >= cfg_.ramBase && *out - cfg_.ramBase >= kBootStackReserve;
  };
  // First obtain the adapted DTB size, then reserve the 64 KiB boot-stack/scratch
  // interval in its FDT memreserve map. Adding a reservation grows the blob by 16
  // bytes, which shifts the DTB address; iterate until the recorded interval ends
  // exactly at the final DTB address.
  dc.bootScratchReserveBase = 0;
  dc.bootScratchReserveSize = 0;
  Status generated = buildHybridAndroidDtb(loadedBootDtb_.data(), loadedBootDtb_.size(), dc, &blob, &report);
  if (generated != Status::Ok) return generated;
  uint64_t addrValue = 0;
  if (!computeDtbAddress(blob.size(), &addrValue)) return Status::OutOfMemory;
  dc.bootScratchReserveSize = kBootStackReserve;
  bool stableReservation = false;
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    dc.bootScratchReserveBase = addrValue - kBootStackReserve;
    generated = buildHybridAndroidDtb(loadedBootDtb_.data(), loadedBootDtb_.size(), dc, &blob, &report);
    if (generated != Status::Ok) return generated;
    uint64_t nextAddr = 0;
    if (!computeDtbAddress(blob.size(), &nextAddr)) return Status::OutOfMemory;
    if (nextAddr == addrValue && dc.bootScratchReserveBase == nextAddr - kBootStackReserve) {
      stableReservation = true;
      break;
    }
    addrValue = nextAddr;
  }
  if (!stableReservation) return Status::InvalidState;
  const uint64_t dtbEnd = addrValue + static_cast<uint64_t>(blob.size());
  const uint64_t safeTop = addrValue - kBootStackReserve;
  if (dtbEnd < addrValue || dtbEnd > ramEnd) return Status::OutOfBounds;
  if (initrdSize != 0) {
    if (initrdBase > UINT64_MAX - initrdSize || initrdBase < cfg_.ramBase || initrdBase + initrdSize > safeTop)
      return Status::InvalidArgument;
    const uint64_t kernelEnd = linuxImageInfo_.loadAddress + linuxImageInfo_.imageSize;
    if (initrdBase < kernelEnd && initrdBase + initrdSize > linuxImageInfo_.loadAddress) return Status::InvalidArgument;
  }
  const uint64_t kernelEnd = linuxImageInfo_.loadAddress + linuxImageInfo_.imageSize;
  if (kernelEnd > safeTop || (linuxImageInfo_.loadAddress < dtbEnd && kernelEnd > addrValue)) return Status::InvalidArgument;
  const Status loaded = bus_.loadBytes(GuestAddr(addrValue), blob.data(), blob.size());
  if (loaded != Status::Ok) return loaded;
  sp_ = (addrValue - 16ull) & ~0xFull; cfg_.initialEL = 1; bootArg0_ = addrValue;
  loadedDtbReport_ = report;
  if (outAddr) *outAddr = GuestAddr(addrValue);
  return Status::Ok;
}

Status Vm::start() {
  std::lock_guard<std::mutex> lk(mu_);
  if (state_ != VmState::Created || syncUsed_ || (linuxImageLoaded_ && bootArg0_ == 0))
    return Status::InvalidState; // a loaded Linux Image must have its DTB prepared first
  cpu_.reset(entry_, sp_, cfg_.initialEL);
  if (linuxImageLoaded_) prepareLinuxArm64EntryState(cpu_.mutableState(), bootArg0_);
  else cpu_.mutableState().x[0] = bootArg0_;
  runtimePc_.store(cpu_.state().pc, std::memory_order_relaxed);
  runtimeInstructions_.store(0, std::memory_order_relaxed);
  runtimeSlices_.store(0, std::memory_order_relaxed);
  runtimeLastInstruction_.store(0, std::memory_order_relaxed);
  runtimeSamePcSlices_.store(0, std::memory_order_relaxed);
  runtimeSamePc_.store(0, std::memory_order_relaxed);
  runtimeX0_.store(0, std::memory_order_relaxed); runtimeX1_.store(0, std::memory_order_relaxed);
  runtimeX2_.store(0, std::memory_order_relaxed); runtimeX3_.store(0, std::memory_order_relaxed);
  runtimeSctlr_.store(0, std::memory_order_relaxed); runtimeEsr_.store(0, std::memory_order_relaxed);
  runtimeFar_.store(0, std::memory_order_relaxed); runtimeElr_.store(0, std::memory_order_relaxed);
  runtimeSpsr_.store(0, std::memory_order_relaxed);
  runtimeLastAbortPc_.store(0, std::memory_order_relaxed); runtimeLastAbortInsn_.store(0, std::memory_order_relaxed);
  runtimeLastAbortVa_.store(0, std::memory_order_relaxed); runtimeLastAbortPa_.store(0, std::memory_order_relaxed);
  runtimeLastAbortBusStatus_.store(static_cast<int32_t>(Status::Ok), std::memory_order_relaxed);
  runtimeLastAbortSize_.store(0, std::memory_order_relaxed); runtimeLastAbortAccess_.store(255, std::memory_order_relaxed);
  runtimeLastAbortTableWalk_.store(false, std::memory_order_relaxed);
  runtimeLastAbortTcr_.store(0, std::memory_order_relaxed); runtimeLastAbortTtbr0_.store(0, std::memory_order_relaxed);
  runtimeLastAbortTtbr1_.store(0, std::memory_order_relaxed); runtimeLastAbortX8_.store(0, std::memory_order_relaxed);
  runtimeLastAbortWalkRoot_.store(0, std::memory_order_relaxed); runtimeLastAbortWalkVaPage_.store(0, std::memory_order_relaxed);
  runtimeLastAbortL1Desc_.store(0, std::memory_order_relaxed); runtimeLastAbortL2Desc_.store(0, std::memory_order_relaxed);
  runtimeLastAbortL3Desc_.store(0, std::memory_order_relaxed);
  runtimeLastAbortL3EntryPa_.store(0, std::memory_order_relaxed);
  runtimeLastAbortPteWriterFound_.store(false, std::memory_order_relaxed);
  runtimeLastAbortPteWriterPc_.store(0, std::memory_order_relaxed);
  runtimeLastAbortPteWriterVa_.store(0, std::memory_order_relaxed);
  runtimeLastAbortPteWriterPa_.store(0, std::memory_order_relaxed);
  runtimeLastAbortPteWriterValue_.store(0, std::memory_order_relaxed);
  runtimeLastAbortPteWriterInsn_.store(0, std::memory_order_relaxed);
  runtimeLastAbortPteWriterSize_.store(0, std::memory_order_relaxed);
  runtimeLastAbortWalkTtbr1_.store(false, std::memory_order_relaxed);
  runtimeLastAbortTlbHit_.store(false, std::memory_order_relaxed);
  runtimeCurrentEl_.store(0, std::memory_order_relaxed);
  stopRequested_ = false;
  state_ = VmState::Running;
  try {
    thread_ = std::thread(&Vm::threadMain, this);
  } catch (const std::system_error&) {
    state_ = VmState::Created;
    return Status::OutOfMemory;
  }
  return Status::Ok;
}

void Vm::threadMain() {
  uint64_t previousPc = UINT64_MAX;
  uint32_t previousInsn = 0;
  uint32_t samePcSlices = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&] { return stopRequested_ || state_ == VmState::Running; });
      if (stopRequested_) {
        inSlice_ = false;
        cvIdle_.notify_all();
        return;
      }
      inSlice_ = true;
    }
    const StopReason r = cpu_.run(kSliceInstructions);  // kilitsiz çalışır
    // Publish a race-free snapshot between slices. This lets the UI distinguish
    // a live-but-silent kernel from a stuck interpreter without reading CpuState concurrently.
    const CpuState& snap = cpu_.state();
    const uint64_t snapPc = snap.pc;
    const uint32_t snapInsn = cpu_.lastInstruction();
    if (snapPc == previousPc && snapInsn == previousInsn) ++samePcSlices;
    else { previousPc = snapPc; previousInsn = snapInsn; samePcSlices = 1; }
    runtimePc_.store(snapPc, std::memory_order_release);
    runtimeInstructions_.store(cpu_.instructionsRetired(), std::memory_order_release);
    runtimeLastInstruction_.store(snapInsn, std::memory_order_release);
    runtimeSamePc_.store(snapPc, std::memory_order_release);
    runtimeSamePcSlices_.store(samePcSlices, std::memory_order_release);
    runtimeX0_.store(snap.x[0], std::memory_order_release); runtimeX1_.store(snap.x[1], std::memory_order_release);
    runtimeX2_.store(snap.x[2], std::memory_order_release); runtimeX3_.store(snap.x[3], std::memory_order_release);
    runtimeSctlr_.store(snap.sctlr_el1, std::memory_order_release); runtimeEsr_.store(snap.esr_el1, std::memory_order_release);
    runtimeFar_.store(snap.far_el1, std::memory_order_release);
    runtimeElr_.store(snap.elr_el1, std::memory_order_release); runtimeSpsr_.store(snap.spsr_el1, std::memory_order_release);
    runtimeLastAbortPc_.store(snap.last_abort_pc, std::memory_order_release);
    runtimeLastAbortInsn_.store(snap.last_abort_insn, std::memory_order_release);
    runtimeLastAbortVa_.store(snap.last_abort_va, std::memory_order_release);
    runtimeLastAbortPa_.store(snap.last_abort_pa, std::memory_order_release);
    runtimeLastAbortBusStatus_.store(snap.last_abort_bus_status, std::memory_order_release);
    runtimeLastAbortSize_.store(snap.last_abort_size, std::memory_order_release);
    runtimeLastAbortAccess_.store(snap.last_abort_access, std::memory_order_release);
    runtimeLastAbortTableWalk_.store(snap.last_abort_table_walk, std::memory_order_release);
    runtimeLastAbortTcr_.store(snap.last_abort_tcr_el1, std::memory_order_release);
    runtimeLastAbortTtbr0_.store(snap.last_abort_ttbr0_el1, std::memory_order_release);
    runtimeLastAbortTtbr1_.store(snap.last_abort_ttbr1_el1, std::memory_order_release);
    runtimeLastAbortX8_.store(snap.last_abort_x8, std::memory_order_release);
    runtimeLastAbortWalkRoot_.store(snap.last_abort_walk_root, std::memory_order_release);
    runtimeLastAbortWalkVaPage_.store(snap.last_abort_walk_va_page, std::memory_order_release);
    runtimeLastAbortL1Desc_.store(snap.last_abort_l1_desc, std::memory_order_release);
    runtimeLastAbortL2Desc_.store(snap.last_abort_l2_desc, std::memory_order_release);
    runtimeLastAbortL3Desc_.store(snap.last_abort_l3_desc, std::memory_order_release);
    runtimeLastAbortL3EntryPa_.store(snap.last_abort_l3_entry_pa, std::memory_order_release);
    runtimeLastAbortPteWriterFound_.store(snap.last_abort_pte_writer_found, std::memory_order_release);
    runtimeLastAbortPteWriterPc_.store(snap.last_abort_pte_writer_pc, std::memory_order_release);
    runtimeLastAbortPteWriterVa_.store(snap.last_abort_pte_writer_va, std::memory_order_release);
    runtimeLastAbortPteWriterPa_.store(snap.last_abort_pte_writer_pa, std::memory_order_release);
    runtimeLastAbortPteWriterValue_.store(snap.last_abort_pte_writer_value, std::memory_order_release);
    runtimeLastAbortPteWriterInsn_.store(snap.last_abort_pte_writer_insn, std::memory_order_release);
    runtimeLastAbortPteWriterSize_.store(snap.last_abort_pte_writer_size, std::memory_order_release);
    runtimeLastAbortWalkTtbr1_.store(snap.last_abort_walk_ttbr1, std::memory_order_release);
    runtimeLastAbortTlbHit_.store(snap.last_abort_translation_cache_hit, std::memory_order_release);
    runtimeLastAbortFreshWalkAttempted_.store(snap.last_abort_fresh_walk_attempted, std::memory_order_release);
    runtimeLastAbortFreshWalkOk_.store(snap.last_abort_fresh_walk_ok, std::memory_order_release);
    runtimeLastAbortFreshPa_.store(snap.last_abort_fresh_pa, std::memory_order_release);
    runtimeLastAbortFreshRoot_.store(snap.last_abort_fresh_root, std::memory_order_release);
    runtimeLastAbortFreshL1Desc_.store(snap.last_abort_fresh_l1_desc, std::memory_order_release);
    runtimeLastAbortFreshL2Desc_.store(snap.last_abort_fresh_l2_desc, std::memory_order_release);
    runtimeLastAbortFreshL3Desc_.store(snap.last_abort_fresh_l3_desc, std::memory_order_release);
    runtimeLastAbortFreshFsc_.store(snap.last_abort_fresh_fsc, std::memory_order_release);
    runtimeLastAbortFreshTableWalk_.store(snap.last_abort_fresh_table_walk, std::memory_order_release);
    runtimeLastAbortFreshFaultPa_.store(snap.last_abort_fresh_fault_pa, std::memory_order_release);
    runtimeLastAbortFreshBusStatus_.store(snap.last_abort_fresh_bus_status, std::memory_order_release);
    runtimeCurrentEl_.store(snap.current_el, std::memory_order_release);
    runtimeSlices_.fetch_add(1, std::memory_order_acq_rel);
    rtc_.poll();
    {
      std::lock_guard<std::mutex> lk(mu_);
      inSlice_ = false;
      if (r != StopReason::InstructionLimit) {
        if (!stopRequested_) {  // host zaten stop() ile sonlandırdıysa üzerine yazma
          lastStop_ = cpu_.lastStop();
          state_ = (r == StopReason::Halted) ? VmState::Stopped : VmState::Faulted;
        }
        cvIdle_.notify_all();
        return;
      }
      cvIdle_.notify_all();
    }
  }
}

Status Vm::pause() {
  std::unique_lock<std::mutex> lk(mu_);
  if (state_ != VmState::Running) return Status::InvalidState;
  state_ = VmState::Paused;
  cvIdle_.wait(lk, [&] { return !inSlice_; });
  // Bekleme sırasında guest sonlanmış olabilir (Stopped/Faulted).
  return state_ == VmState::Paused ? Status::Ok : Status::InvalidState;
}

Status Vm::resume() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ != VmState::Paused || syncUsed_) return Status::InvalidState;
    state_ = VmState::Running;
  }
  cv_.notify_all();
  return Status::Ok;
}

Status Vm::stop() {
  {
    std::unique_lock<std::mutex> lk(mu_);
    if (state_ == VmState::Created) return Status::InvalidState;
    if (state_ == VmState::Running || state_ == VmState::Paused) {
      stopRequested_ = true;
      state_ = VmState::Stopped;
      lastStop_ = StopInfo();
      lastStop_.reason = StopReason::StoppedByHost;  // pc bilinmiyor: CPU thread'i çalışıyor olabilir
    }
  }
  cv_.notify_all();
  joinThread();
  return Status::Ok;
}

Status Vm::reset() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ != VmState::Stopped && state_ != VmState::Faulted) return Status::InvalidState;
  }
  joinThread();  // terminal durumda CPU thread'i çıkmıştır / çıkmak üzeredir
  std::lock_guard<std::mutex> lk(mu_);
  bus_.zeroRam();
  uart_.clear();
  rtc_.reset();
  framebuffer_.clear();
  rng_.reset();
  gic_.reset();
  for (auto& device : blockDevices_) device->reset();
  cpu_.reset(entry_, sp_, cfg_.initialEL);
  bootArg0_ = 0;
  loadedInitrdBase_ = 0;
  loadedInitrdSize_ = 0;
  loadedBootDtb_.clear();
  loadedDtbReport_.clear();
  loadedBootArgs_.clear();
  linuxImageInfo_ = Arm64LinuxImageInfo();
  linuxImageLoaded_ = false;  // RAM was cleared; kernel image must be loaded again.
  lastStop_ = StopInfo();
  stopRequested_ = false;
  inSlice_ = false;
  syncUsed_ = false;
  state_ = VmState::Created;
  return Status::Ok;
}

StopReason Vm::runSync(uint64_t maxInstructions) {
  std::lock_guard<std::mutex> lk(mu_);
  if (thread_.joinable()) return StopReason::None;  // async kullanımda yasak (çağıran InvalidState gibi ele almalı)
  if (state_ == VmState::Created) {
    if (linuxImageLoaded_ && bootArg0_ == 0) return StopReason::None;
    cpu_.reset(entry_, sp_, cfg_.initialEL);
    if (linuxImageLoaded_) prepareLinuxArm64EntryState(cpu_.mutableState(), bootArg0_);
    else cpu_.mutableState().x[0] = bootArg0_;
  } else if (!(state_ == VmState::Paused && syncUsed_)) {
    return StopReason::None;
  }
  syncUsed_ = true;
  state_ = VmState::Running;
  const StopReason r = cpu_.run(maxInstructions);
  if (r == StopReason::InstructionLimit) {
    state_ = VmState::Paused;
  } else {
    lastStop_ = cpu_.lastStop();
    state_ = (r == StopReason::Halted) ? VmState::Stopped : VmState::Faulted;
  }
  return r;
}

Status Vm::cpuState(CpuState* out) const {
  if (out == nullptr) return Status::InvalidArgument;
  std::lock_guard<std::mutex> lk(mu_);
  if (state_ == VmState::Running) return Status::InvalidState;
  *out = cpu_.state();
  return Status::Ok;
}

uint64_t Vm::instructionsRetired() const {
  std::lock_guard<std::mutex> lk(mu_);
  return state_ == VmState::Running ? 0 : cpu_.instructionsRetired();
}

}  // namespace ncv
