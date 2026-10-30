/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com> */
/*
 * DualSense Bluetooth and behavior protocol data derived from DS5Dongle.
 *
 * DS5Dongle source attribution:
 * Copyright (c) 2026 awalol, released under the MIT license.
 */
#ifndef _VDS_DS5_PROTOCOL_H
#define _VDS_DS5_PROTOCOL_H

#ifdef _WIN32
#ifdef _KERNEL_MODE
typedef unsigned char vds_u8;
typedef unsigned long vds_u32;
#else
#include <stdint.h>
typedef uint8_t vds_u8;
typedef uint32_t vds_u32;
#endif
#else
#include <linux/types.h>
typedef __u8 vds_u8;
typedef __u32 vds_u32;
#endif

#define VDS_SONY_VENDOR_ID 0x054c

#define VDS_DS5_PRODUCT_ID 0x0ce6
#define VDS_DSE_PRODUCT_ID 0x0df2

#define VDS_USB_INPUT_REPORT_ID 0x01
#define VDS_USB_INPUT_REPORT_SIZE 64
#define VDS_USB_INPUT_PAYLOAD_SIZE 63

#define VDS_USB_OUTPUT_REPORT_ID 0x02
#define VDS_SET_STATE_SIZE 47

#define VDS_BT_OUTPUT_PREFIX 0xa2
#define VDS_BT_STATE_REPORT_ID 0x31
#define VDS_BT_STATE_REPORT_SIZE 78
#define VDS_BT_HAPTICS_REPORT_ID 0x36
#define VDS_BT_HAPTICS_REPORT_SIZE 398
#define VDS_HAPTICS_SAMPLE_SIZE 64

#define VDS_AUDIO_CHANNELS 4
#define VDS_AUDIO_SAMPLE_RATE 48000
#define VDS_HAPTICS_SAMPLE_RATE 3000

#pragma pack(push, 1)
struct vds_set_state_data {
	vds_u8 enable_rumble_emulation : 1;
	vds_u8 use_rumble_not_haptics : 1;
	vds_u8 allow_right_trigger_ffb : 1;
	vds_u8 allow_left_trigger_ffb : 1;
	vds_u8 allow_headphone_volume : 1;
	vds_u8 allow_speaker_volume : 1;
	vds_u8 allow_mic_volume : 1;
	vds_u8 allow_audio_control : 1;
	vds_u8 allow_mute_light : 1;
	vds_u8 allow_audio_mute : 1;
	vds_u8 allow_led_color : 1;
	vds_u8 reset_lights : 1;
	vds_u8 allow_player_indicators : 1;
	vds_u8 allow_haptic_low_pass_filter : 1;
	vds_u8 allow_motor_power_level : 1;
	vds_u8 allow_audio_control2 : 1;
	vds_u8 rumble_emulation_right;
	vds_u8 rumble_emulation_left;
	vds_u8 volume_headphones;
	vds_u8 volume_speaker;
	vds_u8 volume_mic;
	vds_u8 mic_select : 2;
	vds_u8 echo_cancel_enable : 1;
	vds_u8 noise_cancel_enable : 1;
	vds_u8 output_path_select : 2;
	vds_u8 input_path_select : 2;
	vds_u8 mute_light_mode;
	vds_u8 touch_power_save : 1;
	vds_u8 motion_power_save : 1;
	vds_u8 haptic_power_save : 1;
	vds_u8 audio_power_save : 1;
	vds_u8 mic_mute : 1;
	vds_u8 speaker_mute : 1;
	vds_u8 headphone_mute : 1;
	vds_u8 haptic_mute : 1;
	vds_u8 right_trigger_ffb[11];
	vds_u8 left_trigger_ffb[11];
	vds_u32 host_timestamp;
	vds_u8 rumble_motor_power_reduction : 4;
	vds_u8 trigger_motor_power_reduction : 4;
	vds_u8 speaker_comp_pre_gain : 3;
	vds_u8 beamforming_enable : 1;
	vds_u8 unk_audio_control2 : 4;
	vds_u8 allow_light_brightness_change : 1;
	vds_u8 allow_color_light_fade_animation : 1;
	vds_u8 enable_improved_rumble_emulation : 1;
	vds_u8 unkbitc : 5;
	vds_u8 haptic_low_pass_filter : 1;
	vds_u8 unkbit : 7;
	vds_u8 unkbyte;
	vds_u8 light_fade_animation;
	vds_u8 light_brightness;
	vds_u8 player_light1 : 1;
	vds_u8 player_light2 : 1;
	vds_u8 player_light3 : 1;
	vds_u8 player_light4 : 1;
	vds_u8 player_light5 : 1;
	vds_u8 player_light_fade : 1;
	vds_u8 player_light_unk : 2;
	vds_u8 led_red;
	vds_u8 led_green;
	vds_u8 led_blue;
};

#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(struct vds_set_state_data) == VDS_SET_STATE_SIZE,
	      "DualSense SetStateData layout must stay protocol-sized");
#endif

#endif /* _VDS_DS5_PROTOCOL_H */
