// NCV - ARM semantiğiyle saf-tamsayı soft-float (host FP donanımına/fenv'e BAĞIMLI DEĞİL).
// Biçimler: IEEE half/single/double. Yuvarlama (FPCR.RMode), FZ, DN, NaN işleme ve FPSR bayrakları ARM ARM'daki
// FPRound/FPProcessNaNs yöntemini izler. Tuzak (trap) etkinleştirme bitleri desteklenmez. AHP (alternatif half) yok.
#pragma once
#include <cstdint>

namespace ncv {
namespace sf {

enum Fmt { H = 0, S = 1, D = 2 };
enum Rnd { RN = 0, RP = 1, RM = 2, RZ = 3, RA = 4 /* ties-away: FCVTA*, FRINTA */ };

constexpr uint32_t IOC = 1u << 0, DZC = 1u << 1, OFC = 1u << 2, UFC = 1u << 3, IXC = 1u << 4, IDC = 1u << 7;

struct Env {
  Rnd rmode = RN;
  bool fz = false;
  bool dn = false;
  uint32_t flags = 0;  // FPSR bit konumlarında birikir
};

uint64_t add(Fmt f, uint64_t a, uint64_t b, Env& e);
uint64_t sub(Fmt f, uint64_t a, uint64_t b, Env& e);
uint64_t mul(Fmt f, uint64_t a, uint64_t b, Env& e);
uint64_t div(Fmt f, uint64_t a, uint64_t b, Env& e);
uint64_t sqrt(Fmt f, uint64_t a, Env& e);
uint64_t mulAdd(Fmt f, uint64_t a, uint64_t b, uint64_t c, Env& e);       // a*b + c (tek yuvarlama)
uint64_t mulX(Fmt f, uint64_t a, uint64_t b, Env& e);                          // FMULX: 0*inf = +-2.0
uint64_t recipStep(Fmt f, uint64_t a, uint64_t b, Env& e);                     // FRECPS: 2 - a*b
uint64_t rsqrtStep(Fmt f, uint64_t a, uint64_t b, Env& e);                     // FRSQRTS: (3 - a*b)/2
uint64_t recipEstimate(Fmt f, uint64_t a, Env& e);                             // FRECPE (S/D)
uint64_t rsqrtEstimate(Fmt f, uint64_t a, Env& e);                             // FRSQRTE (S/D)
uint64_t convertOdd(Fmt from, Fmt to, uint64_t a, Env& e);                     // FCVTXN: round-to-odd
uint64_t minMax(Fmt f, uint64_t a, uint64_t b, Env& e, bool isMin, bool num);  // num: FMAXNM/FMINNM
unsigned compare(Fmt f, uint64_t a, uint64_t b, Env& e, bool signaling);     // NZCV (N<<3|Z<<2|C<<1|V)
uint64_t convert(Fmt from, Fmt to, uint64_t a, Env& e);                      // FCVT
uint64_t roundInt(Fmt f, uint64_t a, Rnd rm, bool exact, Env& e);            // FRINT*
// FP -> tamsayı (width: 32/64 bit sonuç, fbits: sabit-nokta kesir bitleri). Sonuç düşük width bitte, kalanı 0.
uint64_t toInt(Fmt f, uint64_t a, bool isSigned, unsigned width, unsigned fbits, Rnd rm, Env& e);
// tamsayı -> FP (srcWidth: 32/64)
uint64_t fromInt(Fmt f, uint64_t v, bool isSigned, unsigned srcWidth, unsigned fbits, Env& e);

}  // namespace sf
}  // namespace ncv
