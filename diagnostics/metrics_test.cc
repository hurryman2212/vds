#include <cmath>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include "vds_protocol.hh"

int main() {
  const int amplitudes[] = {0, 1, 127, 128, 255, 256, 384, 512, 8192, 32767};
  const int positive[] = {0, 0, 0, 1, 1, 1, 2, 2, 32, 127};
  const int negative[] = {0, 0, 0, -1, -1, -1, -2, -2, -32, -128};
  for (int test = 0; test < 10; ++test) {
    const int amplitude = amplitudes[test];
    vds::PcmAudioExtractor extractor;
    std::vector<std::uint8_t> pcm(512 * 8);
    for (int frame = 0; frame < 512; ++frame) {
      for (int ch = 2; ch < 4; ++ch) {
        const auto value = static_cast<std::uint16_t>(ch == 2 ? amplitude : -amplitude);
        pcm[frame*8+ch*2] = value & 255;
        pcm[frame*8+ch*2+1] = value >> 8;
      }
    }
    // Split within a sample to exercise the existing partial-frame path.
    auto first = extractor.push_usb_audio(std::span(pcm).first(3));
    auto chunks = extractor.push_usb_audio(std::span(pcm).subspan(3));
    if (!first.empty() || chunks.size() != 1) throw std::runtime_error("chunk boundaries");
    const auto &c = chunks[0];
    for (int i = 0; i < 64; ++i) {
      const int expected = i % 2 ? negative[test] : positive[test];
      if (c.haptics[i] != expected) throw std::runtime_error("incorrect rounded output");
    }
    if (c.diagnostic_raw_count != 1024 || c.diagnostic_raw_peak != amplitude ||
        c.diagnostic_raw_energy != double(amplitude)*amplitude*1024 ||
        c.diagnostic_average_energy != c.diagnostic_raw_energy ||
        c.diagnostic_quantized_energy != (double(positive[test])*positive[test] + double(negative[test])*negative[test])*65536*512 ||
        c.diagnostic_erased_averages != (amplitude > 0 && amplitude < 128 ? 64 : 0))
      throw std::runtime_error("incorrect diagnostic metrics");
  }
  for (int gain : {0, 100, 150, 300}) {
    vds::PcmAudioExtractor extractor;
    extractor.set_haptics_gain(gain);
    std::vector<std::uint8_t> pcm(512*8);
    for (int i=0;i<512;++i) for (int ch=2;ch<4;++ch) {
      const auto value=static_cast<std::uint16_t>(ch==2 ? 16384 : -16384);
      pcm[i*8+ch*2]=value&255; pcm[i*8+ch*2+1]=value>>8;
    }
    const auto chunks=extractor.push_usb_audio(pcm);
    const int magnitude=64*gain/100;
    if (chunks.size()!=1 || chunks[0].haptics[0]!=std::min(127,magnitude) ||
        chunks[0].haptics[1]!=std::max(-128,-magnitude) ||
        chunks[0].diagnostic_clipped_samples!=(gain==300 ? 64 : 0))
      throw std::runtime_error("haptics gain/clipping");
    std::vector<std::uint8_t> report(78,0);
    report[0]=0x31; report[3]=3; report[5]=100; report[6]=200; report[14]=23;
    vds::amplify_rumble_report(report,gain);
    if (report[5]!=std::min(255,gain) || report[6]!=std::min(255,2*gain) || report[14]!=23)
      throw std::runtime_error("rumble gain or unrelated state modified");
  }
  vds::DsOutputState state;
  std::array<std::uint8_t, 48> report{};
  report[0] = 2;
  report[2] = 0x60; // Allow motor power + haptic low-pass updates.
  report[37] = 0xa5; // rumble=5, triggers=10.
  report[40] = 1;
  if (!state.apply_usb_output_report(report) || state.state()[36] != 0xa5 ||
      (state.state()[39] & 1) != 1) throw std::runtime_error("power/filter forwarding");
  vds_set_state_data decoded{};
  std::memcpy(&decoded, report.data()+1, sizeof(decoded));
  if (decoded.rumble_motor_power_reduction != 5 || decoded.trigger_motor_power_reduction != 10)
    throw std::runtime_error("motor nibble layout");
  report[2] = 0; report[37] = 0; report[40] = 0;
  state.apply_usb_output_report(report);
  if (state.state()[36] != 0xa5 || (state.state()[39] & 1) != 1)
    throw std::runtime_error("unflagged update overwrote state");
  report[2] = 0x60;
  state.apply_usb_output_report(report);
  if (state.state()[36] != 0 || (state.state()[39] & 1) != 0)
    throw std::runtime_error("cannot clear power/filter state");
  report[2] = 0; report[39] = 4; report[3] = 73; report[4] = 111;
  state.apply_usb_output_report(report);
  if (state.state()[2] != 73 || state.state()[3] != 111)
    throw std::runtime_error("improved-rumble magnitudes not forwarded");
  vds::HapticsPacketBuilder builder;
  vds::HapticsChunk haptics{};
  vds::SpeakerChunk speaker{};
  std::uint8_t sequence = 0;
  for (int i = 0; i < 40; ++i) {
    auto audio = builder.build_packet(haptics, speaker, state.state(), true, false);
    auto control = state.build_bt_state_report();
    vds::sequence_output_report(control, sequence);
    vds::sequence_output_report(audio, sequence);
    if ((control[1] >> 4) != ((2*i)&15) || (audio[1] >> 4) != ((2*i+1)&15))
      throw std::runtime_error("shared output sequence/wrap");
    auto verified = audio;
    vds::fill_output_report_checksum(verified);
    if (verified != audio || audio[13+36] != state.state()[36])
      throw std::runtime_error("resequence corrupts checksum/payload");
  }
  std::cout << "PASS: rounding boundaries, signed saturation, energy metrics, fragmented input, power/filter flags, improved rumble, shared sequence wrap and CRC\n";
}
