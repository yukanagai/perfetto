/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX)

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/traced/traced.h"
#include "perfetto/ext/tracing/core/commit_data_request.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/tracing/core/tracing_service_state.h"
#include "src/base/test/test_task_runner.h"
#include "src/base/test/utils.h"
#include "src/traced/probes/ftrace/ftrace_controller.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

#include "protos/perfetto/config/test_config.gen.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.gen.h"
#include "protos/perfetto/trace/ftrace/ftrace_stats.gen.h"
#include "protos/perfetto/trace/perfetto/tracing_service_event.gen.h"
#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
#include "test/android_test_utils.h"
#endif

namespace perfetto {

namespace {

using ::testing::ContainsRegex;
using ::testing::Each;
using ::testing::ElementsAreArray;
using ::testing::HasSubstr;
using ::testing::Property;
using ::testing::SizeIs;

class PerfettoFtraceIntegrationTest : public ::testing::Test {
 public:
  void SetUp() override {
    ftrace_procfs_ = FtraceProcfs::CreateGuessingMountPoint();

// On android we do expect that tracefs is accessible, both in the case of
// running as part of traced/probes system daemons and shell. On Linux this is
// up to the system admin, don't hard fail.
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
    if (!ftrace_procfs_) {
      PERFETTO_ELOG(
          "Cannot acces tracefs. On Linux you need to manually run `sudo chown "
          "-R $USER /sys/kernel/tracing` to enable these tests. Skipping");
      GTEST_SKIP();
    } else {
      // Recent kernels set tracing_on=1 by default. On Android this is
      // disabled by initrc scripts. Be tolerant on Linux where we don't have
      // that and force disable ftrace.
      ftrace_procfs_->SetTracingOn(false);
    }
#endif
  }

  std::unique_ptr<FtraceProcfs> ftrace_procfs_;
};

}  // namespace

TEST_F(PerfettoFtraceIntegrationTest, TestFtraceProducer) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  ProbesProducerThread probes(GetTestProducerSockName());
  probes.Connect();
#endif

  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);
  trace_config.set_duration_ms(3000);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("linux.ftrace");
  ds_config->set_target_buffer(0);

  protos::gen::FtraceConfig ftrace_config;
  ftrace_config.add_ftrace_events("sched_switch");
  ftrace_config.add_ftrace_events("bar");
  ds_config->set_ftrace_config_raw(ftrace_config.SerializeAsString());

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled();

  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  ASSERT_GT(packets.size(), 0u);

  for (const auto& packet : packets) {
    for (int ev = 0; ev < packet.ftrace_events().event_size(); ev++) {
      ASSERT_TRUE(packet.ftrace_events()
                      .event()[static_cast<size_t>(ev)]
                      .has_sched_switch());
    }
  }
}

TEST_F(PerfettoFtraceIntegrationTest, TestFtraceFlush) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  ProbesProducerThread probes(GetTestProducerSockName());
  probes.Connect();
#endif

  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  // Wait for the traced_probes service to connect. We want to start tracing
  // only after it connects, otherwise we'll start a tracing session with 0
  // producers connected (which is valid but not what we want here).
  helper.WaitForDataSourceConnected("linux.ftrace");

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(32);
  trace_config.set_duration_ms(kDefaultTestTimeoutMs);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("linux.ftrace");

  protos::gen::FtraceConfig ftrace_config;
  ftrace_config.add_ftrace_events("print");
  ds_config->set_ftrace_config_raw(ftrace_config.SerializeAsString());

  helper.StartTracing(trace_config);

  // Wait for traced_probes to start.
  helper.WaitFor([&] { return ftrace_procfs_->IsTracingEnabled(); }, "ftrace");

  // Do a first flush just to synchronize with the producer. The problem here
  // is that, on a Linux workstation, the producer can take several seconds just
  // to get to the point where it is fully ready. We use the flush ack as a
  // synchronization point.
  helper.FlushAndWait(kDefaultTestTimeoutMs);

  const char kMarker[] = "just_one_event";
  EXPECT_TRUE(ftrace_procfs_->WriteTraceMarker(kMarker));

  // This is the real flush we are testing.
  helper.FlushAndWait(kDefaultTestTimeoutMs);

  helper.DisableTracing();
  helper.WaitForTracingDisabled(kDefaultTestTimeoutMs);

  helper.ReadData();
  helper.WaitForReadData();

  int marker_found = 0;
  for (const auto& packet : helper.trace()) {
    for (int i = 0; i < packet.ftrace_events().event_size(); i++) {
      const auto& ev = packet.ftrace_events().event()[static_cast<size_t>(i)];
      if (ev.has_print() && ev.print().buf().find(kMarker) != std::string::npos)
        marker_found++;
    }
  }
  ASSERT_EQ(marker_found, 1);
}

// Disable this test:
// 1. On cuttlefish (x86-kvm). It's too slow when running on GCE (b/171771440).
//    We cannot change the length of the production code in
//    CanReadKernelSymbolAddresses() to deal with it.
// 2. On user (i.e. non-userdebug) builds. As that doesn't work there by design.
// 3. On ARM builds, because they fail on our CI.
#if (PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD) && defined(__i386__)) || \
    defined(__arm__)
#define MAYBE_KernelAddressSymbolization DISABLED_KernelAddressSymbolization
#else
#define MAYBE_KernelAddressSymbolization KernelAddressSymbolization
#endif
TEST_F(PerfettoFtraceIntegrationTest, MAYBE_KernelAddressSymbolization) {
  // On Android in-tree builds (TreeHugger): this test must always run to
  // prevent selinux / property-related regressions. However it can run only on
  // userdebug.
  // On standalone builds and Linux, this can be optionally skipped because
  // there it requires root to lower kptr_restrict.
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  if (!IsDebuggableBuild())
    GTEST_SKIP();
#else
  if (geteuid() != 0)
    GTEST_SKIP();
#endif

  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  ProbesProducerThread probes(GetTestProducerSockName());
  probes.Connect();
#endif

  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(1024);

  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("linux.ftrace");
  protos::gen::FtraceConfig ftrace_cfg;
  ftrace_cfg.set_symbolize_ksyms(true);
  ftrace_cfg.set_initialize_ksyms_synchronously_for_testing(true);
  ds_config->set_ftrace_config_raw(ftrace_cfg.SerializeAsString());

  helper.StartTracing(trace_config);

  // Synchronize with the ftrace data source. The kernel symbol map is loaded
  // at this point.
  helper.FlushAndWait(kDefaultTestTimeoutMs);
  helper.DisableTracing();
  helper.WaitForTracingDisabled();
  helper.ReadData();
  helper.WaitForReadData();

  const auto& packets = helper.trace();
  ASSERT_GT(packets.size(), 0u);

  int symbols_parsed = -1;
  for (const auto& packet : packets) {
    if (!packet.has_ftrace_stats())
      continue;
    if (packet.ftrace_stats().phase() != protos::gen::FtraceStats::END_OF_TRACE)
      continue;
    symbols_parsed =
        static_cast<int>(packet.ftrace_stats().kernel_symbols_parsed());
  }
  ASSERT_GT(symbols_parsed, 100);
}

}  // namespace perfetto
#endif  // OS_ANDROID || OS_LINUX
