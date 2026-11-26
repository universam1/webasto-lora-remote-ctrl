#include <unity.h>
#include <vector>

#include "../../src/receiver/command_router.cpp"  // host build of the router (no Arduino includes)

using namespace rx;

struct FakeHeater : heater::IHeater {
  bool accept = true;
  std::vector<heater::HeaterRequest> got;
  bool begin() override { return true; }
  bool submit(const heater::HeaterRequest& r) override {
    if (!accept) return false;
    got.push_back(r);
    return true;
  }
  bool busy() const override { return !accept; }
  wh::HeaterSnapshot snapshot() const override { return {}; }
  bool popEvent(heater::HeaterEvent&) override { return false; }
  const char* transport() const override { return "fake"; }
  bool supportsVentilation() const override { return false; }
  bool supportsRunMinutes() const override { return false; }
};

struct Sink : EventSink {
  std::vector<rt::CmdEvent> ev;
  void emit(const rt::CmdEvent& e) override { ev.push_back(e); }
  void forceStatus() override {}
};

static wh::ClimateController* climate;
static FakeHeater* heat;
static Sink* sink;
static CommandRouter* router;

void setUp() {
  climate = new wh::ClimateController();
  heat = new FakeHeater();
  sink = new Sink();
  router = new CommandRouter(*climate, *heat, *sink);
}
void tearDown() {
  delete router; delete sink; delete heat; delete climate;
}

static rt::Command cmd(rt::CmdType t, rt::Source s = rt::Source::Lora, uint16_t seq = 0) {
  rt::Command c{};
  c.type = t;
  c.src = s;
  c.loraSeq = seq;
  return c;
}

static int countEv(uint16_t seq, wh::CmdResult r, uint8_t stage) {
  int n = 0;
  for (auto& e : sink->ev)
    if (e.loraSeq == seq && e.result == (uint8_t)r && e.stage == stage) n++;
  return n;
}

static void startClimate() {
  wh::ClimateSettings st;
  st.targetC = 20;
  st.durationMin = 60;
  climate->configure(st);
  climate->enable(0);
}

// Finding 1: a Stop parked behind a running start survives kRetryMs and ends the session only
// when the heater accepts it.
void test_parked_stop_survives_timeout_and_ends_climate_on_accept() {
  startClimate();
  heat->accept = false;
  router->route(cmd(rt::CmdType::Stop, rt::Source::Lora, 5), 1000);
  TEST_ASSERT_TRUE(climate->enabled());  // not ended while only requested
  TEST_ASSERT_FALSE(router->takePersist());
  router->service(1000 + CommandRouter::kRetryMs + 5000);
  TEST_ASSERT_EQUAL(0, countEv(5, wh::CmdResult::Busy, 4));  // no final failure
  TEST_ASSERT_EQUAL(1, countEv(5, wh::CmdResult::Pending, 1));  // "still pending" reported once
  router->service(1000 + CommandRouter::kRetryMs + 6000);
  TEST_ASSERT_EQUAL(1, countEv(5, wh::CmdResult::Pending, 1));
  TEST_ASSERT_TRUE(climate->enabled());
  heat->accept = true;
  router->service(60000);
  TEST_ASSERT_EQUAL(1, (int)heat->got.size());
  TEST_ASSERT_TRUE(heat->got[0].kind == wh::CmdKind::Stop);
  TEST_ASSERT_FALSE(climate->enabled());
  TEST_ASSERT_TRUE(router->takePersist());
}

void test_stop_accepted_at_once_ends_climate() {
  startClimate();
  router->route(cmd(rt::CmdType::Stop), 0);
  TEST_ASSERT_FALSE(climate->enabled());
  TEST_ASSERT_TRUE(router->takePersist());
}

// Heat expires as before.
void test_parked_heat_expires_with_busy() {
  heat->accept = false;
  router->route(cmd(rt::CmdType::Heat, rt::Source::Lora, 3), 0);
  router->service(CommandRouter::kRetryMs - 1);
  TEST_ASSERT_EQUAL(0, countEv(3, wh::CmdResult::Busy, 4));
  router->service(CommandRouter::kRetryMs);
  TEST_ASSERT_EQUAL(1, countEv(3, wh::CmdResult::Busy, 4));
  heat->accept = true;
  router->service(CommandRouter::kRetryMs + 100);
  TEST_ASSERT_EQUAL(0, (int)heat->got.size());  // expired, not submitted later
}

// Finding 2: a parked Stop is sticky.
void test_heat_after_parked_stop_is_rejected() {
  heat->accept = false;
  router->route(cmd(rt::CmdType::Stop, rt::Source::Lora, 5), 0);
  router->route(cmd(rt::CmdType::Heat, rt::Source::Ble, 6), 10);
  TEST_ASSERT_EQUAL(1, countEv(6, wh::CmdResult::Busy, 4));
  heat->accept = true;
  router->service(20);
  TEST_ASSERT_EQUAL(1, (int)heat->got.size());
  TEST_ASSERT_TRUE(heat->got[0].kind == wh::CmdKind::Stop);
}

// Even if the heater is free again, a Heat must not overtake a parked Stop.
void test_heat_does_not_overtake_parked_stop() {
  heat->accept = false;
  router->route(cmd(rt::CmdType::Stop, rt::Source::Lora, 5), 0);
  heat->accept = true;
  router->route(cmd(rt::CmdType::Heat, rt::Source::Lora, 6), 5);
  TEST_ASSERT_EQUAL(0, (int)heat->got.size());
  TEST_ASSERT_EQUAL(1, countEv(6, wh::CmdResult::Busy, 4));
}

void test_stop_replaces_parked_heat() {
  heat->accept = false;
  router->route(cmd(rt::CmdType::Heat, rt::Source::Lora, 3), 0);
  router->route(cmd(rt::CmdType::Stop, rt::Source::Lora, 4), 10);
  TEST_ASSERT_EQUAL(1, countEv(3, wh::CmdResult::Busy, 4));
  heat->accept = true;
  router->service(20);
  TEST_ASSERT_EQUAL(1, (int)heat->got.size());
  TEST_ASSERT_TRUE(heat->got[0].kind == wh::CmdKind::Stop);
}

void test_newer_stop_replaces_parked_stop() {
  heat->accept = false;
  router->route(cmd(rt::CmdType::Stop, rt::Source::Lora, 5), 0);
  router->route(cmd(rt::CmdType::Stop, rt::Source::Ble, 0), 10);
  TEST_ASSERT_EQUAL(1, countEv(5, wh::CmdResult::Busy, 4));
  heat->accept = true;
  router->service(20);
  TEST_ASSERT_EQUAL(1, (int)heat->got.size());
  TEST_ASSERT_TRUE(heat->got[0].src == heater::Source::Ble);
}

void test_climate_start_blocked_by_parked_stop() {
  heat->accept = false;
  router->route(cmd(rt::CmdType::Stop), 0);
  heat->accept = true;
  router->startClimate(10);
  TEST_ASSERT_EQUAL(0, (int)heat->got.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_parked_stop_survives_timeout_and_ends_climate_on_accept);
  RUN_TEST(test_stop_accepted_at_once_ends_climate);
  RUN_TEST(test_parked_heat_expires_with_busy);
  RUN_TEST(test_heat_after_parked_stop_is_rejected);
  RUN_TEST(test_heat_does_not_overtake_parked_stop);
  RUN_TEST(test_stop_replaces_parked_heat);
  RUN_TEST(test_newer_stop_replaces_parked_stop);
  RUN_TEST(test_climate_start_blocked_by_parked_stop);
  return UNITY_END();
}
