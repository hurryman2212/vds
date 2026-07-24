// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
// Copyright (C) 2026 Antonio <bleader93@gmail.com>
//
// Partly influenced by DS5Dongle source:
// Copyright (c) 2026 awalol, released under the MIT license.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <opus/opus.h>

#include "uapi/vds.h"
#include "vds_protocol.hh"

namespace vds {

namespace {

constexpr std::uint32_t kOutputCrcSeed = 0xeada2d49;
constexpr std::uint32_t kFeatureCrcSeed = 0x2060efc3;
constexpr std::size_t kPcmChannels = VDS_AUDIO_CHANNELS;
constexpr std::size_t kPcmSampleSize = sizeof(std::int16_t);
constexpr std::size_t kPcmFrameSize = kPcmChannels * kPcmSampleSize;
constexpr std::uint32_t kHapticsDecimation =
    VDS_AUDIO_SAMPLE_RATE / VDS_HAPTICS_SAMPLE_RATE;
constexpr std::size_t kSetStateSize = VDS_SET_STATE_SIZE;
constexpr std::size_t kBtStateOffset = 3;
constexpr std::size_t kSpeakerBlockOffset = 142;
constexpr std::size_t kSpeakerDataOffset = 144;
constexpr std::uint8_t kSpeakerBlockControllerSpeaker = 0x13;
constexpr std::uint8_t kSpeakerBlockHeadphones = 0x16;
constexpr int kSpeakerOpusBitrate = kSpeakerOpusSize * 8 * 100;
constexpr std::uint8_t kBtAudioBufferLength = 64;
constexpr std::size_t kOutputFlag0Offset = 0;
constexpr std::size_t kOutputFlag1Offset = 1;
constexpr std::size_t kOutputHeadphoneVolumeOffset = 4;
constexpr std::size_t kOutputSpeakerVolumeOffset = 5;
constexpr std::size_t kOutputMicVolumeOffset = 6;
constexpr std::size_t kOutputAudioControlOffset = 7;
constexpr std::size_t kOutputMuteLedOffset = 8;
constexpr std::size_t kOutputPowerSaveControlOffset = 9;
constexpr std::size_t kOutputAudioControl2Offset = 37;
constexpr std::size_t kOutputLightBrightnessOffset =
    offsetof(vds_set_state_data, light_brightness);
constexpr std::size_t kOutputLedColorOffset =
    offsetof(vds_set_state_data, led_red);
constexpr std::uint8_t kBtHidpInputPrefix = 0xa1;
constexpr std::uint8_t kBtHidpOutputPrefix = VDS_BT_OUTPUT_PREFIX;
constexpr std::uint8_t kBtHidpFeaturePrefix = 0xa3;
constexpr std::uint8_t kBtHidpGetFeaturePrefix = 0x43;
constexpr std::uint8_t kBtHidpSetFeaturePrefix = 0x53;
constexpr std::size_t kBtInputReportIdOffset = 1;
constexpr std::size_t kBtInputHeaderOffset = 2;
constexpr std::size_t kBtInputUsbPayloadOffset = 3;
constexpr std::size_t kBtInputMinimumSize =
    kBtInputUsbPayloadOffset + VDS_USB_INPUT_PAYLOAD_SIZE;
constexpr std::size_t kBtMicOpusOffset = 4;
constexpr std::uint8_t kBtInputPayloadTypeMask = 0x0f;
constexpr std::uint8_t kBtInputPayloadTypeControl = 0x01;
constexpr std::uint8_t kBtInputPayloadTypeAudio = 0x02;
constexpr std::uint8_t kOutputFlag0HeadphoneVolumeEnable = 0x10;
constexpr std::uint8_t kOutputFlag0SpeakerVolumeEnable = 0x20;
constexpr std::uint8_t kOutputFlag0MicVolumeEnable = 0x40;
constexpr std::uint8_t kOutputFlag0AudioControlEnable = 0x80;
constexpr std::uint8_t kOutputFlag1MicMuteLedControlEnable = 0x01;
constexpr std::uint8_t kOutputFlag1PowerSaveControlEnable = 0x02;
constexpr std::uint8_t kOutputFlag1AudioControl2Enable = 0x80;
constexpr std::uint8_t kOutputMicSelectMask = 0x03;
constexpr std::uint8_t kOutputMicSelectAuto = 0x00;
constexpr std::uint8_t kOutputMicSelectInternal = 0x01;
constexpr std::uint8_t kOutputMicAudioControl = 0x09;
constexpr std::uint8_t kOutputMicVolumeDefault = 0x08;
constexpr std::uint8_t kOutputAudioControl2Default = 0x01;
constexpr std::uint8_t kOutputAudioControlNonPathMask = 0xcf;
constexpr std::uint8_t kOutputPathHeadphones = 0x00;
constexpr std::uint8_t kOutputPathSpeaker = 0x30;
constexpr std::uint8_t kOutputPathMask = 0x30;
constexpr std::uint8_t kOutputPowerSaveMicMute = 0x10;
constexpr std::uint8_t kBtAudioSectionsEnable = 0xff;
constexpr std::uint8_t kBtAudioSectionsDisableMic = 0xfe;
constexpr std::uint8_t kBtMicReportId = 0x32;
constexpr std::uint8_t kBtMicOpen = 0xff;
constexpr std::uint8_t kBtMicClose = 0xfe;
constexpr std::array<std::uint8_t, kSetStateSize> kInitialSetStateData{
    0xfd,
    0xf7,
    0x00,
    0x00,
    0x7f,
    0x64,
    kOutputMicVolumeDefault,
    kOutputMicAudioControl,
    0x00,
    0x0f,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    kOutputAudioControl2Default,
    0x07,
    0x00,
    0x00,
    0x02,
    0x01,
    0x00,
    0xff,
    0xd7,
    0x00};

constexpr DsState initial_ds_state() {
  DsState state{};
  for (std::size_t i = 0; i < kInitialSetStateData.size(); ++i) {
    state[i] = kInitialSetStateData[i];
  }
  return state;
}

constexpr DsState kInitialDsState = initial_ds_state();
static_assert(kHapticsSampleSize % kSpeakerChannels == 0);

constexpr std::uint32_t crc32_table_entry(std::uint32_t index) {
  for (unsigned bit = 0; bit < 8; ++bit) {
    index = (index >> 1) ^ (0xedb88320u & (0u - (index & 1u)));
  }
  return index;
}

constexpr auto make_crc32_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < table.size(); ++i) {
    table[i] = crc32_table_entry(i);
  }
  return table;
}

constexpr auto kCrc32Table = make_crc32_table();

std::int8_t s16_to_s8_haptic(std::int16_t sample) {
  const int value = std::clamp(static_cast<int>(sample / 256), -128, 127);
  return static_cast<std::int8_t>(value);
}

std::int16_t read_le_s16(const std::uint8_t *data) {
  const auto lo = static_cast<std::uint16_t>(data[0]);
  const auto hi = static_cast<std::uint16_t>(data[1]) << 8;
  return static_cast<std::int16_t>(lo | hi);
}

std::uint8_t audio_control_with_output_path(std::uint8_t audio_control,
                                            std::uint8_t output_path) {
  return static_cast<std::uint8_t>(
      (audio_control & static_cast<std::uint8_t>(~kOutputPathMask)) |
      output_path);
}

std::int16_t lerp_i16(std::int16_t left, std::int16_t right,
                      std::uint32_t numerator, std::uint32_t denominator) {
  const auto left_part = static_cast<std::int32_t>(left) *
                         static_cast<std::int32_t>(denominator - numerator);
  const auto right_part =
      static_cast<std::int32_t>(right) * static_cast<std::int32_t>(numerator);
  const auto mixed = static_cast<int>((left_part + right_part) /
                                      static_cast<std::int32_t>(denominator));
  return static_cast<std::int16_t>(std::clamp(mixed, -32768, 32767));
}

std::array<opus_int16, kSpeakerOpusFrames * kSpeakerChannels>
resample_speaker_pcm(std::span<const std::int16_t> pcm,
                     std::size_t pcm_frames) {
  std::array<opus_int16, kSpeakerOpusFrames * kSpeakerChannels> opus_pcm{};

  // USB audio is accumulated in platform-sized speaker windows, then resampled
  // one 10 ms, 480-frame Opus speaker block for the 0x36 packet.
  for (std::size_t frame = 0; frame < kSpeakerOpusFrames; ++frame) {
    const std::size_t source_numerator = frame * pcm_frames;
    const std::size_t source_frame = source_numerator / kSpeakerOpusFrames;
    const auto fraction =
        static_cast<std::uint32_t>(source_numerator % kSpeakerOpusFrames);
    const std::size_t next_frame = std::min(source_frame + 1, pcm_frames - 1);

    for (std::size_t channel = 0; channel < kSpeakerChannels; ++channel) {
      opus_pcm[frame * kSpeakerChannels + channel] =
          lerp_i16(pcm[source_frame * kSpeakerChannels + channel],
                   pcm[next_frame * kSpeakerChannels + channel], fraction,
                   kSpeakerOpusFrames);
    }
  }

  return opus_pcm;
}

HapticsChunk resample_haptics_pcm(std::span<const std::int16_t> pcm,
                                  std::size_t pcm_frames) {
  HapticsChunk chunk{};
  constexpr std::size_t haptics_frames = kHapticsSampleSize / kSpeakerChannels;

  for (std::size_t frame = 0; frame < haptics_frames; ++frame) {
    const std::size_t begin = frame * pcm_frames / haptics_frames;
    const std::size_t end = (frame + 1) * pcm_frames / haptics_frames;
    const std::size_t count = std::max<std::size_t>(1, end - begin);
    std::int32_t left_sum = 0;
    std::int32_t right_sum = 0;
    for (std::size_t sample = begin; sample < end; ++sample) {
      left_sum += pcm[sample * kSpeakerChannels + 0];
      right_sum += pcm[sample * kSpeakerChannels + 1];
    }

    const auto left =
        static_cast<std::int16_t>(left_sum / static_cast<std::int32_t>(count));
    const auto right =
        static_cast<std::int16_t>(right_sum / static_cast<std::int32_t>(count));
    chunk[frame * kSpeakerChannels + 0] = s16_to_s8_haptic(left);
    chunk[frame * kSpeakerChannels + 1] = s16_to_s8_haptic(right);
  }

  return chunk;
}

void opus_ctl_checked(int result, const char *name) {
  if (result != OPUS_OK) {
    throw std::runtime_error(std::string(name) +
                             " failed: " + opus_strerror(result));
  }
}

std::uint32_t crc32_seeded(std::span<const std::uint8_t> data,
                           std::uint32_t seed) {
  std::uint32_t crc = ~seed;

  for (const std::uint8_t byte : data) {
    crc = (crc >> 8) ^ kCrc32Table[(crc ^ byte) & 0xffu];
  }

  return ~crc;
}

std::uint32_t output_crc32(std::span<const std::uint8_t> data) {
  return crc32_seeded(data, kOutputCrcSeed);
}

std::uint32_t feature_crc32(std::span<const std::uint8_t> data) {
  return crc32_seeded(data, kFeatureCrcSeed);
}

void copy_state_bytes(DsState &state, std::span<const std::uint8_t> update,
                      std::size_t offset, std::size_t length) {
  std::copy(update.begin() + static_cast<std::ptrdiff_t>(offset),
            update.begin() + static_cast<std::ptrdiff_t>(offset + length),
            state.begin() + static_cast<std::ptrdiff_t>(offset));
}

void set_state_bit(std::uint8_t &byte, int bit, bool value) {
  const std::uint8_t mask = static_cast<std::uint8_t>(1u << bit);
  byte = static_cast<std::uint8_t>((byte & ~mask) |
                                   (value ? mask : std::uint8_t{0}));
}

std::uint8_t scale_light_component(std::uint8_t component,
                                   std::uint8_t brightness) {
  std::uint16_t scale = 255;
  switch (brightness) {
  case 1:
    scale = 128;
    break;
  case 2:
    scale = 64;
    break;
  default:
    break;
  }

  return static_cast<std::uint8_t>(
      (static_cast<std::uint16_t>(component) * scale + 127) / 255);
}

} // namespace

struct PcmAudioExtractor::SpeakerEncoder {
  SpeakerEncoder() {
    int error = OPUS_OK;
    encoder = opus_encoder_create(VDS_AUDIO_SAMPLE_RATE, kSpeakerChannels,
                                  OPUS_APPLICATION_AUDIO, &error);
    if (!encoder || error != OPUS_OK) {
      throw std::runtime_error("opus_encoder_create failed: " +
                               std::string(opus_strerror(error)));
    }

    opus_ctl_checked(opus_encoder_ctl(encoder, OPUS_SET_VBR(0)),
                     "OPUS_SET_VBR");
    opus_ctl_checked(opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0)),
                     "OPUS_SET_COMPLEXITY");
    opus_ctl_checked(opus_encoder_ctl(encoder, OPUS_SET_EXPERT_FRAME_DURATION(
                                                   OPUS_FRAMESIZE_10_MS)),
                     "OPUS_SET_EXPERT_FRAME_DURATION");
    opus_ctl_checked(
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(kSpeakerOpusBitrate)),
        "OPUS_SET_BITRATE");
  }

  ~SpeakerEncoder() {
    if (encoder) {
      opus_encoder_destroy(encoder);
    }
  }

  SpeakerEncoder(const SpeakerEncoder &) = delete;
  SpeakerEncoder &operator=(const SpeakerEncoder &) = delete;

  SpeakerChunk encode(std::span<const std::int16_t> pcm,
                      std::size_t pcm_frames) {
    SpeakerChunk chunk{};
    const auto opus_pcm = resample_speaker_pcm(pcm, pcm_frames);
    const int bytes =
        opus_encode(encoder, opus_pcm.data(), kSpeakerOpusFrames, chunk.data(),
                    static_cast<opus_int32>(chunk.size()));
    if (bytes < 0) {
      throw std::runtime_error("opus_encode failed: " +
                               std::string(opus_strerror(bytes)));
    }
    if (static_cast<std::size_t>(bytes) < chunk.size()) {
      std::fill(chunk.begin() + static_cast<std::ptrdiff_t>(bytes), chunk.end(),
                0);
    }
    return chunk;
  }

  OpusEncoder *encoder = nullptr;
};

struct MicAudioDecoder::Decoder {
  Decoder() {
    int error = OPUS_OK;
    decoder = opus_decoder_create(VDS_AUDIO_SAMPLE_RATE, 1, &error);
    if (!decoder || error != OPUS_OK) {
      throw std::runtime_error("opus_decoder_create failed: " +
                               std::string(opus_strerror(error)));
    }
  }

  ~Decoder() {
    if (decoder) {
      opus_decoder_destroy(decoder);
    }
  }

  Decoder(const Decoder &) = delete;
  Decoder &operator=(const Decoder &) = delete;

  std::vector<std::uint8_t>
  decode(std::span<const std::uint8_t, kMicOpusSize> payload) {
    std::array<opus_int16, kMicOpusFrames> mono{};
    const int frames = opus_decode(
        decoder, payload.data(), static_cast<opus_int32>(payload.size()),
        mono.data(), static_cast<int>(mono.size()), 0);
    if (frames < 0) {
      throw std::runtime_error("opus_decode failed: " +
                               std::string(opus_strerror(frames)));
    }

    std::vector<std::uint8_t> pcm(static_cast<std::size_t>(frames) *
                                  kMicUsbChannels * sizeof(std::int16_t));
    for (int frame = 0; frame < frames; ++frame) {
      const auto sample = static_cast<std::uint16_t>(mono[frame]);
      const auto low = static_cast<std::uint8_t>(sample & 0xff);
      const auto high = static_cast<std::uint8_t>((sample >> 8) & 0xff);
      const std::size_t offset = static_cast<std::size_t>(frame) *
                                 kMicUsbChannels * sizeof(std::int16_t);
      pcm[offset + 0] = 0;
      pcm[offset + 1] = 0;
      pcm[offset + 2] = low;
      pcm[offset + 3] = high;
    }
    return pcm;
  }

  OpusDecoder *decoder = nullptr;
};

PcmAudioExtractor::PcmAudioExtractor(std::size_t pcm_window_frames)
    : speaker_encoder_(std::make_unique<SpeakerEncoder>()),
      pcm_window_frames_(pcm_window_frames) {
  if (pcm_window_frames_ == 0 || pcm_window_frames_ > kPcmWindowFrames ||
      pcm_window_frames_ % kHapticsDecimation != 0) {
    throw std::invalid_argument("unsupported PCM frame window");
  }
}

PcmAudioExtractor::~PcmAudioExtractor() = default;

PcmAudioExtractor::PcmAudioExtractor(PcmAudioExtractor &&) noexcept = default;

PcmAudioExtractor &
PcmAudioExtractor::operator=(PcmAudioExtractor &&) noexcept = default;

void fill_output_report_checksum(std::span<std::uint8_t> report) {
  if (report.size() < sizeof(std::uint32_t)) {
    throw std::invalid_argument("output report is too small for CRC");
  }
  const std::uint32_t crc =
      output_crc32(report.first(report.size() - sizeof(std::uint32_t)));
  const std::size_t offset = report.size() - sizeof(std::uint32_t);
  report[offset + 0] = static_cast<std::uint8_t>((crc >> 0) & 0xff);
  report[offset + 1] = static_cast<std::uint8_t>((crc >> 8) & 0xff);
  report[offset + 2] = static_cast<std::uint8_t>((crc >> 16) & 0xff);
  report[offset + 3] = static_cast<std::uint8_t>((crc >> 24) & 0xff);
}

void fill_feature_report_checksum(std::span<std::uint8_t> report) {
  if (report.size() < sizeof(std::uint32_t)) {
    throw std::invalid_argument("feature report is too small for CRC");
  }
  const std::uint32_t crc =
      feature_crc32(report.first(report.size() - sizeof(std::uint32_t)));
  const std::size_t offset = report.size() - sizeof(std::uint32_t);
  report[offset + 0] = static_cast<std::uint8_t>((crc >> 0) & 0xff);
  report[offset + 1] = static_cast<std::uint8_t>((crc >> 8) & 0xff);
  report[offset + 2] = static_cast<std::uint8_t>((crc >> 16) & 0xff);
  report[offset + 3] = static_cast<std::uint8_t>((crc >> 24) & 0xff);
}

std::vector<std::uint8_t>
hidp_output_packet(std::span<const std::uint8_t> report) {
  std::vector<std::uint8_t> packet{kBtHidpOutputPrefix};
  packet.insert(packet.end(), report.begin(), report.end());
  return packet;
}

std::vector<std::uint8_t> feature_get_packet(std::uint8_t report_id) {
  return {kBtHidpGetFeaturePrefix, report_id};
}

std::vector<std::uint8_t>
feature_set_packet(std::span<const std::uint8_t> report) {
  if (report.empty()) {
    return {};
  }

  std::vector<std::uint8_t> checked_report(report.begin(), report.end());
  if (checked_report.size() >= sizeof(std::uint32_t)) {
    fill_feature_report_checksum(checked_report);
  }

  std::vector<std::uint8_t> packet;
  packet.reserve(report.size() + 1);
  packet.push_back(kBtHidpSetFeaturePrefix);
  packet.insert(packet.end(), checked_report.begin(), checked_report.end());
  return packet;
}

std::optional<UsbInputReport>
bt_input_to_usb_input(std::span<const std::uint8_t> packet) {
  if (packet.size() < kBtInputMinimumSize || packet[0] != kBtHidpInputPrefix ||
      packet[kBtInputReportIdOffset] != VDS_BT_STATE_REPORT_ID ||
      bt_input_payload_type(packet) != BtInputPayloadType::Control) {
    return std::nullopt;
  }

  UsbInputReport report{};
  report[0] = VDS_USB_INPUT_REPORT_ID;
  std::copy(packet.begin() +
                static_cast<std::ptrdiff_t>(kBtInputUsbPayloadOffset),
            packet.begin() + static_cast<std::ptrdiff_t>(kBtInputMinimumSize),
            report.begin() + 1);
  return report;
}

BtInputPayloadType bt_input_payload_type(std::span<const std::uint8_t> packet) {
  if (packet.size() <= kBtInputHeaderOffset ||
      packet[0] != kBtHidpInputPrefix ||
      packet[kBtInputReportIdOffset] != VDS_BT_STATE_REPORT_ID) {
    return BtInputPayloadType::Unknown;
  }

  switch (packet[kBtInputHeaderOffset] & kBtInputPayloadTypeMask) {
  case kBtInputPayloadTypeControl:
    return BtInputPayloadType::Control;
  case kBtInputPayloadTypeAudio:
    return BtInputPayloadType::Audio;
  default:
    return BtInputPayloadType::Unknown;
  }
}

std::optional<std::span<const std::uint8_t, kMicOpusSize>>
bt_mic_opus_payload(std::span<const std::uint8_t> packet) {
  if (bt_input_payload_type(packet) != BtInputPayloadType::Audio ||
      packet.size() < kBtMicOpusOffset + kMicOpusSize) {
    return std::nullopt;
  }
  return std::span<const std::uint8_t, kMicOpusSize>(
      packet.data() + kBtMicOpusOffset, kMicOpusSize);
}

std::optional<std::vector<std::uint8_t>>
bt_feature_to_usb_feature_reply(std::span<const std::uint8_t> packet) {
  if (packet.size() < 2 || packet[0] != kBtHidpFeaturePrefix) {
    return std::nullopt;
  }
  return std::vector<std::uint8_t>(packet.begin() + 1, packet.end());
}

MicAudioDecoder::MicAudioDecoder() : decoder_(std::make_unique<Decoder>()) {}

MicAudioDecoder::~MicAudioDecoder() = default;

MicAudioDecoder::MicAudioDecoder(MicAudioDecoder &&) noexcept = default;

MicAudioDecoder &
MicAudioDecoder::operator=(MicAudioDecoder &&) noexcept = default;

std::vector<std::uint8_t>
MicAudioDecoder::decode(std::span<const std::uint8_t, kMicOpusSize> payload) {
  return decoder_->decode(payload);
}

DsOutputState::DsOutputState()
    : state_(kInitialDsState), light_color_{state_[kOutputLedColorOffset + 0],
                                            state_[kOutputLedColorOffset + 1],
                                            state_[kOutputLedColorOffset + 2]},
      headphones_volume_(state_[kOutputHeadphoneVolumeOffset]),
      speaker_volume_(state_[kOutputSpeakerVolumeOffset]),
      mic_volume_(state_[kOutputMicVolumeOffset]),
      audio_control_(state_[kOutputAudioControlOffset] &
                     kOutputAudioControlNonPathMask),
      audio_output_path_(state_[kOutputAudioControlOffset] & kOutputPathMask),
      audio_control2_(state_[kOutputAudioControl2Offset]),
      power_save_control_(state_[kOutputPowerSaveControlOffset]),
      light_brightness_(state_[kOutputLightBrightnessOffset]) {}

void DsOutputState::apply_mic_select() {
  audio_control_ =
      static_cast<std::uint8_t>(audio_control_ & ~kOutputMicSelectMask);
  audio_control_ |=
      headset_mic_plugged_ ? kOutputMicSelectAuto : kOutputMicSelectInternal;
  state_[kOutputFlag0Offset] |= kOutputFlag0AudioControlEnable;
  state_[kOutputAudioControlOffset] =
      audio_control_with_output_path(audio_control_, audio_output_path_);
}

BtInitReport DsOutputState::build_bt_init_report() {
  BtInitReport report{};
  report[0] = 0x32;
  report[1] = 0x10;
  report[2] = 0x10 | (1 << 7);
  report[3] = kDsStateSize;
  std::copy(state_.begin(), state_.end(), report.begin() + 4);
  fill_output_report_checksum(report);
  return report;
}

bool DsOutputState::apply_usb_output_report(
    std::span<const std::uint8_t> report) {
  std::span<const std::uint8_t> update;

  if (report.size() >= 1 + kSetStateSize &&
      report[0] == VDS_USB_OUTPUT_REPORT_ID) {
    update = report.subspan(1, kSetStateSize);
  } else if (report.size() == kSetStateSize) {
    update = report.first(kSetStateSize);
  } else {
    return false;
  }

  vds_set_state_data decoded{};
  std::memcpy(&decoded, update.data(), sizeof(decoded));

  set_state_bit(state_[0], 0, decoded.enable_rumble_emulation);
  set_state_bit(state_[0], 1, decoded.use_rumble_not_haptics);
  set_state_bit(state_[38], 2, decoded.enable_improved_rumble_emulation);

  if (decoded.use_rumble_not_haptics || decoded.enable_rumble_emulation) {
    copy_state_bytes(state_, update,
                     offsetof(vds_set_state_data, rumble_emulation_right), 2);
  }
  if (decoded.allow_headphone_volume) {
    state_[kOutputFlag0Offset] |= kOutputFlag0HeadphoneVolumeEnable;
    headphones_volume_ = decoded.volume_headphones;
    copy_state_bytes(state_, update,
                     offsetof(vds_set_state_data, volume_headphones),
                     sizeof(decoded.volume_headphones));
  }
  if (decoded.allow_speaker_volume) {
    state_[kOutputFlag0Offset] |= kOutputFlag0SpeakerVolumeEnable;
    speaker_volume_ = decoded.volume_speaker;
    copy_state_bytes(state_, update,
                     offsetof(vds_set_state_data, volume_speaker),
                     sizeof(decoded.volume_speaker));
  }
  if (decoded.allow_mic_volume) {
    state_[kOutputFlag0Offset] |= kOutputFlag0MicVolumeEnable;
    mic_volume_ = decoded.volume_mic;
    state_[kOutputMicVolumeOffset] = mic_volume_;
  }
  if (decoded.allow_audio_control) {
    state_[kOutputFlag0Offset] |= kOutputFlag0AudioControlEnable;
    const std::uint8_t requested_control = update[kOutputAudioControlOffset];
    const std::uint8_t requested_non_path =
        requested_control & kOutputAudioControlNonPathMask;
    if (requested_non_path != 0) {
      audio_control_ = requested_non_path;
    }
    audio_output_path_ = requested_control & kOutputPathMask;
    apply_mic_select();
  }
  if (decoded.allow_audio_control2) {
    state_[kOutputFlag1Offset] |= kOutputFlag1AudioControl2Enable;
    audio_control2_ = update[kOutputAudioControl2Offset];
    copy_state_bytes(state_, update, kOutputAudioControl2Offset, 1);
  }
  if (decoded.allow_audio_mute) {
    state_[kOutputFlag1Offset] |= kOutputFlag1PowerSaveControlEnable;
    power_save_control_ = update[kOutputPowerSaveControlOffset];
    state_[kOutputPowerSaveControlOffset] = power_save_control_;
  }
  if (decoded.allow_speaker_volume && decoded.volume_speaker != 0) {
    state_[kOutputFlag0Offset] |=
        kOutputFlag0AudioControlEnable | kOutputFlag0SpeakerVolumeEnable;
    state_[kOutputFlag1Offset] |= kOutputFlag1AudioControl2Enable;
    state_[kOutputAudioControlOffset] =
        audio_control_with_output_path(audio_control_, kOutputPathSpeaker);
    state_[kOutputAudioControl2Offset] = audio_control2_;
  }
  if (decoded.allow_mute_light) {
    copy_state_bytes(state_, update,
                     offsetof(vds_set_state_data, mute_light_mode),
                     sizeof(decoded.mute_light_mode));
  }
  if (decoded.allow_right_trigger_ffb) {
    copy_state_bytes(state_, update,
                     offsetof(vds_set_state_data, right_trigger_ffb),
                     sizeof(decoded.right_trigger_ffb));
  }
  if (decoded.allow_left_trigger_ffb) {
    copy_state_bytes(state_, update,
                     offsetof(vds_set_state_data, left_trigger_ffb),
                     sizeof(decoded.left_trigger_ffb));
  }
  if (decoded.allow_color_light_fade_animation) {
    copy_state_bytes(state_, update,
                     offsetof(vds_set_state_data, light_fade_animation),
                     sizeof(decoded.light_fade_animation));
  }
  if (decoded.allow_light_brightness_change) {
    copy_state_bytes(state_, update, kOutputLightBrightnessOffset,
                     sizeof(decoded.light_brightness));
    light_brightness_ = decoded.light_brightness;
    emulate_light_brightness_ = true;
  }
  if (decoded.allow_player_indicators) {
    // player_light* is a bitfield byte immediately before led_red.
    copy_state_bytes(state_, update, kOutputLedColorOffset - 1, 1);
  }
  if (decoded.allow_led_color) {
    light_color_ = {update[kOutputLedColorOffset + 0],
                    update[kOutputLedColorOffset + 1],
                    update[kOutputLedColorOffset + 2]};
    copy_state_bytes(state_, update, kOutputLedColorOffset, 3);
  }
  if (emulate_light_brightness_ &&
      (decoded.allow_light_brightness_change || decoded.allow_led_color)) {
    state_[kOutputLedColorOffset + 0] =
        scale_light_component(light_color_[0], light_brightness_);
    state_[kOutputLedColorOffset + 1] =
        scale_light_component(light_color_[1], light_brightness_);
    state_[kOutputLedColorOffset + 2] =
        scale_light_component(light_color_[2], light_brightness_);
  }
  return true;
}

void DsOutputState::set_audio_out_stream_active(bool active,
                                                bool headset_plugged) {
  state_[kOutputFlag0Offset] |= kOutputFlag0AudioControlEnable;
  state_[kOutputHeadphoneVolumeOffset] = headphones_volume_;

  if (active) {
    if (headset_plugged) {
      state_[kOutputFlag0Offset] |= kOutputFlag0HeadphoneVolumeEnable;
      state_[kOutputFlag0Offset] &= ~kOutputFlag0SpeakerVolumeEnable;
      state_[kOutputFlag1Offset] &= ~kOutputFlag1AudioControl2Enable;
      state_[kOutputSpeakerVolumeOffset] = 0;
      state_[kOutputAudioControl2Offset] = 0;
      audio_output_path_ = kOutputPathHeadphones;
      state_[kOutputAudioControlOffset] =
          audio_control_with_output_path(audio_control_, audio_output_path_);
    } else {
      state_[kOutputFlag0Offset] |= kOutputFlag0SpeakerVolumeEnable;
      state_[kOutputFlag1Offset] |= kOutputFlag1AudioControl2Enable;
      state_[kOutputSpeakerVolumeOffset] = speaker_volume_;
      state_[kOutputAudioControl2Offset] = audio_control2_;
      audio_output_path_ = kOutputPathSpeaker;
      state_[kOutputAudioControlOffset] =
          audio_control_with_output_path(audio_control_, audio_output_path_);
    }
    return;
  }

  state_[kOutputFlag0Offset] &= ~kOutputFlag0SpeakerVolumeEnable;
  state_[kOutputFlag1Offset] &= ~kOutputFlag1AudioControl2Enable;
  state_[kOutputSpeakerVolumeOffset] = 0;
  state_[kOutputAudioControl2Offset] = 0;
  audio_output_path_ = kOutputPathHeadphones;
  state_[kOutputAudioControlOffset] =
      audio_control_with_output_path(audio_control_, audio_output_path_);
}

void DsOutputState::set_headset_mic_plugged(bool plugged) {
  headset_mic_plugged_ = plugged;
  state_[kOutputFlag0Offset] |=
      kOutputFlag0MicVolumeEnable | kOutputFlag0AudioControlEnable;
  state_[kOutputMicVolumeOffset] = mic_volume_;
  apply_mic_select();
}

BtStateReport DsOutputState::build_bt_mic_state_report(bool active,
                                                       bool muted) {
  const std::uint8_t power_save_control =
      active && !muted
          ? static_cast<std::uint8_t>(
                power_save_control_ &
                static_cast<std::uint8_t>(~kOutputPowerSaveMicMute))
          : static_cast<std::uint8_t>(power_save_control_ |
                                      kOutputPowerSaveMicMute);

  state_[kOutputFlag0Offset] |=
      kOutputFlag0MicVolumeEnable | kOutputFlag0AudioControlEnable;
  state_[kOutputFlag1Offset] |= kOutputFlag1MicMuteLedControlEnable |
                                kOutputFlag1PowerSaveControlEnable |
                                kOutputFlag1AudioControl2Enable;
  const std::uint8_t audio_control =
      audio_control_with_output_path(audio_control_, audio_output_path_);
  state_[kOutputMicVolumeOffset] = active && !muted ? mic_volume_ : 0;
  state_[kOutputAudioControlOffset] = audio_control;
  state_[kOutputAudioControl2Offset] = audio_control2_;
  state_[kOutputMuteLedOffset] = 0;
  state_[kOutputPowerSaveControlOffset] = power_save_control;

  BtStateReport report{};
  report[0] = VDS_BT_STATE_REPORT_ID;
  report[1] = report_sequence_ << 4;
  report_sequence_ = (report_sequence_ + 1) & 0x0f;
  report[2] = 0x10;
  report[kBtStateOffset + kOutputFlag0Offset] =
      kOutputFlag0MicVolumeEnable | kOutputFlag0AudioControlEnable;
  report[kBtStateOffset + kOutputFlag1Offset] =
      kOutputFlag1MicMuteLedControlEnable | kOutputFlag1PowerSaveControlEnable |
      kOutputFlag1AudioControl2Enable;
  report[kBtStateOffset + kOutputMicVolumeOffset] =
      active && !muted ? mic_volume_ : 0;
  report[kBtStateOffset + kOutputAudioControlOffset] = audio_control;
  report[kBtStateOffset + kOutputMuteLedOffset] = 0;
  report[kBtStateOffset + kOutputPowerSaveControlOffset] = power_save_control;
  report[kBtStateOffset + kOutputAudioControl2Offset] = audio_control2_;
  fill_output_report_checksum(report);
  return report;
}

BtInitReport DsOutputState::build_bt_mic_report(bool active) {
  BtInitReport report{};
  report[0] = kBtMicReportId;
  report[1] = mic_sequence_ << 4;
  report[2] = 0x91;
  report[3] = 0x07;
  report[4] = active ? kBtMicOpen : kBtMicClose;
  report[5] = kBtAudioBufferLength;
  report[6] = kBtAudioBufferLength;
  report[7] = kBtAudioBufferLength;
  report[8] = kBtAudioBufferLength;
  report[9] = kBtAudioBufferLength;
  report[10] = mic_sequence_;
  report[11] = 0x92;
  report[12] = 0x40;
  mic_sequence_ = (mic_sequence_ + 1) & 0x0f;
  fill_output_report_checksum(report);
  return report;
}

BtStateReport DsOutputState::build_bt_state_report() {
  BtStateReport report{};
  report[0] = VDS_BT_STATE_REPORT_ID;
  report[1] = report_sequence_ << 4;
  report_sequence_ = (report_sequence_ + 1) & 0x0f;
  report[2] = 0x10;
  std::copy(state_.begin(), state_.begin() + kSetStateSize,
            report.begin() + kBtStateOffset);
  fill_output_report_checksum(report);
  return report;
}

BtReport HapticsPacketBuilder::build_packet(
    std::span<const std::int8_t, kHapticsSampleSize> haptics,
    std::span<const std::uint8_t, kSpeakerOpusSize> speaker,
    std::span<const std::uint8_t, kDsStateSize> state,
    bool audio_sections_enabled, bool headset_plugged) {
  BtReport packet{};
  packet[0] = VDS_BT_HAPTICS_REPORT_ID;
  packet[1] = report_sequence_ << 4;
  report_sequence_ = (report_sequence_ + 1) & 0x0f;
  packet[2] = 0x11 | (1 << 7);
  packet[3] = 7;
  packet[4] = audio_sections_enabled ? kBtAudioSectionsEnable
                                     : kBtAudioSectionsDisableMic;
  packet[5] = kBtAudioBufferLength;
  packet[6] = kBtAudioBufferLength;
  packet[7] = kBtAudioBufferLength;
  packet[8] = kBtAudioBufferLength;
  packet[9] = kBtAudioBufferLength;
  packet[10] = packet_sequence_++;
  packet[11] = 0x10 | (1 << 7);
  packet[12] = kDsStateSize;
  std::copy(state.begin(), state.end(), packet.begin() + 13);
  packet[76] = 0x12 | (1 << 7);
  packet[77] = kHapticsSampleSize;
  std::memcpy(packet.data() + 78, haptics.data(), haptics.size());
  packet[kSpeakerBlockOffset] =
      (headset_plugged ? kSpeakerBlockHeadphones
                       : kSpeakerBlockControllerSpeaker) |
      (1 << 7);
  packet[kSpeakerBlockOffset + 1] = static_cast<std::uint8_t>(kSpeakerOpusSize);
  std::memcpy(packet.data() + kSpeakerDataOffset, speaker.data(),
              speaker.size());
  fill_output_report_checksum(packet);
  return packet;
}

std::vector<AudioChunk>
PcmAudioExtractor::push_usb_audio(std::span<const std::uint8_t> pcm_bytes) {
  std::vector<AudioChunk> chunks;
  chunks.reserve(pcm_bytes.size() / (kPcmFrameSize * pcm_window_frames_) + 1);

  const auto consume_frame = [this, &chunks](const std::uint8_t *base) {
    std::array<std::int16_t, kPcmChannels> samples{};
    for (std::size_t channel = 0; channel < kPcmChannels; ++channel) {
      samples[channel] = read_le_s16(base + channel * kPcmSampleSize);
      chunk_has_signal_ |= samples[channel] != 0;
    }

    speaker_pcm_[pcm_window_frame_pos_ * kSpeakerChannels + 0] = samples[0];
    speaker_pcm_[pcm_window_frame_pos_ * kSpeakerChannels + 1] = samples[1];
    haptics_pcm_[pcm_window_frame_pos_ * kSpeakerChannels + 0] = samples[2];
    haptics_pcm_[pcm_window_frame_pos_ * kSpeakerChannels + 1] = samples[3];
    chunk_has_haptics_signal_ |= samples[2] != 0 || samples[3] != 0;
    ++pcm_window_frame_pos_;

    if (pcm_window_frame_pos_ != pcm_window_frames_) {
      return;
    }

    pcm_window_frame_pos_ = 0;
    AudioChunk chunk{};
    chunk.haptics = resample_haptics_pcm(
        std::span<const std::int16_t>(haptics_pcm_.data(),
                                      pcm_window_frames_ * kSpeakerChannels),
        pcm_window_frames_);
    chunk.speaker = speaker_encoder_->encode(
        std::span<const std::int16_t>(speaker_pcm_.data(),
                                      pcm_window_frames_ * kSpeakerChannels),
        pcm_window_frames_);
    chunk.has_signal = chunk_has_signal_;
    chunk.has_haptics_signal = chunk_has_haptics_signal_;
    chunks.push_back(chunk);
    chunk_has_signal_ = false;
    chunk_has_haptics_signal_ = false;
  };

  while (!pcm_bytes.empty()) {
    if (pending_frame_pos_ == 0 && pcm_bytes.size() >= kPcmFrameSize) {
      const std::size_t frames = pcm_bytes.size() / kPcmFrameSize;
      for (std::size_t frame = 0; frame < frames; ++frame) {
        consume_frame(pcm_bytes.data() + frame * kPcmFrameSize);
      }
      pcm_bytes = pcm_bytes.subspan(frames * kPcmFrameSize);
      continue;
    }

    const std::size_t copy_size =
        std::min(pcm_bytes.size(), kPcmFrameSize - pending_frame_pos_);
    std::copy_n(pcm_bytes.begin(), copy_size,
                pending_frame_.begin() +
                    static_cast<std::ptrdiff_t>(pending_frame_pos_));
    pending_frame_pos_ += copy_size;
    pcm_bytes = pcm_bytes.subspan(copy_size);
    if (pending_frame_pos_ == kPcmFrameSize) {
      consume_frame(pending_frame_.data());
      pending_frame_pos_ = 0;
    }
  }

  return chunks;
}

std::vector<std::uint8_t> frame_bytes(std::uint16_t type,
                                      std::span<const std::uint8_t> payload) {
  if (payload.size() > VDS_FRAME_MAX_PAYLOAD) {
    throw std::invalid_argument("frame payload exceeds VDS_FRAME_MAX_PAYLOAD");
  }

  vds_frame_header header{};
  header.type = type;
  header.length = static_cast<std::uint32_t>(payload.size());

  std::vector<std::uint8_t> bytes(sizeof(header) + payload.size());
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + sizeof(header), payload.data(), payload.size());
  return bytes;
}

std::string frame_type_name(std::uint16_t type) {
  switch (type) {
  case VDS_FRAME_STATUS:
    return "STATUS";
  case VDS_FRAME_USB_HID_OUT:
    return "USB_HID_OUT";
  case VDS_FRAME_USB_FEATURE_GET:
    return "USB_FEATURE_GET";
  case VDS_FRAME_USB_FEATURE_SET:
    return "USB_FEATURE_SET";
  case VDS_FRAME_USB_AUDIO_OUT:
    return "USB_AUDIO_OUT";
  case VDS_FRAME_USB_AUDIO_IN:
    return "USB_AUDIO_IN";
  case VDS_FRAME_USB_HID_IN:
    return "USB_HID_IN";
  case VDS_FRAME_USB_FEATURE_REPLY:
    return "USB_FEATURE_REPLY";
  case VDS_FRAME_USB_INTERFACE:
    return "USB_INTERFACE";
  case VDS_FRAME_BT_CONTROL_PACKET:
    return "BT_CONTROL_PACKET";
  case VDS_FRAME_BT_INTERRUPT_PACKET:
    return "BT_INTERRUPT_PACKET";
  default:
    return "UNKNOWN";
  }
}

} // namespace vds
