# vDS (virtual DualSense)

[![Sponsor](https://img.shields.io/badge/Sponsor-hurryman2212-ea4aaa?logo=githubsponsors&logoColor=white)](https://github.com/sponsors/hurryman2212)

Virtual USB-to-Bluetooth bridge for DualSense and DualSense Edge Wireless
Controllers. vDS currently supports all USB-based DualSense features over
Bluetooth (except firmware update), including quadraphonic haptic feedback
(vibration and speaker output), adaptive triggers, microphone input, and
headphone output. Microphone and headphone support and the Windows USB/IP
integration were contributed by
[@TechAntohere](https://github.com/TechAntohere).

On both Linux and Windows, `vdsd` communicates directly with the physical
controller over Bluetooth HID and translates between its Bluetooth protocol and
virtual USB traffic. Linux exposes the virtual DualSense-class device through
`vds_hcd.ko`. Windows exports it through usbip-win2 and uses HidHide to conceal
the physical controller from other applications.

Detailed DualSense output and haptics packet handling is based on
[DS5Dongle](https://github.com/awalol/DS5Dongle) and protocol capture research.
In theory, other physical gamepad controllers could be implemented as vDS
backends and attached to vDS, as long as they can provide the DualSense-specific
features required by games. The vDS infrastructure could also be extended to
support physical transports other than Bluetooth.

## Manual Build Guides

- [Linux](README-LINUX.md)
- [Windows](README-WINDOWS.md)

## Common Controller Configuration

On both Linux and Windows, `vdsctl` is a control client. Every operation is sent
through the daemon control interface and executed by `vdsd`.

`vdsctl attach` registers paired physical Bluetooth controllers in `vdsd.db`.
The same command format and JSONL database format are used on Linux and Windows.
Use `vdsctl list-targets` to list paired attachable Bluetooth controllers and
their addresses.

```sh
vdsctl list-targets
vdsctl attach aa:bb:cc:dd:ee:01 --profile ds5 --ports 0 # Connect as DualSense
vdsctl attach aa:bb:cc:dd:ee:02 --profile dse --ports 1 # Connect as DualSense Edge
vdsctl detach aa:bb:cc:dd:ee:02 # Detach aa:bb:cc:dd:ee:02
vdsctl list # Show registered controllers
```

Omit `--ports` or pass `--ports ""` to allow all configured ports. Omit
`--profile` or pass `--profile ""` to use the physical controller profile.

> [!TIP]
>
> `--profile` can expose a controller as a different controller type than the
> physical device. This can be useful when an application supports DualSense but
> not DualSense Edge, or the other way around.

```jsonl
{"address":"aa:bb:cc:dd:ee:01","profile":"","ports":[]}
{"address":"aa:bb:cc:dd:ee:02","profile":"dse","ports":[1]}
```

With `--ports 0,2`, Linux reports `/dev/vds0` or `/dev/vds2` as the active
device path. Windows reports the corresponding USB/IP endpoint,
`usbip://127.0.0.1:3240/1-1` or `usbip://127.0.0.1:3240/1-3`.

## Reporting Issues

When reporting an issue, include the platform, vDS version, controller model,
connection type (e.g. your bluetooth adapter's model name), affected application
or game, and reproduction steps.

For runtime problems, enable tracing before reproducing the issue, then turn it
off afterwards:

```sh
vdsctl trace on --scope all
vdsctl trace off --scope all
```

You can check or attach the `vdsd` log from:

```text
Linux:   /var/log/vdsd.log
Windows: C:\ProgramData\vDS\vdsd.log
```
