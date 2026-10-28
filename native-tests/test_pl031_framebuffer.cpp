#include "devices/ncv_pl031_rtc.h"
#include "devices/ncv_framebuffer.h"
#include "test_framework.h"

using namespace ncv;

NT_TEST(pl031_rtc_exposes_counter_control_mask_and_ids) {
  Pl031Rtc rtc; uint64_t v=0;
  CHECK_ST(rtc.read(0,4,&v),Status::Ok); CHECK(v>1000000000ull);
  CHECK_ST(rtc.read(0x0C,4,&v),Status::Ok); CHECK_EQ(v,1u);
  CHECK_ST(rtc.write(0x10,4,1),Status::Ok); CHECK_ST(rtc.read(0x10,4,&v),Status::Ok); CHECK_EQ(v,1u);
  CHECK_ST(rtc.read(0xFE0,4,&v),Status::Ok); CHECK_EQ(v,0x31u);
  CHECK_ST(rtc.read(0x20,4,&v),Status::DeviceError);
}

NT_TEST(simple_framebuffer_stores_guest_pixel_bytes_and_exports_argb) {
  GuestFramebuffer fb;
  CHECK_ST(fb.write(0,4,0x00112233u),Status::Ok); // little endian bytes: 33,22,11,00 => B,G,R,X
  CHECK_EQ(fb.writeOperations(), 1ull);
  CHECK_EQ(fb.nonZeroWriteOperations(), 1ull);
  auto pixels=fb.snapshotArgb(); CHECK_EQ(pixels.size(),static_cast<size_t>(640*480));
  CHECK_EQ(pixels[0],0xFF112233u);
  uint64_t raw=0; CHECK_ST(fb.read(0,4,&raw),Status::Ok); CHECK_EQ(raw,0x00112233u);
  CHECK_ST(fb.write(GuestFramebuffer::kRegionSize,4,1),Status::OutOfBounds);
}
