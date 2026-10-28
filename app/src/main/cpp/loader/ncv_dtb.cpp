#include "ncv_dtb.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <iomanip>
#include <utility>
#include <initializer_list>
#include <string_view>

namespace ncv {
namespace {
constexpr uint32_t kFdtMagic = 0xD00Dfeedu;
constexpr uint32_t kBeginNode = 1, kEndNode = 2, kProp = 3, kEnd = 9;
constexpr size_t kHeaderSize = 40, kReserveSize = 16, kStructOffset = kHeaderSize + kReserveSize;

void putBe32(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>(v >> 24)); b.push_back(static_cast<uint8_t>(v >> 16));
  b.push_back(static_cast<uint8_t>(v >> 8)); b.push_back(static_cast<uint8_t>(v));
}
void setBe32(std::vector<uint8_t>& b, size_t offset, uint32_t v) {
  b[offset] = static_cast<uint8_t>(v >> 24); b[offset + 1] = static_cast<uint8_t>(v >> 16);
  b[offset + 2] = static_cast<uint8_t>(v >> 8); b[offset + 3] = static_cast<uint8_t>(v);
}
void align4(std::vector<uint8_t>& b) { while (b.size() & 3u) b.push_back(0); }

class FdtWriter {
 public:
  void begin(std::string_view name) { token(kBeginNode); bytes(name); align4(structure_); }
  void endNode() { token(kEndNode); }
  void prop(std::string_view name, const std::vector<uint8_t>& value) {
    token(kProp); putBe32(structure_, static_cast<uint32_t>(value.size()));
    putBe32(structure_, nameOffset(name));
    structure_.insert(structure_.end(), value.begin(), value.end()); align4(structure_);
  }
  void u32(std::string_view name, uint32_t v) { std::vector<uint8_t> b; putBe32(b, v); prop(name, b); }
  void cells(std::string_view name, std::initializer_list<uint32_t> vs) {
    std::vector<uint8_t> b; for (uint32_t v : vs) putBe32(b, v); prop(name, b);
  }
  void u64reg(std::string_view name, uint64_t base, uint64_t size) {
    cells(name, {static_cast<uint32_t>(base >> 32), static_cast<uint32_t>(base),
                 static_cast<uint32_t>(size >> 32), static_cast<uint32_t>(size)});
  }
  void string(std::string_view name, std::string_view v) {
    std::vector<uint8_t> b(v.begin(), v.end()); b.push_back(0); prop(name, b);
  }
  void strings(std::string_view name, std::initializer_list<std::string_view> vs) {
    std::vector<uint8_t> b;
    for (auto v : vs) { b.insert(b.end(), v.begin(), v.end()); b.push_back(0); }
    prop(name, b);
  }
  void empty(std::string_view name) { prop(name, {}); }
  std::vector<uint8_t> finish() {
    token(kEnd);
    std::vector<uint8_t> out(kStructOffset, 0);
    const uint32_t structOff = static_cast<uint32_t>(out.size());
    out.insert(out.end(), structure_.begin(), structure_.end());
    const uint32_t stringsOff = static_cast<uint32_t>(out.size());
    out.insert(out.end(), strings_.begin(), strings_.end());
    const uint32_t total = static_cast<uint32_t>(out.size());
    setBe32(out, 0, kFdtMagic); setBe32(out, 4, total); setBe32(out, 8, structOff);
    setBe32(out, 12, stringsOff); setBe32(out, 16, kHeaderSize); setBe32(out, 20, 17);
    setBe32(out, 24, 16); setBe32(out, 28, 0); setBe32(out, 32, static_cast<uint32_t>(strings_.size()));
    setBe32(out, 36, static_cast<uint32_t>(structure_.size()));
    return out;
  }
 private:
  void token(uint32_t v) { putBe32(structure_, v); }
  void bytes(std::string_view v) { structure_.insert(structure_.end(), v.begin(), v.end()); structure_.push_back(0); }
  uint32_t nameOffset(std::string_view name) {
    auto it = nameOffsets_.find(std::string(name)); if (it != nameOffsets_.end()) return it->second;
    const uint32_t off = static_cast<uint32_t>(strings_.size());
    strings_.insert(strings_.end(), name.begin(), name.end()); strings_.push_back(0);
    nameOffsets_.emplace(std::string(name), off); return off;
  }
  std::vector<uint8_t> structure_, strings_;
  std::map<std::string, uint32_t> nameOffsets_;
};

bool validRange(uint64_t base, uint64_t size) { return size != 0 && base <= UINT64_MAX - size; }
}

Status buildLinuxDtb(const LinuxDtbConfig& c, std::vector<uint8_t>* out) {
  if (out == nullptr || !validRange(c.ramBase, c.ramSize) || c.counterFrequency == 0 ||
      c.bootargs.size() > 4096 || c.gicBase > UINT64_MAX - 0x20000ull || c.uartBase > UINT64_MAX - 0x1000ull ||
      c.virtioBlockCount > 15 || c.virtioBlockBase > UINT64_MAX - static_cast<uint64_t>(c.virtioBlockCount) * 0x200ull ||
      c.virtioRngBase > UINT64_MAX - 0x200ull || c.framebufferBase > UINT64_MAX - static_cast<uint64_t>(c.framebufferStride) * c.framebufferHeight ||
      c.framebufferWidth == 0 || c.framebufferHeight == 0 || c.framebufferStride < c.framebufferWidth * 4u ||
      (c.initrdSize != 0 && (!validRange(c.initrdBase, c.initrdSize) || c.initrdBase < c.ramBase ||
       c.initrdBase + c.initrdSize > c.ramBase + c.ramSize))) return Status::InvalidArgument;
  try {
    FdtWriter w;
    w.begin("");
    w.cells("#address-cells", {2}); w.cells("#size-cells", {2});
    w.string("model", "NCV ARM64 virtual platform");
    w.strings("compatible", {"ncv,arm64-virt", "linux,dummy-virt"});

    char stdoutPath[64];
    std::snprintf(stdoutPath, sizeof(stdoutPath), "/pl011@%llx", static_cast<unsigned long long>(c.uartBase));
    w.begin("chosen");
    w.string("bootargs", c.bootargs);
    w.string("stdout-path", stdoutPath);
    if (c.initrdSize != 0) {
      w.cells("linux,initrd-start", {static_cast<uint32_t>(c.initrdBase >> 32), static_cast<uint32_t>(c.initrdBase)});
      w.cells("linux,initrd-end", {static_cast<uint32_t>((c.initrdBase + c.initrdSize) >> 32), static_cast<uint32_t>(c.initrdBase + c.initrdSize)});
    }
    w.endNode();

    w.begin("psci");
    w.strings("compatible", {"arm,psci-1.0", "arm,psci-0.2"});
    w.string("method", "hvc");
    w.endNode();

    char memoryName[48];
    std::snprintf(memoryName, sizeof(memoryName), "memory@%llx", static_cast<unsigned long long>(c.ramBase));
    w.begin(memoryName);
    w.string("device_type", "memory"); w.u64reg("reg", c.ramBase, c.ramSize);
    w.endNode();

    w.begin("cpus"); w.cells("#address-cells", {1}); w.cells("#size-cells", {0});
    w.u32("timebase-frequency", c.counterFrequency);
    w.begin("cpu@0");
    w.string("device_type", "cpu"); w.string("compatible", "arm,arm-v8"); w.cells("reg", {0});
    w.endNode(); w.endNode();

    char gicName[48];
    std::snprintf(gicName, sizeof(gicName), "interrupt-controller@%llx", static_cast<unsigned long long>(c.gicBase));
    w.begin(gicName);
    w.strings("compatible", {"arm,cortex-a15-gic"});
    w.cells("reg", {static_cast<uint32_t>(c.gicBase >> 32), static_cast<uint32_t>(c.gicBase), 0, 0x1000,
                     static_cast<uint32_t>((c.gicBase + 0x10000) >> 32), static_cast<uint32_t>(c.gicBase + 0x10000), 0, 0x1000});
    w.cells("#interrupt-cells", {3}); w.cells("#address-cells", {0}); w.empty("interrupt-controller"); w.cells("phandle", {1});
    w.endNode();

    w.begin("timer"); w.string("compatible", "arm,armv8-timer"); w.cells("interrupt-parent", {1});
    // Standard ARMv8 timer order: secure phys, non-secure phys, virtual, hypervisor phys.
    // NCV currently drives only non-secure physical (PPI 14 / ID 30) and virtual (PPI 11 / ID 27).
    w.cells("interrupts", {1, 13, 4, 1, 14, 4, 1, 11, 4, 1, 10, 4});
    w.strings("interrupt-names", {"secure-phys", "phys", "virt", "hyp-phys"});
    w.endNode();

    w.begin("rtc@90010000");
    w.string("compatible", "arm,pl031"); w.u64reg("reg", 0x09010000ull, 0x1000);
    w.cells("interrupt-parent", {1}); w.cells("interrupts", {0, 2, 4});
    w.endNode();

    w.begin("clk24mhz");
    w.string("compatible", "fixed-clock"); w.cells("#clock-cells", {0});
    w.u32("clock-frequency", 24000000); w.string("clock-output-names", "ncv-uart-clock");
    w.cells("phandle", {2});
    w.endNode();

    char uartName[48];
    std::snprintf(uartName, sizeof(uartName), "pl011@%llx", static_cast<unsigned long long>(c.uartBase));
    w.begin(uartName);
    w.string("compatible", "arm,pl011"); w.u64reg("reg", c.uartBase, 0x1000);
    w.cells("interrupt-parent", {1}); w.cells("interrupts", {0, 1, 4}); // SPI 1 -> GIC ID 33
    w.cells("clocks", {2, 2}); w.strings("clock-names", {"uartclk", "apb_pclk"});
    w.endNode();

    for (uint32_t n = 0; n < c.virtioBlockCount; ++n) {
      const uint64_t base = c.virtioBlockBase + static_cast<uint64_t>(n) * 0x200ull;
      char virtioName[64];
      std::snprintf(virtioName, sizeof(virtioName), "virtio_mmio@%llx", static_cast<unsigned long long>(base));
      w.begin(virtioName);
      w.string("compatible", "virtio,mmio");
      w.u64reg("reg", base, 0x200);
      w.cells("interrupt-parent", {1});
      w.cells("interrupts", {0, 16u + n, 4}); // GIC IDs 48..63 map to SPI indices 16..31
      w.endNode();
    }
    char rngName[64];
    std::snprintf(rngName, sizeof(rngName), "virtio_rng@%llx", static_cast<unsigned long long>(c.virtioRngBase));
    w.begin(rngName); w.string("compatible", "virtio,mmio"); w.u64reg("reg", c.virtioRngBase, 0x200);
    w.cells("interrupt-parent", {1}); w.cells("interrupts", {0, 31, 4}); w.endNode();

    char fbName[64];
    std::snprintf(fbName, sizeof(fbName), "framebuffer@%llx", static_cast<unsigned long long>(c.framebufferBase));
    w.begin(fbName); w.string("compatible", "simple-framebuffer");
    w.u64reg("reg", c.framebufferBase, static_cast<uint64_t>(c.framebufferStride) * c.framebufferHeight);
    w.u32("width", c.framebufferWidth); w.u32("height", c.framebufferHeight); w.u32("stride", c.framebufferStride);
    w.string("format", "x8r8g8b8"); w.string("status", "okay"); w.endNode();

    w.endNode();
    *out = w.finish();
  } catch (...) { return Status::OutOfMemory; }
  return Status::Ok;
}


namespace {
constexpr uint32_t kHybridFdtMagic = 0xD00Dfeedu;
constexpr uint32_t kHybridDtbTableMagic = 0xD7B7AB1Eu;
constexpr uint32_t kHybridBeginNode = 1, kHybridEndNode = 2, kHybridProp = 3,
                   kHybridNop = 4, kHybridEnd = 9;
struct HybridProp { std::string name; std::vector<uint8_t> value; };
struct HybridNode {
  std::string name;
  std::vector<HybridProp> props;
  std::vector<std::unique_ptr<HybridNode>> children;
};
struct HybridReservation { uint64_t address = 0, size = 0; };
struct HybridFdt {
  std::unique_ptr<HybridNode> root;
  std::vector<HybridReservation> reservations;
  uint32_t bootCpu = 0;
};
uint32_t hb32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
uint64_t hb64(const uint8_t* p) { return (static_cast<uint64_t>(hb32(p)) << 32) | hb32(p + 4); }
uint32_t hl32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void hp32(std::vector<uint8_t>& v, uint32_t n) {
  v.push_back(static_cast<uint8_t>(n >> 24)); v.push_back(static_cast<uint8_t>(n >> 16));
  v.push_back(static_cast<uint8_t>(n >> 8)); v.push_back(static_cast<uint8_t>(n));
}
void hp64(std::vector<uint8_t>& v, uint64_t n) { hp32(v, static_cast<uint32_t>(n >> 32)); hp32(v, static_cast<uint32_t>(n)); }
void hAlign4(std::vector<uint8_t>& v) { while (v.size() & 3u) v.push_back(0); }
const HybridProp* hProp(const HybridNode& n, std::string_view name) {
  for (const auto& p : n.props) if (p.name == name) return &p;
  return nullptr;
}
HybridProp* hProp(HybridNode& n, std::string_view name) {
  for (auto& p : n.props) if (p.name == name) return &p;
  return nullptr;
}
void hSetProp(HybridNode& n, std::string name, std::vector<uint8_t> value) {
  if (auto* p = hProp(n, name)) { p->value = std::move(value); return; }
  n.props.push_back({std::move(name), std::move(value)});
}
void hEraseProp(HybridNode& n, std::string_view name) {
  n.props.erase(std::remove_if(n.props.begin(), n.props.end(), [&](const auto& p) { return p.name == name; }), n.props.end());
}
void hSetString(HybridNode& n, const std::string& key, std::string_view value) {
  std::vector<uint8_t> b(value.begin(), value.end()); b.push_back(0); hSetProp(n, key, std::move(b));
}
void hSetStrings(HybridNode& n, const std::string& key, std::initializer_list<std::string_view> values) {
  std::vector<uint8_t> b;
  for (auto v : values) { b.insert(b.end(), v.begin(), v.end()); b.push_back(0); }
  hSetProp(n, key, std::move(b));
}
void hSetEmpty(HybridNode& n, const std::string& key) { hSetProp(n, key, {}); }
void hSetU32(HybridNode& n, const std::string& key, uint32_t value) {
  std::vector<uint8_t> b; hp32(b, value); hSetProp(n, key, std::move(b));
}
std::string hString(const HybridNode& n, std::string_view key) {
  const auto* p = hProp(n, key); if (!p || p->value.empty()) return {};
  size_t len = 0; while (len < p->value.size() && p->value[len] != 0) ++len;
  return std::string(reinterpret_cast<const char*>(p->value.data()), len);
}
uint32_t hU32(const HybridNode& n, std::string_view key, uint32_t fallback) {
  const auto* p = hProp(n, key); return p && p->value.size() >= 4 ? hb32(p->value.data()) : fallback;
}
uint64_t hPhandle(const HybridNode& n) {
  const auto* p = hProp(n, "phandle"); if (!p) p = hProp(n, "linux,phandle");
  return p && p->value.size() >= 4 ? hb32(p->value.data()) : 0;
}
bool hHasCompatible(const HybridNode& n, std::string_view needle) {
  const auto* p = hProp(n, "compatible"); if (!p) return false;
  size_t at = 0;
  while (at < p->value.size()) {
    size_t end = at; while (end < p->value.size() && p->value[end] != 0) ++end;
    if (std::string_view(reinterpret_cast<const char*>(p->value.data() + at), end - at).find(needle) != std::string_view::npos) return true;
    at = end + 1;
  }
  return false;
}
bool hIsMemory(const HybridNode& n) {
  return hString(n, "device_type") == "memory" || n.name == "memory" || n.name.rfind("memory@", 0) == 0;
}
HybridNode* hChild(HybridNode& n, std::string_view name) {
  for (auto& c : n.children) if (c->name == name) return c.get();
  return nullptr;
}
void hRemoveChildrenByName(HybridNode& n, const std::string& name) {
  n.children.erase(std::remove_if(n.children.begin(), n.children.end(), [&](const auto& c) { return c->name == name; }), n.children.end());
}
std::vector<uint8_t> hEncodeReg(uint64_t base, uint64_t size, unsigned ac, unsigned sc) {
  if (ac > 2 || sc > 2 || ac == 0 || sc == 0) return {};
  std::vector<uint8_t> b;
  if (ac == 2) hp64(b, base); else hp32(b, static_cast<uint32_t>(base));
  if (sc == 2) hp64(b, size); else hp32(b, static_cast<uint32_t>(size));
  return b;
}
std::vector<uint8_t> hEncodeRegs(const std::vector<std::pair<uint64_t,uint64_t>>& regs, unsigned ac, unsigned sc) {
  std::vector<uint8_t> b;
  for (const auto& r : regs) {
    auto one = hEncodeReg(r.first, r.second, ac, sc);
    b.insert(b.end(), one.begin(), one.end());
  }
  return b;
}
std::vector<uint8_t> hEncodeCells(std::initializer_list<uint32_t> cells) {
  std::vector<uint8_t> b; for (auto c : cells) hp32(b, c); return b;
}
std::vector<uint8_t> hEncodeAddress(uint64_t value, unsigned cells) {
  std::vector<uint8_t> b;
  if (cells == 1) hp32(b, static_cast<uint32_t>(value));
  else if (cells == 2) hp64(b, value);
  return b;
}
void hSetReg(HybridNode& n, uint64_t base, uint64_t size, unsigned ac, unsigned sc) {
  hSetProp(n, "reg", hEncodeReg(base, size, ac, sc));
}
void hSetRegPairs(HybridNode& n, const std::vector<std::pair<uint64_t,uint64_t>>& regs, unsigned ac, unsigned sc) {
  hSetProp(n, "reg", hEncodeRegs(regs, ac, sc));
}
void hSetStatusOkay(HybridNode& n) { hSetString(n, "status", "okay"); }

bool hSelectDtb(const uint8_t* src, uint64_t size, const uint8_t** out, uint64_t* outSize, std::string* mode) {
  if (!src || !out || !outSize || size < 8) return false;
  auto validFdtAt = [&](uint64_t off, const uint8_t** ptr, uint64_t* len) {
    if (off > size || size - off < 40 || hb32(src + off) != kHybridFdtMagic) return false;
    const uint32_t total = hb32(src + off + 4);
    if (total < 40 || total > size - off) return false;
    *ptr = src + off; *len = total; return true;
  };
  if (validFdtAt(0, out, outSize)) { if (mode) *mode = "raw FDT"; return true; }
  if (size >= 32 && hl32(src) == kHybridDtbTableMagic) {
    const uint32_t total = hl32(src + 4), header = hl32(src + 8), entrySize = hl32(src + 12), count = hl32(src + 16), entries = hl32(src + 20);
    if (total > size || header < 32 || header > total || entrySize < 32 || count == 0 || entries > total || static_cast<uint64_t>(count) * entrySize > total - entries) return false;
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* e = src + entries + static_cast<uint64_t>(i) * entrySize;
      const uint32_t dtSize = hl32(e), dtOffset = hl32(e + 4);
      if (dtSize == 0 || dtOffset > total || dtSize > total - dtOffset) continue;
      const uint8_t* fdt = nullptr; uint64_t fdtSize = 0;
      if (validFdtAt(dtOffset, &fdt, &fdtSize) && fdtSize <= dtSize) {
        *out = fdt; *outSize = fdtSize; if (mode) *mode = "Android DT table first valid entry"; return true;
      }
    }
    return false;
  }
  // Some legacy images concatenate DTBs without a table header. Selecting the first valid
  // FDT is deterministic, but can select the wrong board variant in a multi-DTB image.
  for (uint64_t off = 4; off + 40 <= size; off += 4) {
    if (validFdtAt(off, out, outSize)) { if (mode) *mode = "first FDT in concatenated DTB payload"; return true; }
  }
  return false;
}

bool hParseTree(const uint8_t* data, uint64_t size, HybridFdt* parsed) {
  if (!data || !parsed || size < 40 || hb32(data) != kHybridFdtMagic) return false;
  const uint32_t total = hb32(data + 4), offStruct = hb32(data + 8), offStrings = hb32(data + 12), offReserve = hb32(data + 16);
  const uint32_t bootCpu = hb32(data + 28), sizeStrings = hb32(data + 32), sizeStruct = hb32(data + 36);
  if (total > size || total < 40 || offStruct > total || sizeStruct > total - offStruct ||
      offStrings > total || sizeStrings > total - offStrings || offReserve > total) return false;
  HybridFdt tmp; tmp.bootCpu = bootCpu;
  uint64_t r = offReserve;
  bool reserveEnd = false;
  while (r <= total && total - r >= 16) {
    const uint64_t address = hb64(data + r), len = hb64(data + r + 8); r += 16;
    if (address == 0 && len == 0) { reserveEnd = true; break; }
    tmp.reservations.push_back({address, len});
  }
  if (!reserveEnd) return false;
  std::vector<HybridNode*> stack;
  uint64_t at = offStruct, endStruct = static_cast<uint64_t>(offStruct) + sizeStruct;
  bool ended = false;
  while (at + 4 <= endStruct) {
    const uint32_t token = hb32(data + at); at += 4;
    if (token == kHybridBeginNode) {
      uint64_t end = at; while (end < endStruct && data[end] != 0) ++end;
      if (end == endStruct) return false;
      auto node = std::make_unique<HybridNode>();
      node->name.assign(reinterpret_cast<const char*>(data + at), static_cast<size_t>(end - at));
      at = (end + 1 + 3) & ~3ull;
      HybridNode* ptr = node.get();
      if (stack.empty()) {
        if (tmp.root) return false;
        tmp.root = std::move(node);
      } else stack.back()->children.push_back(std::move(node));
      stack.push_back(ptr);
    } else if (token == kHybridEndNode) {
      if (stack.empty()) return false;
      stack.pop_back();
    } else if (token == kHybridProp) {
      if (stack.empty() || at + 8 > endStruct) return false;
      const uint32_t len = hb32(data + at), nameOff = hb32(data + at + 4); at += 8;
      if (at + len > endStruct || nameOff >= sizeStrings) return false;
      const char* nm = reinterpret_cast<const char*>(data + offStrings + nameOff);
      const void* terminator = std::memchr(nm, 0, sizeStrings - nameOff);
      if (!terminator) return false;
      HybridProp p; p.name.assign(nm, static_cast<const char*>(terminator) - nm);
      p.value.assign(data + at, data + at + len); stack.back()->props.push_back(std::move(p));
      at = (at + len + 3) & ~3ull;
    } else if (token == kHybridNop) {
      continue;
    } else if (token == kHybridEnd) { ended = true; break; }
    else return false;
  }
  if (!ended || !stack.empty() || !tmp.root || !tmp.root->name.empty()) return false;
  *parsed = std::move(tmp); return true;
}

void hEmitNode(const HybridNode& node, std::vector<uint8_t>& structure,
               std::vector<uint8_t>& strings, std::map<std::string,uint32_t>& offsets) {
  hp32(structure, kHybridBeginNode);
  structure.insert(structure.end(), node.name.begin(), node.name.end()); structure.push_back(0); hAlign4(structure);
  for (const auto& p : node.props) {
    uint32_t nameOff = 0;
    const auto it = offsets.find(p.name);
    if (it != offsets.end()) nameOff = it->second;
    else { nameOff = static_cast<uint32_t>(strings.size()); offsets.emplace(p.name, nameOff); strings.insert(strings.end(), p.name.begin(), p.name.end()); strings.push_back(0); }
    hp32(structure, kHybridProp); hp32(structure, static_cast<uint32_t>(p.value.size())); hp32(structure, nameOff);
    structure.insert(structure.end(), p.value.begin(), p.value.end()); hAlign4(structure);
  }
  for (const auto& c : node.children) hEmitNode(*c, structure, strings, offsets);
  hp32(structure, kHybridEndNode);
}
std::vector<uint8_t> hSerializeTree(const HybridFdt& fdt) {
  std::vector<uint8_t> structure, strings; std::map<std::string,uint32_t> offsets;
  hEmitNode(*fdt.root, structure, strings, offsets); hp32(structure, kHybridEnd);
  std::vector<uint8_t> out(40, 0);
  const uint32_t offReserve = static_cast<uint32_t>(out.size());
  for (const auto& r : fdt.reservations) { hp64(out, r.address); hp64(out, r.size); }
  hp64(out, 0); hp64(out, 0);
  while (out.size() & 7u) out.push_back(0);
  const uint32_t offStruct = static_cast<uint32_t>(out.size());
  out.insert(out.end(), structure.begin(), structure.end());
  const uint32_t offStrings = static_cast<uint32_t>(out.size());
  out.insert(out.end(), strings.begin(), strings.end());
  const uint32_t total = static_cast<uint32_t>(out.size());
  auto set32 = [&](size_t o, uint32_t v) { out[o] = static_cast<uint8_t>(v >> 24); out[o+1] = static_cast<uint8_t>(v >> 16); out[o+2] = static_cast<uint8_t>(v >> 8); out[o+3] = static_cast<uint8_t>(v); };
  set32(0, kHybridFdtMagic); set32(4, total); set32(8, offStruct); set32(12, offStrings);
  set32(16, offReserve); set32(20, 17); set32(24, 16); set32(28, fdt.bootCpu);
  set32(32, static_cast<uint32_t>(strings.size())); set32(36, static_cast<uint32_t>(structure.size()));
  return out;
}

void hFindMemory(HybridNode& node, uint64_t base, uint64_t size, unsigned ac, unsigned sc, bool* found) {
  for (auto it = node.children.begin(); it != node.children.end();) {
    if (hIsMemory(**it)) {
      if (!*found) {
        hSetString(**it, "device_type", "memory");
        hSetProp(**it, "reg", hEncodeReg(base, size, ac, sc)); hSetStatusOkay(**it); *found = true; ++it;
      } else it = node.children.erase(it);
    } else { hFindMemory(**it, base, size, ac, sc, found); ++it; }
  }
}
struct HNodeLoc { HybridNode* parent = nullptr; size_t index = 0; HybridNode* node = nullptr; };
bool hFindGic(HybridNode& node, HybridNode* parent, HNodeLoc* out) {
  for (size_t i = 0; i < node.children.size(); ++i) {
    HybridNode* c = node.children[i].get();
    if (hProp(*c, "interrupt-controller") && (hHasCompatible(*c, "gic") || hHasCompatible(*c, "GIC"))) {
      if (out) *out = {&node, i, c};
      return true;
    }
    if (hFindGic(*c, &node, out)) return true;
  }
  (void)parent; return false;
}
uint32_t hMaxPhandle(const HybridNode& node) {
  uint32_t maximum = static_cast<uint32_t>(hPhandle(node));
  for (const auto& c : node.children) maximum = std::max(maximum, hMaxPhandle(*c));
  return maximum;
}
void hSetPhandle(HybridNode& n, uint32_t v) { hSetU32(n, "phandle", v); hSetU32(n, "linux,phandle", v); }
void hSetCellsProp(HybridNode& n, const std::string& key, std::initializer_list<uint32_t> cells);
void hFindTimer(HybridNode& n, uint32_t gicPhandle, uint32_t frequency, bool* done) {
  if (!*done && hHasCompatible(n, "arm,armv8-timer")) {
    hSetCellsProp(n, "interrupts", {1,13,4, 1,14,4, 1,11,4, 1,10,4});
    hSetU32(n, "interrupt-parent", gicPhandle);
    hSetStrings(n, "interrupt-names", {"secure-phys", "phys", "virt", "hyp-phys"}); *done = true;
  }
  for (auto& c : n.children) hFindTimer(*c, gicPhandle, frequency, done);
  (void)frequency;
}
void hSetCellsProp(HybridNode& n, const std::string& key, std::initializer_list<uint32_t> cells) {
  hSetProp(n, key, hEncodeCells(cells));
}
void hDisableSecondaryCpus(HybridNode& root) {
  HybridNode* cpus = hChild(root, "cpus"); if (!cpus) return;
  for (auto& c : cpus->children) {
    if (c->name.rfind("cpu@", 0) == 0 && c->name != "cpu@0") hSetString(*c, "status", "disabled");
    else if (c->name == "cpu@0") { hSetStatusOkay(*c); hSetStrings(*c, "compatible", {"arm,arm-v8"}); hSetString(*c, "enable-method", "psci"); }
  }
  hSetCellsProp(*cpus, "#address-cells", {1}); hSetCellsProp(*cpus, "#size-cells", {0});
}
void hStripPhysicalReservedMemory(HybridNode& root, unsigned addressCells, unsigned sizeCells) {
  // The input DTB reserves memory for the original phone SoC (Mali, modem, IOMMU,
  // secure-world buffers, etc.). Those physical addresses do not describe NCV RAM.
  // Drop these allocations and stale memory-region references rather than reserving
  // arbitrary chunks of the virtual guest's RAM. This does not emulate those devices.
  HybridNode* reserved = hChild(root, "reserved-memory");
  if (reserved) {
    reserved->children.clear();
    hSetCellsProp(*reserved, "#address-cells", {addressCells});
    hSetCellsProp(*reserved, "#size-cells", {sizeCells});
    hSetEmpty(*reserved, "ranges");
  }
  auto walk = [&](auto&& self, HybridNode& node) -> void {
    hEraseProp(node, "memory-region");
    hEraseProp(node, "memory-region-names");
    for (auto& child : node.children) self(self, *child);
  };
  walk(walk, root);
}
void hRemoveNodeName(HybridNode& root, const std::string& name) { hRemoveChildrenByName(root, name); }
void hAddVirtualDevices(HybridNode& root, const LinuxDtbConfig& c, uint32_t gicPhandle, unsigned ac, unsigned sc, uint32_t clockPhandle) {
  auto clock = std::make_unique<HybridNode>(); clock->name = "ncv-clk24mhz";
  hSetStrings(*clock, "compatible", {"fixed-clock"}); hSetCellsProp(*clock, "#clock-cells", {0});
  hSetU32(*clock, "clock-frequency", 24000000); hSetString(*clock, "clock-output-names", "ncv-uart-clock"); hSetPhandle(*clock, clockPhandle);
  hRemoveNodeName(root, clock->name); root.children.push_back(std::move(clock));

  char uartName[64];
  std::snprintf(uartName, sizeof(uartName), "pl011@%llx", static_cast<unsigned long long>(c.uartBase));
  auto uart = std::make_unique<HybridNode>(); uart->name = uartName;
  hSetStrings(*uart, "compatible", {"arm,pl011", "arm,primecell"}); hSetReg(*uart, c.uartBase, 0x1000, ac, sc);
  hSetU32(*uart, "interrupt-parent", gicPhandle); hSetCellsProp(*uart, "interrupts", {0,1,4});
  hSetCellsProp(*uart, "clocks", {clockPhandle, clockPhandle}); hSetStrings(*uart, "clock-names", {"uartclk", "apb_pclk"}); hSetStatusOkay(*uart);
  hRemoveNodeName(root, uart->name); root.children.push_back(std::move(uart));

  auto rtc = std::make_unique<HybridNode>(); rtc->name = "rtc@90010000";
  hSetStrings(*rtc, "compatible", {"arm,pl031"}); hSetReg(*rtc, 0x09010000ull, 0x1000, ac, sc);
  hSetU32(*rtc, "interrupt-parent", gicPhandle); hSetCellsProp(*rtc, "interrupts", {0,2,4}); hSetStatusOkay(*rtc);
  hRemoveNodeName(root, rtc->name); root.children.push_back(std::move(rtc));

  for (uint32_t i = 0; i < c.virtioBlockCount; ++i) {
    const uint64_t base = c.virtioBlockBase + static_cast<uint64_t>(i) * 0x200ull;
    char name[64]; std::snprintf(name, sizeof(name), "virtio_mmio@%llx", static_cast<unsigned long long>(base));
    auto node = std::make_unique<HybridNode>(); node->name = name; hSetString(*node, "compatible", "virtio,mmio");
    hSetReg(*node, base, 0x200, ac, sc); hSetU32(*node, "interrupt-parent", gicPhandle);
    hSetCellsProp(*node, "interrupts", {0, 16u+i, 4}); hSetStatusOkay(*node); hRemoveNodeName(root, node->name); root.children.push_back(std::move(node));
  }
  {
    auto node = std::make_unique<HybridNode>(); node->name = "virtio_rng@a002000"; hSetString(*node, "compatible", "virtio,mmio");
    hSetReg(*node, c.virtioRngBase, 0x200, ac, sc); hSetU32(*node, "interrupt-parent", gicPhandle);
    hSetCellsProp(*node, "interrupts", {0,31,4}); hSetStatusOkay(*node); hRemoveNodeName(root, node->name); root.children.push_back(std::move(node));
  }
  {
    char fbName[64]; std::snprintf(fbName, sizeof(fbName), "framebuffer@%llx", static_cast<unsigned long long>(c.framebufferBase));
    auto node = std::make_unique<HybridNode>(); node->name = fbName; hSetStrings(*node, "compatible", {"simple-framebuffer"});
    hSetReg(*node, c.framebufferBase, static_cast<uint64_t>(c.framebufferStride)*c.framebufferHeight, ac, sc);
    hSetU32(*node, "width", c.framebufferWidth); hSetU32(*node, "height", c.framebufferHeight); hSetU32(*node, "stride", c.framebufferStride);
    hSetString(*node, "format", "x8r8g8b8"); hSetStatusOkay(*node); hRemoveNodeName(root, node->name); root.children.push_back(std::move(node));
  }
}
} // namespace

Status buildHybridAndroidDtb(const uint8_t* source, uint64_t sourceSize,
                             const LinuxDtbConfig& config, std::vector<uint8_t>* out,
                             std::string* report) {
  if (!source || !out || sourceSize == 0 || config.ramSize == 0 || config.bootargs.size() > 4096 ||
      config.virtioBlockCount > 15 || config.ramBase > UINT64_MAX - config.ramSize) return Status::InvalidArgument;
  const uint8_t* fdtData = nullptr; uint64_t fdtSize = 0; std::string sourceMode;
  if (!hSelectDtb(source, sourceSize, &fdtData, &fdtSize, &sourceMode)) return Status::InvalidArgument;
  HybridFdt tree;
  if (!hParseTree(fdtData, fdtSize, &tree)) return Status::InvalidArgument;
  try {
    const uint64_t ramEnd = config.ramBase + config.ramSize;
    tree.reservations.erase(std::remove_if(tree.reservations.begin(), tree.reservations.end(),
        [&](const HybridReservation& r) {
          // Keep reservations wholly outside guest RAM. Any overlap is stale physical-board
          // metadata and must not silently carve the NCV RAM map into unusable pieces.
          if (r.size == 0 || r.address > UINT64_MAX - r.size) return true;
          const uint64_t end = r.address + r.size;
          return r.address < ramEnd && end > config.ramBase;
        }), tree.reservations.end());
    if (config.bootScratchReserveSize != 0) {
      if (config.bootScratchReserveBase > UINT64_MAX - config.bootScratchReserveSize ||
          config.bootScratchReserveBase < config.ramBase ||
          config.bootScratchReserveBase + config.bootScratchReserveSize > ramEnd)
        return Status::InvalidArgument;
      tree.reservations.push_back({config.bootScratchReserveBase, config.bootScratchReserveSize});
    }
    HybridNode& root = *tree.root;
    unsigned ac = hU32(root, "#address-cells", 2), sc = hU32(root, "#size-cells", 2);
    if (ac == 0 || ac > 2 || sc == 0 || sc > 2) { ac = 2; sc = 2; hSetCellsProp(root, "#address-cells", {2}); hSetCellsProp(root, "#size-cells", {2}); }

    bool memoryFound = false; hFindMemory(root, config.ramBase, config.ramSize, ac, sc, &memoryFound);
    if (!memoryFound) {
      auto memory = std::make_unique<HybridNode>(); char name[48];
      std::snprintf(name, sizeof(name), "memory@%llx", static_cast<unsigned long long>(config.ramBase)); memory->name = name;
      hSetString(*memory, "device_type", "memory"); hSetReg(*memory, config.ramBase, config.ramSize, ac, sc); root.children.push_back(std::move(memory));
    }

    HybridNode* chosen = hChild(root, "chosen");
    if (!chosen) { auto node = std::make_unique<HybridNode>(); node->name = "chosen"; chosen = node.get(); root.children.push_back(std::move(node)); }
    hSetString(*chosen, "bootargs", config.bootargs);
    char stdoutPath[96];
    std::snprintf(stdoutPath, sizeof(stdoutPath), "/pl011@%llx:115200n8", static_cast<unsigned long long>(config.uartBase));
    hSetString(*chosen, "stdout-path", stdoutPath);
    if (config.initrdSize) {
      hSetProp(*chosen, "linux,initrd-start", hEncodeAddress(config.initrdBase, ac));
      hSetProp(*chosen, "linux,initrd-end", hEncodeAddress(config.initrdBase + config.initrdSize, ac));
    } else { hEraseProp(*chosen, "linux,initrd-start"); hEraseProp(*chosen, "linux,initrd-end"); }

    HNodeLoc loc;
    bool gicWasFound = hFindGic(root, nullptr, &loc);
    uint32_t gicPhandle = 0;
    std::unique_ptr<HybridNode> gicNode;
    if (gicWasFound && loc.parent && loc.node) {
      gicPhandle = static_cast<uint32_t>(hPhandle(*loc.node));
      gicNode = std::move(loc.parent->children[loc.index]);
      loc.parent->children.erase(loc.parent->children.begin() + static_cast<std::ptrdiff_t>(loc.index));
    } else {
      gicNode = std::make_unique<HybridNode>();
    }
    if (gicPhandle == 0) gicPhandle = hMaxPhandle(root) + 1;
    if (gicPhandle == 0) gicPhandle = 1;
    gicNode->name = "interrupt-controller@8000000";
    hSetStrings(*gicNode, "compatible", {"arm,cortex-a15-gic"});
    hSetRegPairs(*gicNode, {{config.gicBase,0x1000},{config.gicBase+0x10000,0x1000}}, ac, sc);
    hSetCellsProp(*gicNode, "#interrupt-cells", {3}); hSetCellsProp(*gicNode, "#address-cells", {0});
    hSetEmpty(*gicNode, "interrupt-controller"); hSetPhandle(*gicNode, gicPhandle); hSetStatusOkay(*gicNode);
    hEraseProp(*gicNode, "redistributor-stride"); hEraseProp(*gicNode, "#redistributor-regions");
    hRemoveNodeName(root, gicNode->name); root.children.push_back(std::move(gicNode));
    hSetU32(root, "interrupt-parent", gicPhandle);

    bool timerFound = false; hFindTimer(root, gicPhandle, config.counterFrequency, &timerFound);
    if (!timerFound) {
      auto timer = std::make_unique<HybridNode>(); timer->name = "timer";
      hSetStrings(*timer, "compatible", {"arm,armv8-timer"}); hSetU32(*timer, "interrupt-parent", gicPhandle);
      hSetCellsProp(*timer, "interrupts", {1,13,4, 1,14,4, 1,11,4, 1,10,4});
      hSetStrings(*timer, "interrupt-names", {"secure-phys", "phys", "virt", "hyp-phys"}); root.children.push_back(std::move(timer));
    }
    hDisableSecondaryCpus(root);
    if (HybridNode* cpus = hChild(root, "cpus")) hSetU32(*cpus, "timebase-frequency", config.counterFrequency);
    hStripPhysicalReservedMemory(root, ac, sc);
    hSetString(root, "model", "NCV adapted virtual ARM64 board (source vendor DTB)");
    HybridNode* psci = hChild(root, "psci");
    if (!psci) { auto node = std::make_unique<HybridNode>(); node->name = "psci"; psci = node.get(); root.children.push_back(std::move(node)); }
    hSetStrings(*psci, "compatible", {"arm,psci-1.0", "arm,psci-0.2"}); hSetString(*psci, "method", "hvc");

    uint32_t clockPhandle = hMaxPhandle(root) + 1; if (clockPhandle == 0) clockPhandle = 1;
    hAddVirtualDevices(root, config, gicPhandle, ac, sc, clockPhandle);
    *out = hSerializeTree(tree);
    if (report) {
      std::ostringstream os; os << "source=" << sourceMode << "; memory=0x" << std::hex << config.ramBase
         << "+0x" << config.ramSize << "; gic=NCV-GICv2@0x" << config.gicBase
         << "; uart=PL011@0x" << config.uartBase << "; virtio-blocks=" << std::dec << config.virtioBlockCount
         << "; virtio-rng=added; RTC=PL031@0x90010000; secondary-cpus=disabled; stale physical reserved-memory/reserve-map entries dropped";
      if (config.bootScratchReserveSize != 0)
        os << std::hex << "; boot-stack-scratch-reserved=0x" << config.bootScratchReserveBase << "+0x" << config.bootScratchReserveSize;
      os << "; vendor-specific nodes retained as probe targets";
      *report = os.str();
    }
  } catch (const std::bad_alloc&) { return Status::OutOfMemory; }
  catch (...) { return Status::OutOfMemory; }
  return Status::Ok;
}


} // namespace ncv
