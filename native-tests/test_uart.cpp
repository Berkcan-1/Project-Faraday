#include <string>

#include "devices/ncv_uart.h"
#include "test_framework.h"

using namespace ncv;

NT_TEST(uart_tx_collects_output) {
  Pl011Uart u;
  CHECK_ST(u.write(0x000, 1, 'H'), Status::Ok);
  CHECK_ST(u.write(0x000, 4, 'i' | 0xFF00), Status::Ok);  // yalnızca bit[7:0] veridir
  CHECK(u.peekOutput() == "Hi");
  CHECK(u.takeOutput() == "Hi");
  CHECK(u.peekOutput().empty());
}

NT_TEST(uart_flags_and_rx) {
  Pl011Uart u;
  uint64_t fr = 0;
  CHECK_ST(u.read(0x018, 4, &fr), Status::Ok);
  CHECK((fr & (1u << 7)) != 0);   // TXFE
  CHECK((fr & (1u << 4)) != 0);   // RXFE (RX boş)
  CHECK((fr & (1u << 6)) == 0);   // RXFF değil
  CHECK(u.pushRx('x'));
  u.read(0x018, 4, &fr);
  CHECK((fr & (1u << 4)) == 0);
  uint64_t dr = 0;
  CHECK_ST(u.read(0x000, 4, &dr), Status::Ok);
  CHECK_EQ(dr, (uint64_t)'x');
  u.read(0x018, 4, &fr);
  CHECK((fr & (1u << 4)) != 0);
  for (size_t i = 0; i < Pl011Uart::kRxFifoDepth; i++) CHECK(u.pushRx((uint8_t)i));
  CHECK(!u.pushRx(0xFF));         // dolu FIFO reddeder
  u.read(0x018, 4, &fr);
  CHECK((fr & (1u << 6)) != 0);   // RXFF
}

NT_TEST(uart_unimplemented_registers_fail_loudly) {
  Pl011Uart u;
  uint64_t v = 0;
  CHECK_ST(u.read(0x100, 4, &v), Status::DeviceError);
  CHECK_ST(u.write(0x100, 4, 1), Status::DeviceError);
  CHECK_ST(u.read(0x044, 4, &v), Status::DeviceError);   // ICR salt-yazılır
  CHECK_ST(u.read(0x001, 1, &v), Status::DeviceError);   // hizasız
  CHECK_ST(u.read(0x000, 8, &v), Status::DeviceError);   // 8 baytlık erişim yok
}

NT_TEST(uart_stored_registers_and_clear) {
  Pl011Uart u;
  CHECK_ST(u.write(0x030, 4, 0x301), Status::Ok);
  uint64_t v = 0;
  CHECK_ST(u.read(0x030, 4, &v), Status::Ok);
  CHECK_EQ(v, 0x301ull);
  u.write(0x000, 1, 'a');
  u.clear();
  u.read(0x030, 4, &v);
  CHECK_EQ(v, 0ull);
  CHECK(u.peekOutput().empty());
}

NT_TEST(uart_tx_buffer_is_bounded) {
  Pl011Uart u(4);
  for (int i = 0; i < 10; i++) u.write(0x000, 1, 'a' + i);
  CHECK(u.peekOutput() == "abcd");
  CHECK_EQ(u.droppedTxBytes(), 6ull);
}

NT_TEST(uart_tx_sink_called) {
  Pl011Uart u;
  std::string seen;
  u.setTxSink([&](uint8_t b) { seen.push_back((char)b); });
  u.write(0x000, 1, 'o');
  u.write(0x000, 1, 'k');
  CHECK(seen == "ok");
}
