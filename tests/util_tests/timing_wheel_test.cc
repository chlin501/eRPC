#include <gtest/gtest.h>
#include <time.h>
#include <algorithm>
#include <vector>

#include "util/huge_alloc.h"
#include "util/test_printf.h"

#define private public
#include "cc/timing_wheel.h"

using namespace erpc;

static constexpr size_t kTestMTU = 1024;       // Few wheel slots
static constexpr double kTestWslotWidth = .5;  // .5 microseconds per wheel slot

// Dummy registration and deregistration functions
Transport::MemRegInfo reg_mr_wrapper(void *, size_t) {
  return Transport::MemRegInfo(0, 0);
}

void dereg_mr_wrapper(Transport::MemRegInfo) {}

using namespace std::placeholders;
typename Transport::reg_mr_func_t reg_mr_func =
    std::bind(reg_mr_wrapper, _1, _2);
typename Transport::dereg_mr_func_t dereg_mr_func =
    std::bind(dereg_mr_wrapper, _1);

TEST(TimingWheelTest, Basic) {
  HugeAlloc alloc(MB(2), 0, reg_mr_func, dereg_mr_func);
  timing_wheel_args_t args;
  args.mtu = kTestMTU;
  args.freq_ghz = measure_rdtsc_freq();
  args.wslot_width = kTestWslotWidth;
  args.huge_alloc = &alloc;

  TimingWheel wheel(args);
  const auto dummy_ent = wheel_ent_t(nullptr, 1);

  // Empty wheel
  wheel.reap(rdtsc());
  ASSERT_EQ(wheel.ready_queue.size(), 0);

  // One entry. Check that it's eventually sent.
  wheel.insert(dummy_ent, rdtsc() + wheel.wslot_width_tsc);

  while (true) {
    wheel.reap(rdtsc());
    if (wheel.ready_queue.size() > 0) break;
  }
}

TEST(TimingWheelTest, RateTest) {
  HugeAlloc alloc(MB(2), 0, reg_mr_func, dereg_mr_func);
  timing_wheel_args_t args;
  args.mtu = kTestMTU;
  args.freq_ghz = measure_rdtsc_freq();
  args.wslot_width = kTestWslotWidth;
  args.huge_alloc = &alloc;

  TimingWheel wheel(args);
  const auto dummy_ent = wheel_ent_t(nullptr, 1);
  const size_t num_pkts = 10000;
  size_t num_pkts_sent = 0;

  std::uniform_real_distribution<double> unif(kTimelyMinRate, kTimelyMaxRate);
  std::default_random_engine re;

  for (size_t iters = 0; iters < 5; iters++) {
    double target_rate = unif(re);
    test_printf("Target rate = %.2f Gbps\n", Timely::rate_to_gbps(target_rate));

    size_t msr_start_tsc = rdtsc();  // Measurement start

    const double ns_per_pkt = 1000000000 * (kTestMTU / target_rate);
    const size_t cycles_per_pkt = round_up(args.freq_ghz * ns_per_pkt);
    size_t last_tsc = rdtsc();

    // Send one window
    for (size_t i = 0; i < kSessionCredits; i++) {
      wheel.insert(dummy_ent, last_tsc);
      last_tsc += cycles_per_pkt;
    }

    while (num_pkts_sent < num_pkts) {
      printf("while: num_pkts_sent = %zu\n", num_pkts_sent);
      wheel.reap(rdtsc());
      size_t num_ready = wheel.ready_queue.size();

      if (num_ready > 0) {
        num_pkts_sent += num_ready;
        // Send more
        for (size_t i = 0; i < num_ready; i++) {
          wheel.insert(dummy_ent, last_tsc);
          last_tsc += cycles_per_pkt;
          wheel.ready_queue.pop();
        }
      }
    }

    double seconds = to_sec(rdtsc() - msr_start_tsc, args.freq_ghz);
    double achieved_rate = kSessionCredits * kTestMTU / seconds;

    test_printf("Achieved rate = %.2f Gbps\n",
                Timely::rate_to_gbps(achieved_rate));
    while (!wheel.ready_queue.empty()) wheel.ready_queue.pop();
  }
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
