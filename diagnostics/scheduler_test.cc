// Exercise the real daemon scheduler with a fake transport: no device access.
#define main vds_daemon_main
#include "../src/platform/win32/vdsd.cc"
#undef main
#include <functional>

class FakeBluetooth final : public BluetoothTransport {
public:
  int sync_writes = 0, async_writes = 0;
  bool fail = false;
  bool fatal = false, cancelled = false;
  std::function<void()> on_write;
  Frame read_frame() override { return {}; }
  void write_feature_report(std::span<const std::uint8_t>) override {}
  void write_interrupt_packet(std::span<const std::uint8_t>) override {
    if (fatal) throw vds::win::BluetoothTransportError("injected disconnect");
    if (fail) throw std::runtime_error("injected write failure");
    ++sync_writes;
    if (on_write) on_write();
  }
  bool try_write_interrupt_packet(std::span<const std::uint8_t>) override {
    if (fatal) throw vds::win::BluetoothTransportError("injected disconnect");
    ++async_writes;
    if (on_write) on_write();
    return !fail;
  }
  void cancel() override { cancelled = true; }
  std::string description() const override { return "test"; }
};

void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}

int main() {
  vds::Logger logger("diagnostics/scheduler-test.log");
  std::mutex output_mutex;
  {
    BridgeState state;
    FakeBluetooth bt;
    bt.fail = true;
    send_bt_state_if_changed(bt, state, logger, output_mutex);
    require(!state.have_last_sent_state, "failed state was cached");
    bt.fail = false;
    const auto snapshot = state.output_state.state();
    bt.on_write = [&] {
      std::array<std::uint8_t,48> update{};
      update[0]=2; update[2]=4; update[45]=19;
      state.output_state.apply_usb_output_report(update);
    };
    send_bt_state_if_changed(bt, state, logger, output_mutex);
    require(state.last_sent_state == snapshot, "cached live state instead of transmitted snapshot");
    bt.on_write = {};
    send_bt_state_if_changed(bt, state, logger, output_mutex);
    require(bt.sync_writes == 2 && state.last_sent_state == state.output_state.state(), "new state was lost");
    send_bt_state_if_changed(bt, state, logger, output_mutex);
    require(bt.sync_writes == 2, "redundant state write");
  }
  {
    BridgeState state;
    FakeBluetooth bt;
    state.audio_out_stream_active = state.audio_pcm_stream_active = true;
    state.audio_jitter_buffering = true;
    state.audio_jitter_buffer_start = Clock::now();
    auto chunk = state.silent_audio_chunk;
    chunk.queued_at = Clock::now();
    state.pending_audio_chunks.push_back(chunk);
    flush_pending_audio_chunk(bt, state, logger, output_mutex);
    require(bt.async_writes == 1 && state.pending_audio_chunks.empty(), "realtime startup waited for a buffer");
    require(!state.have_last_sent_state, "async enqueue falsely marked state delivered");
  }
  {
    BridgeState state;
    FakeBluetooth bt;
    state.audio_out_stream_active = state.audio_pcm_stream_active = true;
    auto stale = state.silent_audio_chunk;
    state.audio_jitter_buffering = false;
    stale.queued_at = Clock::now() - std::chrono::milliseconds(100);
    state.pending_audio_chunks.push_back(stale);
    auto fresh = stale;
    fresh.queued_at = Clock::now();
    state.pending_audio_chunks.push_back(fresh);
    bt.on_write = [&] {
      std::lock_guard guard(state.mutex);
      state.pending_audio_chunks.clear();
      state.pending_audio_chunks.push_back(fresh);
    };
    flush_pending_audio_chunk(bt, state, logger, output_mutex);
    require(state.audio_stale_dropped_count == 1, "stale audio not discarded");
    require(state.pending_audio_chunks.size() == 1, "sender removed a producer's new chunk");
  }
  {
    BridgeState state;
    FakeBluetooth bt;
    state.audio_out_stream_active = state.audio_pcm_stream_active = true;
    auto chunk = state.silent_audio_chunk;
    chunk.queued_at = Clock::now();
    for (int i=0; i<12; ++i) state.pending_audio_chunks.push_back(chunk);
    bt.fail = true;
    flush_pending_audio_chunk(bt, state, logger, output_mutex);
    require(state.pending_audio_chunks.size() == 1 && state.audio_stale_dropped_count == 10,
            "freshness cap or chunk ownership incorrect");
    require(state.audio_blocked_count == 1 && state.audio_dropped_count == 11,
            "failed audio enqueue accounting incorrect");
  }
  {
    BridgeState state;
    FakeBluetooth bt;
    state.audio_out_stream_active = state.audio_pcm_stream_active = true;
    state.audio_jitter_buffering = false;
    state.next_haptics_send_time = Clock::now()+std::chrono::seconds(1);
    flush_pending_audio_chunk(bt,state,logger,output_mutex);
    require(!state.audio_jitter_buffering && bt.async_writes==0,"empty queue reset valid cadence");
    auto chunk=state.silent_audio_chunk; chunk.queued_at=Clock::now();
    state.pending_audio_chunks.push_back(chunk);
    flush_pending_audio_chunk(bt,state,logger,output_mutex);
    require(bt.async_writes==0,"next packet sent ahead of cadence");
    state.next_haptics_send_time={}; bt.fatal=true;
    bool propagated=false;
    try { flush_pending_audio_chunk(bt,state,logger,output_mutex); }
    catch(const vds::win::BluetoothTransportError &) { propagated=true; }
    require(propagated,"audio disconnect was swallowed");
  }
  {
    vds::win::AdaptivePlayout policy;
    auto now = Clock::time_point(std::chrono::seconds(1));
    policy.observe(now);
    require(policy.target().count()==0, "nonzero fixed startup target");
    now += std::chrono::milliseconds(52); policy.observe(now);
    const auto learned = policy.target();
    require(learned >= std::chrono::milliseconds(40) && learned <= std::chrono::milliseconds(60), "did not learn USB jitter");
    for (int i=0;i<700;++i) { now += std::chrono::milliseconds(10); policy.observe(now); }
    require(policy.target() < learned/2, "latency did not decay under stable delivery");
    now += std::chrono::milliseconds(200); policy.observe(now);
    require(policy.target() <= policy.ceiling, "unbounded jitter latency");
    now += std::chrono::seconds(1); policy.observe(now);
    require(policy.target().count()==0, "long interruption retained old timing");
  }
  {
    BridgeState state;
    FakeBluetooth bt;
    bt.fatal = true;
    bool propagated = false;
    try { send_bt_state_if_changed(bt,state,logger,output_mutex); }
    catch(const vds::win::BluetoothTransportError &) { propagated=true; }
    require(propagated, "disconnect was swallowed");
    std::atomic_bool restart=false;
    request_bridge_restart(INVALID_HANDLE_VALUE,&bt,restart);
    require(restart && bt.cancelled,"reconnect cancellation not signaled");
    BridgeState recovered;
    require(recovered.pending_audio_chunks.empty() && recovered.playout.target().count()==0,
            "new connection replayed stale data/timing");
  }
  {
    const std::string db = "diagnostics/tuning-test.db";
    bool reload=false; std::uint32_t trace=0;
    std::vector<std::unique_ptr<BridgeWorker>> workers;
    auto reply = handle_supervisor_control_command("{\"command\":\"gain\",\"value\":\"150\"}",workers,db,trace,reload,logger);
    require(reply.find("\"OK\":true")!=std::string::npos && vds::win::tuning::gain_percent==150,"gain command failed");
    vds::win::tuning::gain_percent=100;
    load_gain(db,logger);
    require(vds::win::tuning::gain_percent==150,"gain did not survive reload");
    reply=handle_supervisor_control_command("{\"command\":\"gain\",\"value\":\"301\"}",workers,db,trace,reload,logger);
    require(reply.find("\"OK\":false")!=std::string::npos && vds::win::tuning::gain_percent==150,"invalid gain mutated state");
    std::filesystem::remove(tuning_path(db));
    vds::win::tuning::gain_percent=100;
  }
  std::cout << "PASS: state retry/races, immediate startup, adaptive jitter growth/decay, stale eviction, queue ownership, disconnect propagation, reconnect reset, gain command validation and persistence\n";
}
