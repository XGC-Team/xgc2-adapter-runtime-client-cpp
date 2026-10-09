#include <fcntl.h>
#include <google/protobuf/util/message_differencer.h>

#include <future>

#include "../src/internal.hpp"
#include "adapter_runtime_client_test_support.hpp"

namespace {

xgc2::adapter_runtime::CapabilityCallbacks ProtocolFixtureCallbacks() {
  xgc2::adapter_runtime::CapabilityCallbacks callbacks;
  callbacks.unary = [](const xgc::adapter::v1::UnaryRequest&,
                       const xgc2::adapter_runtime::CancellationToken&) {
    return xgc2::adapter_runtime::UnaryResult::Success(TestPayload("protocol-fixture"));
  };
  callbacks.operation = [](const xgc::adapter::v1::OperationRequest&,
                           const xgc2::adapter_runtime::CancellationToken&) {
    return xgc2::adapter_runtime::OperationResult::Success();
  };
  callbacks.source_open = [](const xgc::adapter::v1::SourceOpenRequest&,
                             const xgc2::adapter_runtime::CancellationToken&) {
    return xgc2::adapter_runtime::SourceOpenDecision::Reject(
        xgc::adapter::v1::ERROR_CLASS_REJECTED, "protocol-fixture",
        "protocol fixture does not open sources");
  };
  return callbacks;
}

void BindProtocolFixture(xgc2::adapter_runtime::ClientConfig* config) {
  ASSERT_NE(config, nullptr);
  std::string error;
  ASSERT_TRUE(config->BindCapability(kCapabilityId, kContractVersion, kContractDigest,
                                     ProtocolFixtureCallbacks(), &error))
      << error;
  ASSERT_TRUE(config->BindCapability("test.disabled", kContractVersion,
                                     kDisabledContractDigest, {}, &error))
      << error;
}

}  // namespace

TEST(AdapterRuntimeHeartbeatSchedule,
     AlignsAfterTheImmediateHeartbeatToHostClockBoundaries) {
  using std::chrono::milliseconds;
  const auto wall_now = std::chrono::system_clock::time_point(milliseconds(12345));
  const auto monotonic_now = std::chrono::steady_clock::time_point(milliseconds(7000));
  const auto deadline = xgc2::adapter_runtime::internal::NextAlignedHeartbeatDeadline(
      wall_now, monotonic_now, milliseconds(5000));
  EXPECT_EQ(std::chrono::duration_cast<milliseconds>(deadline - monotonic_now),
            milliseconds(2655));

  const auto exact_boundary =
      xgc2::adapter_runtime::internal::NextAlignedHeartbeatDeadline(
          std::chrono::system_clock::time_point(milliseconds(15000)), monotonic_now,
          milliseconds(5000));
  EXPECT_EQ(std::chrono::duration_cast<milliseconds>(exact_boundary - monotonic_now),
            milliseconds(5000));
}

TEST(AdapterRuntimeHeartbeatSchedule,
     ClockJumpsAndDelayedWakeNeverIncreaseTheNegotiatedMaximumGap) {
  using std::chrono::milliseconds;
  const auto interval = milliseconds(5000);
  const auto delayed_monotonic_now =
      std::chrono::steady_clock::time_point(milliseconds(91000));
  for (const auto wall_now : {
           std::chrono::system_clock::time_point(milliseconds(57601)),
           std::chrono::system_clock::time_point(milliseconds(3101)),
           std::chrono::system_clock::time_point(milliseconds(-8999)),
       }) {
    const auto deadline = xgc2::adapter_runtime::internal::NextAlignedHeartbeatDeadline(
        wall_now, delayed_monotonic_now, interval);
    const auto delay = std::chrono::duration_cast<std::chrono::nanoseconds>(
        deadline - delayed_monotonic_now);
    EXPECT_GT(delay.count(), 0);
    EXPECT_LE(delay, std::chrono::duration_cast<std::chrono::nanoseconds>(interval));
  }
}

TEST_F(AdapterRuntimeClientTest,
       RunsFencedDualStreamsCapabilityGatingReplayAndSourceCredit) {
  auto config = xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(bootstrap_path_);
  config.initial_connect_timeout_ms = 3000;
  config.rpc_timeout_ms = 1000;

  std::atomic<int> enabled_started{0};
  std::atomic<int> enabled_ready{0};
  std::atomic<int> enabled_stopped{0};
  std::atomic<int> disabled_started{0};
  std::atomic<int> unary_calls{0};
  std::atomic<int> operation_calls{0};
  std::atomic<bool> deadline_normalized{false};
  std::atomic<bool> applied_scope_and_secret{false};
  std::atomic<int> cleared{0};
  std::atomic<xgc2::adapter_runtime::SourceWriteResult> precredit_result{
      xgc2::adapter_runtime::SourceWriteResult::kNotReady};
  xgc2::adapter_runtime::Client* client_ptr = nullptr;

  xgc2::adapter_runtime::CapabilityCallbacks enabled;
  enabled.start = [&](const xgc::adapter::v1::AdapterInstanceSpec&,
                      const xgc::adapter::v1::EnabledCapability&, std::string*) {
    ++enabled_started;
    return true;
  };
  enabled.ready = [&] { ++enabled_ready; };
  enabled.stop = [&] { ++enabled_stopped; };
  enabled.unary = [&](const xgc::adapter::v1::UnaryRequest& request,
                      const xgc2::adapter_runtime::CancellationToken&) {
    ++unary_calls;
    deadline_normalized.store(request.context().deadline().ttl_ms() == 0 &&
                              request.context().deadline().deadline_unix_nanos() >
                                  NowNanos());
    return xgc2::adapter_runtime::UnaryResult::Success(TestPayload("unary-result"));
  };
  enabled.operation = [&](const xgc::adapter::v1::OperationRequest&,
                          const xgc2::adapter_runtime::CancellationToken& token) {
    ++operation_calls;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!token.IsCancellationRequested() &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return xgc2::adapter_runtime::OperationResult::Cancelled();
  };
  enabled.source_open = [&](const xgc::adapter::v1::SourceOpenRequest& request,
                            const xgc2::adapter_runtime::CancellationToken&) {
    EXPECT_EQ(request.context().work_id(), "source-1");
    EXPECT_EQ(request.context().capability_id(), kCapabilityId);
    EXPECT_EQ(request.context().endpoint_id(), "events");
    EXPECT_EQ(request.initial_credit().messages(), 1U);
    EXPECT_EQ(request.initial_credit().bytes(), 8U);
    precredit_result.store(
        client_ptr->PublishSource(request.context().work_id(), {"before-accept"}));
    return xgc2::adapter_runtime::SourceOpenDecision::Accept();
  };
  std::string bind_error;
  ASSERT_TRUE(config.BindCapability(kCapabilityId, kContractVersion, kContractDigest,
                                    std::move(enabled), &bind_error))
      << bind_error;

  xgc2::adapter_runtime::CapabilityCallbacks disabled;
  disabled.start = [&](const xgc::adapter::v1::AdapterInstanceSpec&,
                       const xgc::adapter::v1::EnabledCapability&, std::string*) {
    ++disabled_started;
    return true;
  };
  ASSERT_TRUE(config.BindCapability("test.disabled", kContractVersion,
                                    kDisabledContractDigest, std::move(disabled),
                                    &bind_error))
      << bind_error;

  xgc2::adapter_runtime::ClientCallbacks callbacks;
  callbacks.apply_instance_spec = [&](const xgc::adapter::v1::AdapterInstanceSpec& spec,
                                      std::string*) {
    applied_scope_and_secret.store(
        spec.scope().kind() == "tenant-context" &&
        spec.scope().attributes().at("target") == "fleet-a" &&
        spec.secrets(0).version() == "sha256:secret-version");
    return true;
  };
  callbacks.clear_instance_spec = [&] { ++cleared; };

  xgc2::adapter_runtime::Client client(std::move(config), std::move(callbacks));
  client_ptr = &client;
  std::string error;
  ASSERT_TRUE(client.Start(&error)) << error;
  EXPECT_EQ(client.session().state, xgc2::adapter_runtime::ClientState::kReady);
  EXPECT_EQ(client.session().spec_revision, kSpecRevision);
  EXPECT_EQ(enabled_started.load(), 1);
  EXPECT_EQ(enabled_ready.load(), 1);
  EXPECT_EQ(disabled_started.load(), 0);
  EXPECT_TRUE(applied_scope_and_secret.load());
  EXPECT_EQ(precredit_result.load(),
            xgc2::adapter_runtime::SourceWriteResult::kNotReady);

  service_->ReleaseSourceCredit();
  ASSERT_TRUE(service_->WaitForSourceCredit());
  xgc2::adapter_runtime::SourceWriteResult publish =
      xgc2::adapter_runtime::SourceWriteResult::kNoCredit;
  const auto publish_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (publish == xgc2::adapter_runtime::SourceWriteResult::kNoCredit &&
         std::chrono::steady_clock::now() < publish_deadline) {
    publish = client.PublishSource("source-1", {"one", "two"});
    if (publish == xgc2::adapter_runtime::SourceWriteResult::kNoCredit) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  EXPECT_EQ(publish, xgc2::adapter_runtime::SourceWriteResult::kAccepted);
  if (publish != xgc2::adapter_runtime::SourceWriteResult::kAccepted) {
    ADD_FAILURE() << "Runtime Link state=" << static_cast<int>(client.session().state)
                  << " last_error=" << client.session().last_error;
  }
  ASSERT_TRUE(service_->WaitForCompleteExchange());
  EXPECT_TRUE(service_->paired_epoch());
  EXPECT_EQ(service_->registrations(), 1);
  EXPECT_EQ(unary_calls.load(), 1);
  EXPECT_EQ(operation_calls.load(), 1);
  EXPECT_TRUE(deadline_normalized.load());

  ASSERT_TRUE(client.CloseSource("source-1"));
  EXPECT_TRUE(service_->WaitForSourceClosed());
  client.Stop();
  EXPECT_EQ(client.session().state, xgc2::adapter_runtime::ClientState::kStopped);
  EXPECT_EQ(enabled_stopped.load(), 1);
  EXPECT_EQ(cleared.load(), 1);
}

TEST_F(AdapterRuntimeClientTest, RejectsInsecureBootstrapAndUntrustedHandler) {
  ASSERT_EQ(::chmod(bootstrap_path_.c_str(), 0644), 0);
  EXPECT_THROW(xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(bootstrap_path_),
               std::runtime_error);
  ASSERT_EQ(::chmod(bootstrap_path_.c_str(), 0600), 0);
  auto config = xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(bootstrap_path_);
  std::string error;
  EXPECT_FALSE(config.BindCapability("not-trusted", 1, "digest", {}, &error));
  EXPECT_FALSE(error.empty());
}

TEST_F(AdapterRuntimeClientTest, RejectsLegacyBootstrapProtocolBeforeRegister) {
  bootstrap_.mutable_registration()->set_runtime_link_protocol_version(
      xgc2::adapter_runtime::kRuntimeLinkProtocolVersion - 1U);
  std::ofstream output(bootstrap_path_,
                       std::ios::out | std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(bootstrap_.SerializeToOstream(&output));
  output.close();
  ASSERT_EQ(::chmod(bootstrap_path_.c_str(), 0600), 0);

  auto config = xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(bootstrap_path_);
  BindProtocolFixture(&config);
  xgc2::adapter_runtime::Client client(std::move(config), {});
  std::string error;
  EXPECT_FALSE(client.Start(&error));
  EXPECT_NE(error.find("protocol version 2 exclusively"), std::string::npos) << error;
  EXPECT_EQ(service_->registration_attempts(), 0);
}

class LegacyProtocolSelectionTest : public AdapterRuntimeClientTest {
 protected:
  FakeRuntimeLink::Scenario scenario() const override {
    return FakeRuntimeLink::Scenario::kLegacyProtocolSelection;
  }
};

TEST_F(LegacyProtocolSelectionTest, RejectsHostSelectedLegacyProtocol) {
  auto config = xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(bootstrap_path_);
  config.initial_connect_timeout_ms = 3000;
  BindProtocolFixture(&config);
  xgc2::adapter_runtime::Client client(std::move(config), {});
  std::string error;
  EXPECT_FALSE(client.Start(&error));
  EXPECT_NE(error.find("protocol version 2"), std::string::npos) << error;
  EXPECT_EQ(service_->registrations(), 1);
}

TEST_F(AdapterRuntimeClientTest,
       RejectsNegotiatedFrameLimitThatCannotFitTerminalMetadata) {
  auto config = xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(bootstrap_path_);
  config.initial_connect_timeout_ms = 3000;
  config.maximum_terminal_replay_bytes = 1024U * 1024U;
  BindProtocolFixture(&config);
  xgc2::adapter_runtime::Client client(std::move(config), {});
  std::string error;
  EXPECT_FALSE(client.Start(&error));
  EXPECT_NE(error.find("byte budgets"), std::string::npos) << error;
  EXPECT_EQ(service_->registrations(), 1);
}

TEST_F(AdapterRuntimeClientTest,
       ReadyCallbackFailureIsReportedBeforeTerminalSessionLoss) {
  auto config = xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(bootstrap_path_);
  config.initial_connect_timeout_ms = 3000;
  auto enabled = ProtocolFixtureCallbacks();
  std::atomic<int> stops{0};
  std::atomic<int> clears{0};
  enabled.ready = [] { throw std::runtime_error("private native failure detail"); };
  enabled.stop = [&] { ++stops; };
  std::string bind_error;
  ASSERT_TRUE(config.BindCapability(kCapabilityId, kContractVersion, kContractDigest,
                                    std::move(enabled), &bind_error))
      << bind_error;
  ASSERT_TRUE(config.BindCapability("test.disabled", kContractVersion,
                                    kDisabledContractDigest, {}, &bind_error))
      << bind_error;

  xgc2::adapter_runtime::ClientCallbacks lifecycle;
  lifecycle.apply_instance_spec = [](const xgc::adapter::v1::AdapterInstanceSpec&,
                                     std::string*) { return true; };
  lifecycle.clear_instance_spec = [&] { ++clears; };
  xgc2::adapter_runtime::Client client(std::move(config), std::move(lifecycle));
  std::string error;
  EXPECT_FALSE(client.Start(&error));
  EXPECT_TRUE(service_->WaitForReadyFailureResult());
  EXPECT_NE(error.find("capability ready callback failed"), std::string::npos) << error;
  EXPECT_EQ(error.find("private native failure detail"), std::string::npos) << error;
  EXPECT_EQ(stops.load(), 1);
  EXPECT_EQ(clears.load(), 1);
  EXPECT_EQ(client.session().state, xgc2::adapter_runtime::ClientState::kStopped);
}

TEST_F(AdapterRuntimeClientTest, BootstrapReadIsNoFollowAndSizeBounded) {
  const std::string symlink_path = bootstrap_path_ + ".symlink";
  const std::string empty_path = bootstrap_path_ + ".empty";
  const std::string oversized_path = bootstrap_path_ + ".oversized";
  std::remove(symlink_path.c_str());
  std::remove(empty_path.c_str());
  std::remove(oversized_path.c_str());

  ASSERT_EQ(::symlink(bootstrap_path_.c_str(), symlink_path.c_str()), 0);
  EXPECT_THROW(xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(symlink_path),
               std::runtime_error);

  { std::ofstream empty(empty_path, std::ios::out | std::ios::binary); }
  ASSERT_EQ(::chmod(empty_path.c_str(), 0600), 0);
  EXPECT_THROW(xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(empty_path),
               std::runtime_error);

  {
    std::ofstream oversized(oversized_path,
                            std::ios::out | std::ios::binary | std::ios::trunc);
    oversized.seekp(4 * 1024 * 1024);
    oversized.put('x');
  }
  ASSERT_EQ(::chmod(oversized_path.c_str(), 0600), 0);
  EXPECT_THROW(xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(oversized_path),
               std::runtime_error);

  std::remove(symlink_path.c_str());
  std::remove(empty_path.c_str());
  std::remove(oversized_path.c_str());
}

TEST_F(AdapterRuntimeClientTest, RejectsOwnedFifoBootstrapWithoutWaitingForWriter) {
  const std::string fifo_path = fixture_directory_ + "/bootstrap.fifo";
  ASSERT_EQ(::mkfifo(fifo_path.c_str(), 0600), 0);
  struct stat metadata {};
  ASSERT_EQ(::lstat(fifo_path.c_str(), &metadata), 0);
  ASSERT_TRUE(S_ISFIFO(metadata.st_mode));
  ASSERT_EQ(metadata.st_mode & 0777, 0600);
  std::promise<bool> result;
  auto completion = result.get_future();
  const auto started = std::chrono::steady_clock::now();
  std::thread reader([&] {
    try {
      (void)xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(fifo_path);
      result.set_value(false);
    } catch (const std::runtime_error&) {
      result.set_value(true);
    } catch (...) {
      result.set_value(false);
    }
  });
  const bool completed_without_writer =
      completion.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
  if (!completed_without_writer) {
    // Rescue only a regressed blocking FIFO open so the native test can report
    // failure and join its actual reader rather than leak a blocked thread.
    const int rescue = ::open(fifo_path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    EXPECT_GE(rescue, 0);
    if (rescue >= 0) ::close(rescue);
  }
  reader.join();
  EXPECT_TRUE(completed_without_writer);
  EXPECT_TRUE(completion.get());
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(500));
  EXPECT_EQ(service_->registration_attempts(), 0);
  ASSERT_EQ(::lstat(fifo_path.c_str(), &metadata), 0);
  EXPECT_TRUE(S_ISFIFO(metadata.st_mode));
}

TEST_F(AdapterRuntimeClientTest, RejectsNonCanonicalRuntimeServiceEndpoints) {
  const auto reject_target = [&](const std::string& runtime_target) {
    bootstrap_.mutable_runtime_service()->mutable_endpoint()->set_address(
        runtime_target);
    std::ofstream output(bootstrap_path_,
                         std::ios::out | std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(bootstrap_.SerializeToOstream(&output));
    output.close();
    ASSERT_EQ(::chmod(bootstrap_path_.c_str(), 0600), 0);

    try {
      (void)xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(bootstrap_path_);
      FAIL() << "untrusted Runtime target was accepted: " << runtime_target;
    } catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find("unix"), std::string::npos)
          << error.what();
    }
  };

  reject_target("dns:///runtime.example:443");
  reject_target("127.0.0.1:50051");
  reject_target("relative/runtime.sock");
  reject_target("/run/xgc2/../runtime.sock");
  reject_target("/run//xgc2/runtime.sock");
}

class PairReconnectTest : public AdapterRuntimeClientTest {
 protected:
  FakeRuntimeLink::Scenario scenario() const override {
    return FakeRuntimeLink::Scenario::kPairReconnects;
  }
};

TEST_F(PairReconnectTest, RegistersOnceAndReplacesThePairAtStrictEpochsTwoAndThree) {
  auto config = xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(bootstrap_path_);
  config.initial_connect_timeout_ms = 5000;
  config.rpc_timeout_ms = 1000;
  config.reconnect_initial_delay_ms = 50;
  config.reconnect_max_delay_ms = 50;
  config.maximum_pair_reconnect_attempts = 1;

  std::atomic<int> starts{0};
  std::atomic<int> readies{0};
  std::atomic<int> stops{0};
  std::atomic<int> applies{0};
  std::atomic<int> clears{0};
  std::atomic<int> session_losses{0};
  std::atomic<int> source_closes{0};
  std::mutex source_mutex;
  std::condition_variable source_condition;
  bool opening_source_started = false;
  bool opening_source_cancelled = false;
  xgc2::adapter_runtime::CapabilityCallbacks enabled;
  enabled.start = [&](const xgc::adapter::v1::AdapterInstanceSpec& spec,
                      const xgc::adapter::v1::EnabledCapability&, std::string*) {
    EXPECT_EQ(spec.revision(), kSpecRevision);
    EXPECT_EQ(spec.spec_digest(), kSpecDigest);
    ++starts;
    return true;
  };
  enabled.ready = [&] { ++readies; };
  enabled.stop = [&] { ++stops; };
  enabled.source_open = [&](const xgc::adapter::v1::SourceOpenRequest&,
                            const xgc2::adapter_runtime::CancellationToken& token) {
    {
      std::lock_guard<std::mutex> lock(source_mutex);
      opening_source_started = true;
    }
    source_condition.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!token.IsCancellationRequested() &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
      std::lock_guard<std::mutex> lock(source_mutex);
      opening_source_cancelled = token.IsCancellationRequested();
    }
    source_condition.notify_all();
    return xgc2::adapter_runtime::SourceOpenDecision::Accept();
  };
  enabled.source_closed = [&](const xgc::adapter::v1::SourceOpenRequest& request,
                              const xgc::adapter::v1::AdapterError& close_error) {
    EXPECT_EQ(request.context().work_id(), "reconnect-source");
    EXPECT_EQ(close_error.code(), "connection-pair-replaced");
    ++source_closes;
  };
  enabled.unary = [](const xgc::adapter::v1::UnaryRequest&,
                     const xgc2::adapter_runtime::CancellationToken&) {
    return xgc2::adapter_runtime::UnaryResult::Success();
  };
  enabled.operation = [](const xgc::adapter::v1::OperationRequest&,
                         const xgc2::adapter_runtime::CancellationToken&) {
    return xgc2::adapter_runtime::OperationResult::Success();
  };
  std::string bind_error;
  ASSERT_TRUE(config.BindCapability(kCapabilityId, kContractVersion, kContractDigest,
                                    std::move(enabled), &bind_error))
      << bind_error;
  ASSERT_TRUE(config.BindCapability("test.disabled", kContractVersion,
                                    kDisabledContractDigest, {}, &bind_error))
      << bind_error;

  xgc2::adapter_runtime::ClientCallbacks callbacks;
  callbacks.apply_instance_spec = [&](const xgc::adapter::v1::AdapterInstanceSpec& spec,
                                      std::string*) {
    EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(
        spec, bootstrap_.initial_spec()));
    ++applies;
    return true;
  };
  callbacks.clear_instance_spec = [&] { ++clears; };
  callbacks.session_lost = [&](const std::string&) { ++session_losses; };

  xgc2::adapter_runtime::Client client(std::move(config), std::move(callbacks));
  std::string error;
  ASSERT_TRUE(client.Start(&error)) << error;
  ASSERT_TRUE(service_->WaitForPairAttachments(1));
  {
    std::unique_lock<std::mutex> lock(source_mutex);
    ASSERT_TRUE(source_condition.wait_for(lock, std::chrono::seconds(5),
                                          [&] { return opening_source_started; }));
  }
  RestartRuntimeServer();
  {
    std::unique_lock<std::mutex> lock(source_mutex);
    ASSERT_TRUE(source_condition.wait_for(lock, std::chrono::seconds(5),
                                          [&] { return opening_source_cancelled; }));
  }
  ASSERT_TRUE(service_->WaitForPairAttachments(2)) << client.session().last_error;
  RestartRuntimeServer();
  ASSERT_TRUE(service_->WaitForPairAttachments(3)) << client.session().last_error;

  const std::vector<std::uint64_t> expected_epochs{1, 2, 3};
  EXPECT_EQ(service_->control_epochs(), expected_epochs);
  EXPECT_EQ(service_->paired_epochs(), expected_epochs);
  EXPECT_EQ(service_->registration_attempts(), 1);
  EXPECT_EQ(service_->registrations(), 1);
  EXPECT_EQ(service_->spec_applications(), 3);
  EXPECT_EQ(applies.load(), 1);
  EXPECT_EQ(starts.load(), 1);
  EXPECT_EQ(readies.load(), 1);
  EXPECT_EQ(stops.load(), 0);
  EXPECT_EQ(clears.load(), 0);
  EXPECT_EQ(source_closes.load(), 1);
  EXPECT_EQ(session_losses.load(), 0);

  const auto snapshot = client.session();
  EXPECT_EQ(snapshot.state, xgc2::adapter_runtime::ClientState::kReady);
  EXPECT_EQ(snapshot.session_id, kSessionId);
  EXPECT_EQ(snapshot.process_generation, kProcessGeneration);
  EXPECT_EQ(snapshot.session_generation, kSessionGeneration);
  EXPECT_EQ(snapshot.runtime_epoch, kRuntimeEpoch);
  EXPECT_EQ(snapshot.spec_revision, kSpecRevision);
  EXPECT_EQ(snapshot.spec_digest, kSpecDigest);

  client.Stop();
  EXPECT_EQ(stops.load(), 1);
  EXPECT_EQ(clears.load(), 1);

  std::string restart_error;
  EXPECT_FALSE(client.Start(&restart_error));
  EXPECT_NE(restart_error.find("single-use"), std::string::npos);
  EXPECT_EQ(service_->registration_attempts(), 1);
}

TEST_F(PairReconnectTest, ExhaustedPairBudgetReportsOneTerminalSessionLoss) {
  auto config = xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(bootstrap_path_);
  config.initial_connect_timeout_ms = 5000;
  config.rpc_timeout_ms = 1000;
  config.reconnect_initial_delay_ms = 50;
  config.reconnect_max_delay_ms = 50;
  config.maximum_pair_reconnect_attempts = 1;

  xgc2::adapter_runtime::CapabilityCallbacks enabled;
  enabled.unary = [](const xgc::adapter::v1::UnaryRequest&,
                     const xgc2::adapter_runtime::CancellationToken&) {
    return xgc2::adapter_runtime::UnaryResult::Success();
  };
  enabled.operation = [](const xgc::adapter::v1::OperationRequest&,
                         const xgc2::adapter_runtime::CancellationToken&) {
    return xgc2::adapter_runtime::OperationResult::Success();
  };
  enabled.source_open = [](const xgc::adapter::v1::SourceOpenRequest&,
                           const xgc2::adapter_runtime::CancellationToken&) {
    return xgc2::adapter_runtime::SourceOpenDecision::Accept();
  };
  std::string bind_error;
  ASSERT_TRUE(config.BindCapability(kCapabilityId, kContractVersion, kContractDigest,
                                    std::move(enabled), &bind_error))
      << bind_error;
  ASSERT_TRUE(config.BindCapability("test.disabled", kContractVersion,
                                    kDisabledContractDigest, {}, &bind_error))
      << bind_error;

  std::mutex loss_mutex;
  std::condition_variable loss_condition;
  int loss_count = 0;
  std::string loss_reason;
  xgc2::adapter_runtime::ClientCallbacks callbacks;
  callbacks.session_lost = [&](const std::string& reason) {
    {
      std::lock_guard<std::mutex> lock(loss_mutex);
      ++loss_count;
      loss_reason = reason;
    }
    loss_condition.notify_all();
  };

  xgc2::adapter_runtime::Client client(std::move(config), std::move(callbacks));
  std::string start_error;
  ASSERT_TRUE(client.Start(&start_error)) << start_error;
  ASSERT_TRUE(service_->WaitForPairAttachments(1));
  StopRuntimeServer();
  {
    std::unique_lock<std::mutex> lock(loss_mutex);
    ASSERT_TRUE(loss_condition.wait_for(lock, std::chrono::seconds(5),
                                        [&] { return loss_count == 1; }));
    EXPECT_NE(loss_reason.find("reconnect budget exhausted"), std::string::npos)
        << loss_reason;
  }

  EXPECT_EQ(service_->control_epochs(), (std::vector<std::uint64_t>{1}));
  EXPECT_EQ(service_->paired_epochs(), (std::vector<std::uint64_t>{1}));
  EXPECT_EQ(service_->registration_attempts(), 1);
  EXPECT_EQ(service_->registrations(), 1);
  EXPECT_EQ(client.session().last_error.find("reconnect budget exhausted") !=
                std::string::npos,
            true)
      << client.session().last_error;
  EXPECT_EQ(client.session().state, xgc2::adapter_runtime::ClientState::kSessionLost);
  client.Stop();
}

class SpecReplacementTest : public AdapterRuntimeClientTest {
 protected:
  FakeRuntimeLink::Scenario scenario() const override {
    return FakeRuntimeLink::Scenario::kSpecReplacement;
  }
};

TEST_F(SpecReplacementTest, OnlyANewSpecRestartsNativeApplicationState) {
  auto config = xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(bootstrap_path_);
  config.initial_connect_timeout_ms = 5000;
  config.rpc_timeout_ms = 1000;

  std::atomic<int> starts{0};
  std::atomic<int> readies{0};
  std::atomic<int> stops{0};
  std::atomic<int> applies{0};
  std::atomic<int> clears{0};
  std::atomic<std::uint64_t> latest_revision{0};

  xgc2::adapter_runtime::CapabilityCallbacks enabled;
  enabled.start = [&](const xgc::adapter::v1::AdapterInstanceSpec& spec,
                      const xgc::adapter::v1::EnabledCapability&, std::string*) {
    latest_revision.store(spec.revision());
    ++starts;
    return true;
  };
  enabled.ready = [&] { ++readies; };
  enabled.stop = [&] { ++stops; };
  enabled.unary = [](const xgc::adapter::v1::UnaryRequest&,
                     const xgc2::adapter_runtime::CancellationToken&) {
    return xgc2::adapter_runtime::UnaryResult::Success();
  };
  enabled.operation = [](const xgc::adapter::v1::OperationRequest&,
                         const xgc2::adapter_runtime::CancellationToken&) {
    return xgc2::adapter_runtime::OperationResult::Success();
  };
  enabled.source_open = [](const xgc::adapter::v1::SourceOpenRequest&,
                           const xgc2::adapter_runtime::CancellationToken&) {
    return xgc2::adapter_runtime::SourceOpenDecision::Accept();
  };
  std::string bind_error;
  ASSERT_TRUE(config.BindCapability(kCapabilityId, kContractVersion, kContractDigest,
                                    std::move(enabled), &bind_error))
      << bind_error;
  ASSERT_TRUE(config.BindCapability("test.disabled", kContractVersion,
                                    kDisabledContractDigest, {}, &bind_error))
      << bind_error;

  xgc2::adapter_runtime::ClientCallbacks callbacks;
  callbacks.apply_instance_spec = [&](const xgc::adapter::v1::AdapterInstanceSpec& spec,
                                      std::string*) {
    latest_revision.store(spec.revision());
    ++applies;
    return true;
  };
  callbacks.clear_instance_spec = [&] { ++clears; };

  xgc2::adapter_runtime::Client client(std::move(config), std::move(callbacks));
  std::string error;
  ASSERT_TRUE(client.Start(&error)) << error;
  ASSERT_TRUE(service_->WaitForSpecApplications(2));

  EXPECT_EQ(service_->registration_attempts(), 1);
  EXPECT_EQ(service_->registrations(), 1);
  EXPECT_EQ(service_->control_epochs(), (std::vector<std::uint64_t>{1}));
  EXPECT_EQ(applies.load(), 2);
  EXPECT_EQ(starts.load(), 2);
  EXPECT_EQ(readies.load(), 2);
  EXPECT_EQ(stops.load(), 1);
  EXPECT_EQ(clears.load(), 1);
  EXPECT_EQ(latest_revision.load(), kSpecRevision + 1);
  EXPECT_EQ(client.session().spec_revision, kSpecRevision + 1);
  EXPECT_EQ(client.session().spec_digest, kReplacementSpecDigest);

  client.Stop();
  EXPECT_EQ(stops.load(), 2);
  EXPECT_EQ(clears.load(), 2);
}

TEST_F(AdapterRuntimeClientTest, TypedBootstrapBindsFreshRandomHexServiceInstance) {
  const auto config =
      xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(bootstrap_path_);
  const auto& reference = config.runtime_service();
  ASSERT_EQ(reference.instance_id().size(), 32U);
  EXPECT_TRUE(std::all_of(
      reference.instance_id().begin(), reference.instance_id().end(), [](char digit) {
        return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
      }));
  EXPECT_EQ(reference.instance_id(), admission_->instance_id());
  EXPECT_EQ(reference.endpoint().address(), socket_path_);
  EXPECT_EQ(reference.endpoint().kind(), "unix");
  EXPECT_EQ(reference.profile(), "grpc.v1");
  EXPECT_NE(TestBootstrap("unix:" + socket_path_).runtime_service().instance_id(),
            reference.instance_id());
  struct stat metadata {};
  ASSERT_EQ(::stat(fixture_directory_.c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & 0777, 0700);
}

TEST_F(AdapterRuntimeClientTest,
       StaleServiceInstanceCannotConsumeBootstrapOrReachDomain) {
  auto stale = bootstrap_;
  stale.mutable_runtime_service()->set_instance_id(xgc2::xrpc::new_instance_id());
  ASSERT_NE(stale.runtime_service().instance_id(), admission_->instance_id());
  std::ofstream output(bootstrap_path_,
                       std::ios::out | std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(stale.SerializeToOstream(&output));
  output.close();
  ASSERT_EQ(::chmod(bootstrap_path_.c_str(), 0600), 0);
  auto config = xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(bootstrap_path_);
  config.initial_connect_timeout_ms = 1000;
  BindProtocolFixture(&config);
  xgc2::adapter_runtime::Client client(std::move(config), {});
  std::string error;
  EXPECT_FALSE(client.Start(&error));
  EXPECT_NE(error.find("instance"), std::string::npos) << error;
  EXPECT_EQ(service_->registration_attempts(), 0);
  EXPECT_EQ(service_->registrations(), 0);
  EXPECT_TRUE(service_->control_epochs().empty());
  EXPECT_TRUE(service_->paired_epochs().empty());
  EXPECT_GE(server_->stats().rejected_calls, 1U);
  client.Stop();
}

TEST_F(AdapterRuntimeClientTest,
       UnsupportedUnknownAndOverCeilingClientPolicyNeverRegisters) {
  auto config = xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(bootstrap_path_);
  for (const auto& invalid : std::vector<std::pair<std::string, std::string>>{
           {"XGC2_XRPC_UNKNOWN", "1"},
           {"XGC2_XRPC_CALL_TIMEOUT_MS", "30001"},
           {"XGC2_XRPC_HOST_MAX_IN_FLIGHT", "1"}}) {
    EXPECT_THROW(config.ApplyXrpcEnvironment({invalid}),
                 xgc2::xrpc::RuntimePolicyError);
  }
  EXPECT_EQ(service_->registration_attempts(), 0);
  EXPECT_EQ(server_->stats().admitted_calls, 0U);
}

class FinitePairRenewalTest : public AdapterRuntimeClientTest {
 protected:
  FakeRuntimeLink::Scenario scenario() const override {
    return FakeRuntimeLink::Scenario::kFinitePairRenewals;
  }
};

TEST_F(FinitePairRenewalTest,
       SharedFinitePolicyRenewsPairsWithoutRegisterOrMutationReplay) {
  using namespace std::chrono;
  const auto budget = milliseconds(450);
  auto config = xgc2::adapter_runtime::ClientConfig::FromBootstrapFile(bootstrap_path_);
  config.ApplyXrpcEnvironment({{"XGC2_XRPC_CALL_TIMEOUT_MS", "450"},
                               {"XGC2_XRPC_MAX_REQUEST_BYTES", "1048576"}});
  bool observed_budget = false, observed_bytes = false;
  for (const auto& field : config.xrpc_runtime_policy().fields()) {
    if (field.name() == "CALL_TIMEOUT_MS")
      observed_budget = field.integer_value() == budget.count() &&
                        field.source() == "environment" && field.has_ceiling() &&
                        field.ceiling() == 30000;
    if (field.name() == "MAX_REQUEST_BYTES")
      observed_bytes =
          field.integer_value() == 1048576 && field.source() == "environment";
  }
  ASSERT_TRUE(observed_budget && observed_bytes);
  EXPECT_GT(config.xrpc_runtime_policy().revision(), 0U);
  config.rpc_timeout_ms = 5000;  // Transport policy remains the tighter budget.
  config.initial_connect_timeout_ms = 2000;
  config.reconnect_initial_delay_ms = 5;
  config.reconnect_max_delay_ms = 5;
  config.maximum_pair_reconnect_attempts = 1;
  std::atomic<int> native_mutations{0}, starts{0}, readies{0}, stops{0}, applies{0},
      clears{0};
  auto enabled = ProtocolFixtureCallbacks();
  enabled.operation = [&](const xgc::adapter::v1::OperationRequest& request,
                          const xgc2::adapter_runtime::CancellationToken&) {
    EXPECT_EQ(request.context().work_id(), "renewal-operation");
    ++native_mutations;
    return xgc2::adapter_runtime::OperationResult::Success();
  };
  enabled.start = [&](const xgc::adapter::v1::AdapterInstanceSpec&,
                      const xgc::adapter::v1::EnabledCapability&, std::string*) {
    ++starts;
    return true;
  };
  enabled.ready = [&] { ++readies; };
  enabled.stop = [&] { ++stops; };
  std::string bind_error;
  ASSERT_TRUE(config.BindCapability(kCapabilityId, kContractVersion, kContractDigest,
                                    std::move(enabled), &bind_error))
      << bind_error;
  ASSERT_TRUE(config.BindCapability("test.disabled", kContractVersion,
                                    kDisabledContractDigest, {}, &bind_error))
      << bind_error;
  xgc2::adapter_runtime::ClientCallbacks callbacks;
  callbacks.apply_instance_spec = [&](const xgc::adapter::v1::AdapterInstanceSpec&,
                                      std::string*) {
    ++applies;
    return true;
  };
  callbacks.clear_instance_spec = [&] { ++clears; };
  xgc2::adapter_runtime::Client client(std::move(config), std::move(callbacks));
  std::string error;
  ASSERT_TRUE(client.Start(&error)) << error;
  ASSERT_TRUE(service_->WaitForRenewedOperation());
  // No listener restart, failure injection, or new Register: the only cause
  // of these replacements is the native transport's finite pair deadline.
  ASSERT_TRUE(service_->WaitForPairAttachments(3));
  EXPECT_EQ(service_->registration_attempts(), 1);
  EXPECT_EQ(service_->registrations(), 1);
  EXPECT_EQ(service_->renewal_operation_requests(), 1);
  EXPECT_EQ(native_mutations.load(), 1);
  EXPECT_EQ(starts.load(), 1);
  EXPECT_EQ(readies.load(), 1);
  EXPECT_EQ(stops.load(), 0);
  EXPECT_EQ(applies.load(), 1);
  EXPECT_EQ(clears.load(), 0);
  const auto control = service_->control_deadlines();
  const auto work = service_->work_deadlines();
  const auto registrations = service_->register_deadlines();
  ASSERT_EQ(registrations.size(), 1U);
  ASSERT_GE(control.size(), 3U);
  ASSERT_GE(work.size(), 3U);
  // Native gRPC rounds its encoded timeout up to the next millisecond.
  for (const auto& call : registrations) {
    EXPECT_NE(call.deadline, system_clock::time_point::max());
    EXPECT_GT(call.deadline, call.admitted_at);
    EXPECT_LE(call.deadline - call.admitted_at, budget + milliseconds(1));
  }
  for (std::size_t pair = 0; pair != 3; ++pair) {
    for (const auto& call : {control[pair], work[pair]}) {
      EXPECT_NE(call.deadline, system_clock::time_point::max());
      EXPECT_GT(call.deadline, call.admitted_at);
      EXPECT_LE(call.deadline - call.admitted_at, budget + milliseconds(1));
    }
    const auto difference = control[pair].deadline > work[pair].deadline
                                ? control[pair].deadline - work[pair].deadline
                                : work[pair].deadline - control[pair].deadline;
    EXPECT_LE(difference, milliseconds(8));  // Native gRPC millisecond rounding.
    if (pair) {
      EXPECT_GT(control[pair].deadline, control[pair - 1].deadline);
    }
  }
  EXPECT_EQ(client.session().state, xgc2::adapter_runtime::ClientState::kReady);
  client.Stop();
  EXPECT_EQ(stops.load(), 1);
  EXPECT_EQ(clears.load(), 1);
  EXPECT_EQ(native_mutations.load(), 1);
}

TEST_F(AdapterRuntimeClientTest, SdkFixturePreservesBusyEndpointAndForeignFile) {
  struct stat owned_before {};
  ASSERT_EQ(::lstat(socket_path_.c_str(), &owned_before), 0);
  xgc2::xrpc::UnixOptions duplicate;
  duplicate.path = socket_path_;
  xgc2::xrpc::GrpcAdmission competitor(xgc2::xrpc::new_instance_id());
  EXPECT_THROW((xgc2::xrpc::GrpcUnixServer(
                   duplicate, competitor, std::vector<grpc::Service*>{service_.get()})),
               std::exception);
  struct stat owned_after {};
  ASSERT_EQ(::lstat(socket_path_.c_str(), &owned_after), 0);
  EXPECT_EQ(owned_before.st_dev, owned_after.st_dev);
  EXPECT_EQ(owned_before.st_ino, owned_after.st_ino);

  const auto foreign_path = fixture_directory_ + "/foreign.sock";
  {
    std::ofstream foreign(foreign_path);
    foreign << "foreign-native-owner";
  }
  struct stat foreign_before {};
  ASSERT_EQ(::lstat(foreign_path.c_str(), &foreign_before), 0);
  xgc2::xrpc::UnixOptions forbidden;
  forbidden.path = foreign_path;
  FakeRuntimeLink isolated(bootstrap_.initial_spec());
  isolated.UseAdmission(
      std::make_shared<xgc2::xrpc::GrpcAdmission>(competitor.instance_id()));
  EXPECT_THROW((xgc2::xrpc::GrpcUnixServer(forbidden, competitor,
                                           std::vector<grpc::Service*>{&isolated})),
               std::exception);
  struct stat foreign_after {};
  ASSERT_EQ(::lstat(foreign_path.c_str(), &foreign_after), 0);
  EXPECT_EQ(foreign_before.st_dev, foreign_after.st_dev);
  EXPECT_EQ(foreign_before.st_ino, foreign_after.st_ino);
  std::string contents;
  {
    std::ifstream foreign(foreign_path);
    std::getline(foreign, contents);
  }
  EXPECT_EQ(contents, "foreign-native-owner");
}
