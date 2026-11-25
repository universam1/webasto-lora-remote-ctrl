#include <unity.h>

#include "can_command_fsm.h"

using namespace wh;

void setUp() {}
void tearDown() {}

// Simulated bus: roof unit (panel), heater and CAN port, driven by a virtual clock.
struct Sim : CanPort {
  CanCommandFsm fsm;
  uint32_t now = 1000;
  // panel
  bool panelAwake = true;       // sends 0x5D3 every 500 ms
  uint8_t panelByte = 0x80;
  bool panelAdopts = true;      // accepts our frame when bit 7 matches
  int adoptAfter = 1;           // number of matching frames needed
  uint32_t nextPanel = 1250;
  uint32_t panelDelay = 100;    // first panel frame after wake
  bool wakesOnTx = false;       // asleep until our first frame
  // heater
  bool heaterLed = false;
  bool heaterConfirms = true;
  uint32_t nextHeater = 1100;
  // port
  bool normal = false;
  int normalOnCount = 0, normalOffCount = 0, sendCount = 0;
  bool failNormal = false, failSend = false;
  Frame sent[32];
  uint32_t sentAt[32];
  int matching = 0;

  Sim() : fsm(*this) {}

  bool setNormalMode(bool on) override {
    if (on && failNormal) return false;
    normal = on;
    if (on) normalOnCount++; else normalOffCount++;
    return true;
  }
  bool send(const Frame& f) override {
    if (failSend) return false;
    if (sendCount < 32) { sent[sendCount] = f; sentAt[sendCount] = now; }
    sendCount++;
    if (wakesOnTx && !panelAwake) {
      panelAwake = true;
      nextPanel = now + panelDelay;
    }
    if (panelAdopts && (f.d[0] & 0x80) == (panelByte & 0x80) && f.d[0] != panelByte) {
      if (++matching >= adoptAfter) {
        panelByte = f.d[0];
        heaterLed = (f.d[0] & 0x02) != 0;
      }
    }
    return true;
  }

  void deliver(const Frame& f) { fsm.onFrame(f, now); }

  void step(uint32_t ms) {
    for (uint32_t end = now + ms; (int32_t)(now - end) < 0;) {
      now += 10;
      if (panelAwake && (int32_t)(now - nextPanel) >= 0) {
        deliver(make5D3(panelByte));
        nextPanel += 500;
      }
      if ((int32_t)(now - nextHeater) >= 0) {
        Frame f;
        f.id = kIdStatus;
        f.dlc = 8;
        f.d[1] = (heaterConfirms && heaterLed) ? 0x02 : 0x00;
        f.d[7] = 0;
        deliver(f);
        nextHeater += 200;
      }
      fsm.tick(now);
    }
  }

  // run until the command finishes, at most maxMs
  bool finish(CmdResult& r, uint8_t& frames, uint32_t maxMs = 15000) {
    for (uint32_t t = 0; t < maxMs; t += 10) {
      step(10);
      if (fsm.takeResult(r, frames)) return true;
    }
    return false;
  }
};

void test_heat_adopted_after_three_frames() {
  Sim s;
  s.adoptAfter = 3;
  s.step(1000);  // panel known and fresh
  TEST_ASSERT_EQUAL(CmdResult::Pending, s.fsm.request(CmdKind::Heat, s.now));
  CmdResult r;
  uint8_t n = 0;
  TEST_ASSERT_TRUE(s.finish(r, n));
  TEST_ASSERT_EQUAL(CmdResult::Ok, r);
  TEST_ASSERT_EQUAL_UINT8(3, n);
  TEST_ASSERT_EQUAL_INT(3, s.sendCount);
  TEST_ASSERT_EQUAL_UINT8(0x82, s.sent[0].d[0]);
  TEST_ASSERT_EQUAL_INT(1, s.normalOnCount);
  TEST_ASSERT_EQUAL_INT(1, s.normalOffCount);
  TEST_ASSERT_FALSE(s.normal);
  TEST_ASSERT_FALSE(s.fsm.busy());
  TEST_ASSERT_FALSE(s.fsm.takeResult(r, n));  // exactly once
}

void test_bit7_mirroring() {
  uint8_t panel[2] = {0x80, 0x00};
  uint8_t expect[2] = {0x82, 0x02};
  for (int i = 0; i < 2; i++) {
    Sim s;
    s.panelByte = panel[i];
    s.step(1000);
    s.fsm.request(CmdKind::Heat, s.now);
    CmdResult r;
    uint8_t n;
    TEST_ASSERT_TRUE(s.finish(r, n));
    TEST_ASSERT_EQUAL(CmdResult::Ok, r);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_UINT8(expect[i], s.sent[0].d[0]);
    TEST_ASSERT_EQUAL_UINT8(0, s.sent[0].d[1]);
    TEST_ASSERT_EQUAL_UINT8(2, s.sent[0].dlc);
  }
}

void test_vent_unsupported() {
  Sim s;
  s.step(1000);
  TEST_ASSERT_EQUAL(CmdResult::Unsupported, s.fsm.request(CmdKind::Vent, s.now));
  TEST_ASSERT_FALSE(s.fsm.busy());
  s.step(1000);
  TEST_ASSERT_EQUAL_INT(0, s.sendCount);
  TEST_ASSERT_EQUAL_INT(0, s.normalOnCount);
  TEST_ASSERT_EQUAL_INT(0, s.normalOffCount);
}

void test_not_adopted_after_max_frames_plus_slot() {
  Sim s;
  s.panelAdopts = false;
  s.step(1000);
  uint32_t t0 = s.now;
  s.fsm.request(CmdKind::Heat, t0);
  CmdResult r;
  uint8_t n;
  TEST_ASSERT_TRUE(s.finish(r, n));
  TEST_ASSERT_EQUAL(CmdResult::NotAdopted, r);
  TEST_ASSERT_EQUAL_UINT8(10, n);
  TEST_ASSERT_EQUAL_INT(10, s.sendCount);
  // one more slot after the last frame
  TEST_ASSERT_UINT32_WITHIN(20, s.sentAt[9] + 500, s.now);
  TEST_ASSERT_EQUAL_INT(1, s.normalOffCount);
  TEST_ASSERT_FALSE(s.normal);
}

void test_sleeping_bus_corrects_bit7() {
  Sim s;
  s.panelAwake = false;
  s.wakesOnTx = true;
  s.panelByte = 0x00;
  s.fsm.request(CmdKind::Heat, s.now);
  CmdResult r;
  uint8_t n;
  TEST_ASSERT_TRUE(s.finish(r, n));
  TEST_ASSERT_EQUAL(CmdResult::Ok, r);
  TEST_ASSERT_EQUAL_UINT8(2, n);
  TEST_ASSERT_EQUAL_UINT8(0x82, s.sent[0].d[0]);
  TEST_ASSERT_EQUAL_UINT8(0x02, s.sent[1].d[0]);
  // corrected frame is not sent before one slot after the first
  TEST_ASSERT_TRUE((uint32_t)(s.sentAt[1] - s.sentAt[0]) >= 500);
  TEST_ASSERT_EQUAL_INT(1, s.normalOffCount);
}

void test_stale_panel_not_trusted() {
  Sim s;
  s.step(1000);          // panel 0x80 seen
  s.panelAwake = false;  // goes silent
  s.wakesOnTx = true;
  s.panelByte = 0x00;    // will come back with bit 7 clear
  s.step(3000);          // older than 2 s now
  s.fsm.request(CmdKind::Heat, s.now);
  CmdResult r;
  uint8_t n;
  TEST_ASSERT_TRUE(s.finish(r, n));
  TEST_ASSERT_EQUAL(CmdResult::Ok, r);
  TEST_ASSERT_EQUAL_UINT8(0x82, s.sent[0].d[0]);  // last seen bit 7
  TEST_ASSERT_EQUAL_UINT8(0x02, s.sent[1].d[0]);  // corrected
  TEST_ASSERT_EQUAL_UINT8(2, n);
}

void test_panel_already_at_target() {
  Sim s;
  s.panelByte = 0x82;
  s.heaterLed = true;
  s.step(1000);
  s.fsm.request(CmdKind::Heat, s.now);
  CmdResult r;
  uint8_t n;
  TEST_ASSERT_TRUE(s.finish(r, n));
  TEST_ASSERT_EQUAL(CmdResult::Ok, r);
  TEST_ASSERT_EQUAL_UINT8(1, n);
}

void test_off_after_retries() {
  Sim s;
  s.panelByte = 0x82;
  s.heaterLed = true;
  s.adoptAfter = 3;
  s.step(1000);
  s.fsm.request(CmdKind::Stop, s.now);
  CmdResult r;
  uint8_t n;
  TEST_ASSERT_TRUE(s.finish(r, n));
  TEST_ASSERT_EQUAL(CmdResult::Ok, r);
  TEST_ASSERT_EQUAL_UINT8(3, n);
  TEST_ASSERT_EQUAL_UINT8(0x80, s.sent[0].d[0]);
  TEST_ASSERT_FALSE(s.heaterLed);
}

void test_unconfirmed() {
  Sim s;
  s.heaterConfirms = false;
  s.step(1000);
  s.fsm.request(CmdKind::Heat, s.now);
  CmdResult r;
  uint8_t n;
  TEST_ASSERT_TRUE(s.finish(r, n));
  TEST_ASSERT_EQUAL(CmdResult::OkUnconfirmed, r);
  TEST_ASSERT_EQUAL_UINT8(1, n);
  TEST_ASSERT_EQUAL_INT(1, s.normalOffCount);
}

void test_busoff_restores_listen_only() {
  Sim s;
  s.panelAdopts = false;
  s.step(1000);
  s.fsm.request(CmdKind::Heat, s.now);
  s.step(700);
  TEST_ASSERT_TRUE(s.normal);
  s.fsm.onBusEvent(true, s.now);
  CmdResult r;
  uint8_t n;
  TEST_ASSERT_TRUE(s.fsm.takeResult(r, n));
  TEST_ASSERT_EQUAL(CmdResult::BusOff, r);
  TEST_ASSERT_FALSE(s.normal);
  TEST_ASSERT_EQUAL_INT(1, s.normalOffCount);
  TEST_ASSERT_FALSE(s.fsm.busy());
}

void test_busy_while_running() {
  Sim s;
  s.step(1000);
  TEST_ASSERT_EQUAL(CmdResult::Pending, s.fsm.request(CmdKind::Heat, s.now));
  TEST_ASSERT_EQUAL(CmdResult::Busy, s.fsm.request(CmdKind::Stop, s.now));
  TEST_ASSERT_EQUAL(CmdResult::Busy, s.fsm.request(CmdKind::Wake, s.now));
  TEST_ASSERT_TRUE(s.fsm.busy());
}

void test_no_bus_cases() {
  Sim s;
  s.failNormal = true;
  s.step(1000);
  s.fsm.request(CmdKind::Heat, s.now);
  CmdResult r;
  uint8_t n;
  TEST_ASSERT_TRUE(s.finish(r, n));
  TEST_ASSERT_EQUAL(CmdResult::NoBus, r);
  TEST_ASSERT_EQUAL_INT(0, s.sendCount);

  Sim s2;
  s2.failSend = true;
  s2.step(1000);
  s2.fsm.request(CmdKind::Heat, s2.now);
  TEST_ASSERT_TRUE(s2.finish(r, n));
  TEST_ASSERT_EQUAL(CmdResult::NoBus, r);
  TEST_ASSERT_FALSE(s2.normal);
  TEST_ASSERT_EQUAL_INT(1, s2.normalOffCount);
}

void test_wake_sends_one_neutral_frame() {
  Sim s;
  s.panelByte = 0x82;
  s.step(1000);
  s.panelAwake = false;
  s.fsm.request(CmdKind::Wake, s.now);
  CmdResult r;
  uint8_t n;
  TEST_ASSERT_TRUE(s.finish(r, n));
  TEST_ASSERT_EQUAL(CmdResult::Ok, r);
  TEST_ASSERT_EQUAL_UINT8(1, n);
  TEST_ASSERT_EQUAL_UINT8(0x82, s.sent[0].d[0]);
  TEST_ASSERT_EQUAL_INT(1, s.normalOffCount);
  // listened for about 3 s in normal mode
  TEST_ASSERT_UINT32_WITHIN(100, s.sentAt[0] + 3000, s.now);
}

void test_wake_default_byte_when_panel_never_seen() {
  Sim s;
  s.panelAwake = false;
  s.fsm.request(CmdKind::Wake, s.now);
  CmdResult r;
  uint8_t n;
  TEST_ASSERT_TRUE(s.finish(r, n));
  TEST_ASSERT_EQUAL_UINT8(0x80, s.sent[0].d[0]);
}

void test_clock_wrap() {
  Sim s;
  s.now = 0xFFFFFF00u;
  s.nextPanel = s.now + 250;
  s.nextHeater = s.now + 100;
  s.adoptAfter = 2;
  s.step(1000);
  s.fsm.request(CmdKind::Heat, s.now);
  CmdResult r;
  uint8_t n;
  TEST_ASSERT_TRUE(s.finish(r, n));
  TEST_ASSERT_EQUAL(CmdResult::Ok, r);
  TEST_ASSERT_EQUAL_UINT8(2, n);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_heat_adopted_after_three_frames);
  RUN_TEST(test_bit7_mirroring);
  RUN_TEST(test_vent_unsupported);
  RUN_TEST(test_not_adopted_after_max_frames_plus_slot);
  RUN_TEST(test_sleeping_bus_corrects_bit7);
  RUN_TEST(test_stale_panel_not_trusted);
  RUN_TEST(test_panel_already_at_target);
  RUN_TEST(test_off_after_retries);
  RUN_TEST(test_unconfirmed);
  RUN_TEST(test_busoff_restores_listen_only);
  RUN_TEST(test_busy_while_running);
  RUN_TEST(test_no_bus_cases);
  RUN_TEST(test_wake_sends_one_neutral_frame);
  RUN_TEST(test_wake_default_byte_when_panel_never_seen);
  RUN_TEST(test_clock_wrap);
  return UNITY_END();
}
