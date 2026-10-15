// Host unit tests for the UART protocol, run in CI:
//   g++ -std=c++17 -Wall -Wextra -Werror -I components tests/host/test_protocol.cpp -o test_protocol
//   ./test_protocol
// The frames and status blocks are real captures from a Dual 200S (MCU 2.0.02).

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "levoit_humidifier/protocol.h"

using namespace esphome::levoit_humidifier::protocol;
using Bytes = std::vector<uint8_t>;

static int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                      \
    }                                                                  \
  } while (0)

static Bytes parse_all(FrameParser &p, const Bytes &bytes, FrameParser::Result &last) {
  last = FrameParser::Result::NONE;
  for (uint8_t b : bytes) {
    auto r = p.feed(b);
    if (r != FrameParser::Result::NONE)
      last = r;
  }
  return p.raw();
}

static void test_encode() {
  // captured: the ESP's status query, ">>> A5 22 06 04 00 2D 01 C0 40 00"
  CHECK(encode_frame(0x22, 0x06, 0x40C0, {}) == Bytes({0xA5, 0x22, 0x06, 0x04, 0x00, 0x2D, 0x01, 0xC0, 0x40, 0x00}));
  CHECK(encode_frame(0x22, 0x07, 0x40C0, {}) == Bytes({0xA5, 0x22, 0x07, 0x04, 0x00, 0x2C, 0x01, 0xC0, 0x40, 0x00}));

  // power on / off
  CHECK(encode_frame(0x22, 1, 0xA000, {1}) == Bytes({0xA5, 0x22, 0x01, 0x05, 0x00, 0x90, 0x01, 0x00, 0xA0, 0x00, 0x01}));

  // auto mode with 50 %: target and a +-5 % band
  CHECK(auto_mode_data(50) == Bytes({50, 45, 55, 2, 1, 1}));
  Bytes frame = encode_frame(0x22, 5, 0x4080, auto_mode_data(50));
  CHECK(frame == Bytes({0xA5, 0x22, 0x05, 0x0A, 0x00, 0xCE, 0x01, 0x80, 0x40, 0x00, 0x32, 0x2D, 0x37, 0x02, 0x01, 0x01}));

  // manual level 2
  CHECK(manual_level_data(2) == Bytes({0, 1, 2}));

  // every encoded frame must parse back
  for (const Bytes &data : {Bytes{}, Bytes{1}, wifi_led_blink_data(), auto_mode_data(80)}) {
    FrameParser p;
    FrameParser::Result r;
    Bytes enc = encode_frame(0x22, 9, 0xA129, data);
    parse_all(p, enc, r);
    CHECK(r == FrameParser::Result::FRAME);
    CHECK(p.frame().cmd() == 0xA129);
    CHECK(Bytes(p.frame().data(), p.frame().data() + p.frame().data_len()) == data);
  }
}

// captured: MCU ACK to the status query, first boot with ESPHome
static const Bytes ACK_FRAME = {0xA5, 0x12, 0x06, 0x1D, 0x00, 0x57, 0x01, 0xC0, 0x40, 0x00, 0x02, 0x00,
                                0x02, 0x00, 0x01, 0x01, 0x00, 0x64, 0x01, 0x2D, 0x25, 0x17, 0x01, 0x01,
                                0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};

static void test_parse_ack() {
  FrameParser p;
  FrameParser::Result r;
  // leading noise is skipped
  Bytes stream = {0x00, 0x13, 0x37};
  stream.insert(stream.end(), ACK_FRAME.begin(), ACK_FRAME.end());
  Bytes raw = parse_all(p, stream, r);
  CHECK(r == FrameParser::Result::FRAME);
  CHECK(raw == ACK_FRAME);
  const Frame &f = p.frame();
  CHECK(f.supported());
  CHECK(f.is_ack());
  CHECK(!f.is_error());
  CHECK(f.seq == 0x06);
  CHECK(f.cmd() == 0x40C0);
  CHECK(f.data_len() == 25);

  Status s;
  CHECK(decode_status(f.data(), f.data_len(), s));
  CHECK(s.fw_major == 2 && s.fw_minor == 0 && s.fw_patch == 2);
  CHECK(!s.power);
  CHECK(s.target == 45);
  CHECK(s.humidity == 37);
  CHECK(s.temperature == 23);
  CHECK(s.manual());
  CHECK(s.mist_level == 1);
  CHECK(s.mode_index() == 1);  // the device showed the Low light
  CHECK(std::strcmp(MODE_NAMES[s.mode_index()], "Low") == 0);
  CHECK(s.display_config == 100);
  CHECK(!s.display_on());   // off while powered off, like the app
  CHECK(!s.low_water());    // water_lacks is ignored while off
  CHECK(!s.mist_active());
  CHECK(s.has_clean_reminder && !s.clean_reminder);
  CHECK(s.exception == 0);
}

static void test_status_variants() {
  // captured: "raw=02.00.02.00.00.01.00.64.01.2D.29.17.00.00.00.FF.FF.FF.FF.FF.00.FF.FF.FF.FF"
  Bytes off_auto = {0x02, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00, 0x64, 0x01, 0x2D, 0x29, 0x17, 0x00,
                    0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
  Status s;
  CHECK(decode_status(off_auto.data(), off_auto.size(), s));
  CHECK(!s.manual());
  CHECK(s.mode_index() == 0);
  CHECK(!s.tank_lifted);
  CHECK(s.water_lacks && !s.low_water());
  CHECK(s.humidity == 41);

  // running, manual high, with water: misting
  Bytes running = off_auto;
  running[3] = 1;   // power
  running[5] = 0;   // water ok
  running[12] = 1;  // manual
  running[13] = 2;  // level 2
  CHECK(decode_status(running.data(), running.size(), s));
  CHECK(s.mode_index() == 2);
  CHECK(s.display_on());
  CHECK(s.mist_active());
  running[6] = 1;  // auto stop reached
  CHECK(decode_status(running.data(), running.size(), s));
  CHECK(!s.mist_active());
  running[6] = 0;
  running[4] = 1;  // tank lifted
  CHECK(decode_status(running.data(), running.size(), s));
  CHECK(!s.mist_active());
  running[4] = 0;
  running[5] = 1;  // water lacks while running
  CHECK(decode_status(running.data(), running.size(), s));
  CHECK(s.low_water() && !s.mist_active());
  running[10] = 81;
  CHECK(decode_status(running.data(), running.size(), s));
  CHECK(s.humidity_high());

  // a 15-byte block (no clean-reminder byte) still decodes
  CHECK(decode_status(off_auto.data(), 15, s));
  CHECK(!s.has_clean_reminder);
  CHECK(!decode_status(off_auto.data(), 14, s));
}

static void test_parse_errors() {
  FrameParser p;
  FrameParser::Result r;
  Bytes bad = ACK_FRAME;
  bad[20] ^= 0x01;  // corrupt one data byte
  parse_all(p, bad, r);
  CHECK(r == FrameParser::Result::BAD_CHECKSUM);
  // the parser recovers for the next frame
  parse_all(p, ACK_FRAME, r);
  CHECK(r == FrameParser::Result::FRAME);

  // an impossible length resets the parser instead of waiting forever
  FrameParser q;
  Bytes huge = {0xA5, 0x22, 0x01, 0xFF, 0xFF, 0x00};
  parse_all(q, huge, r);
  CHECK(r == FrameParser::Result::NONE);
  CHECK(!q.busy());

  // the button event the MCU sends wants an ACK
  Bytes button = encode_frame(0x22, 0x10, 0xD102, {2});
  parse_all(q, button, r);
  CHECK(r == FrameParser::Result::FRAME);
  CHECK(q.frame().wants_ack() && !q.frame().is_ack());
}

static void test_night_light() {
  // captured 0x40C1 reports, pushed by the MCU after a night light button press
  FrameParser p;
  FrameParser::Result r;
  Status s;
  Bytes cycle = {0xA5, 0x02, 0x14, 0x1D, 0x00, 0x2B, 0x01, 0xC1, 0x40, 0x00, 0x02, 0x00, 0x02, 0x00,
                 0x00, 0x01, 0x00, 0x64, 0x01, 0x44, 0x3B, 0x17, 0x00, 0x00, 0x00, 0x01, 0x00, 0xFF,
                 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
  parse_all(p, cycle, r);
  CHECK(r == FrameParser::Result::FRAME);
  CHECK(p.frame().cmd() == 0x40C1 && !p.frame().is_ack());
  CHECK(decode_status(p.frame().data(), p.frame().data_len(), s));
  CHECK(s.has_night_light && s.night_light_on && s.night_light_cycle);

  Bytes blue = {0xA5, 0x02, 0x1B, 0x1D, 0x00, 0x58, 0x01, 0xC1, 0x40, 0x00, 0x02, 0x00, 0x02, 0x00,
                0x00, 0x01, 0x00, 0x64, 0x01, 0x44, 0x3B, 0x17, 0x00, 0x00, 0x00, 0x01, 0x01, 0x14,
                0x28, 0x8C, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
  parse_all(p, blue, r);
  CHECK(r == FrameParser::Result::FRAME);
  CHECK(decode_status(p.frame().data(), p.frame().data_len(), s));
  CHECK(s.night_light_on && !s.night_light_cycle);
  CHECK(s.night_light_r == 0x14 && s.night_light_g == 0x28 && s.night_light_b == 0x8C);
  CHECK(std::strcmp(night_light_color_name(s.night_light_r, s.night_light_g, s.night_light_b), "Blue") == 0);
  CHECK(night_light_color_name(1, 2, 3) == nullptr);

  // off keeps the last colour
  Bytes off(p.frame().data(), p.frame().data() + p.frame().data_len());
  off[15] = 0;
  CHECK(decode_status(off.data(), off.size(), s));
  CHECK(!s.night_light_on && s.night_light_b == 0x8C);
  // a 20-byte block has no night light
  CHECK(decode_status(off.data(), 19, s));
  CHECK(!s.has_night_light);
}

static void test_raw_command() {
  uint16_t cmd = 0;
  Bytes data;
  CHECK(parse_raw_command("A003 01 01 D2 50 00", cmd, data));
  CHECK(cmd == 0xA003 && data == Bytes({0x01, 0x01, 0xD2, 0x50, 0x00}));
  CHECK(parse_raw_command("0x40c0", cmd, data));
  CHECK(cmd == 0x40C0 && data.empty());
  CHECK(parse_raw_command("a1:05:64", cmd, data));
  CHECK(cmd == 0xA105 && data == Bytes({0x64}));
  CHECK(!parse_raw_command("A0", cmd, data));
  CHECK(!parse_raw_command("A003 1", cmd, data));
  CHECK(!parse_raw_command("A003 zz", cmd, data));
  // goes on the wire low byte first, like the known commands
  CHECK(encode_frame(0x22, 1, cmd, data)[7] == 0x05 && encode_frame(0x22, 1, cmd, data)[8] == 0xA1);
}

int main() {
  test_encode();
  test_parse_ack();
  test_status_variants();
  test_parse_errors();
  test_night_light();
  test_raw_command();
  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all protocol tests passed\n");
  return 0;
}
