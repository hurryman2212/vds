/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com> */
#ifndef _UAPI_VDS_H
#define _UAPI_VDS_H

#ifdef _WIN32
#include <stdint.h>
typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
#else
#include <linux/ioctl.h>
#include <linux/types.h>
#endif

#define VDS_IOC_MAGIC 'V'
#define VDS_FRAME_MAX_PAYLOAD 4096

enum {
	VDS_MIN_PORT_COUNT = 1,
	VDS_MAX_PORT_COUNT = 4,
};

enum vds_profile {
	VDS_PROFILE_DS5 = 0,
	VDS_PROFILE_DSE = 1,
};

enum vds_frame_type {
	VDS_FRAME_STATUS = 0,
	VDS_FRAME_USB_HID_OUT = 1,
	VDS_FRAME_USB_FEATURE_GET = 2,
	VDS_FRAME_USB_FEATURE_SET = 3,
	VDS_FRAME_USB_AUDIO_OUT = 4,
	VDS_FRAME_USB_HID_IN = 5,
	VDS_FRAME_USB_FEATURE_REPLY = 6,
	VDS_FRAME_USB_INTERFACE = 7,
	VDS_FRAME_BT_CONTROL_PACKET = 8,
	VDS_FRAME_BT_INTERRUPT_PACKET = 9,
	VDS_FRAME_USB_AUDIO_IN = 10,
};

enum vds_status_flags {
	VDS_STATUS_CONNECTED = 1u << 0,
	VDS_STATUS_CONFIGURED = 1u << 1,
	VDS_STATUS_HID_ENABLED = 1u << 2,
	VDS_STATUS_AUDIO_ENABLED = 1u << 3,
};

enum vds_usb_interface_type {
	VDS_USB_INTERFACE_HID = 0,
	VDS_USB_INTERFACE_AUDIO_OUT = 1,
	VDS_USB_INTERFACE_AUDIO_IN = 2,
};

enum {
	VDS_DRIVER_INFO_VERSION = 1,
	VDS_DRIVER_VERSION_MAX = 64,
};

struct vds_frame_header {
	__u16 type;
	__u16 flags;
	__u32 length;
	__u64 sequence;
};

struct vds_usb_interface_event {
	__u8 interface_number;
	__u8 altsetting;
	__u8 interface_type;
	__u8 reserved;
};

struct vds_status {
	__u32 status_flags;
	__u32 profile;
	__u64 frames_to_user;
	__u64 frames_from_user;
};

struct vds_driver_info {
	__u32 version;
	__u32 size;
	char driver_version[VDS_DRIVER_VERSION_MAX];
};

struct vds_profile_config {
	__u32 profile;
	__u32 polling_rate_mode;
};

#ifndef _WIN32
#define VDS_IOC_GET_STATUS _IOR(VDS_IOC_MAGIC, 0x01, struct vds_status)
#define VDS_IOC_SET_PROFILE _IOW(VDS_IOC_MAGIC, 0x02, struct vds_profile_config)
#define VDS_IOC_CONNECT _IO(VDS_IOC_MAGIC, 0x03)
#define VDS_IOC_DISCONNECT _IO(VDS_IOC_MAGIC, 0x04)
#define VDS_IOC_GET_DRIVER_INFO \
	_IOR(VDS_IOC_MAGIC, 0x05, struct vds_driver_info)
#endif

#endif /* _UAPI_VDS_H */
