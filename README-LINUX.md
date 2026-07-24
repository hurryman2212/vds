# vDS Linux Guide

[Back to README.md](README.md)

> [!NOTE]
>
> Starting with 0.3.0, the internal database format used by `vdsd` has changed
> for future extensibility and is not compatible with older releases. Remove
> `/var/lib/vds`, then register your controllers again with `vdsctl attach ...`.

Linux uses a custom virtual USB HCD and raw BlueZ/L2CAP input. Games and
applications see a USB DualSense-class controller, while `vdsd` owns the
physical Bluetooth HID Control and Interrupt channels.

This project includes a custom virtual USB HCD primarily because Linux's
`dummy_hcd` module does not support isochronous transfers, which are required
for the USB audio path used by DualSense haptics. The custom HCD currently also
hosts the `/dev/vdsX` bridge and virtual port lifecycle, but that architecture
may be revisited if `dummy_hcd` gains suitable isochronous transfer support.

The current architecture uses both the kernel module and userspace daemon for
runtime communication. For example, haptic feedback currently follows this flow:

```text
Application
  -> Userspace audio server (e.g. PipeWire)
  -(ALSA PCM)-> Linux ALSA stack [snd-usb-audio.ko]
  -(USB isochronous OUT URBs)-> Linux USB stack [vds_hcd.ko]
  -(/dev/vdsX)-> vdsd
  -(AF_BLUETOOTH L2CAP socket)-> Linux Bluetooth stack
  -(Bluetooth HID Control/Interrupt)-> DualSense (Edge) controller
```

The virtual USB HID endpoint descriptors match each controller's USB HID
specification: DualSense HID IN/OUT use 4 ms intervals, while DualSense Edge HID
IN uses 1 ms and HID OUT uses 4 ms. vDS uses the HID IN interval to pace virtual
USB input URB completion.

## Dependencies

### Runtime Dependencies

- `libopus` (Bluetooth audio encoding)
- `libudev` (virtual device discovery)
- `libdbus-1` (BlueZ device metadata)
- `libbluetooth` (Bluetooth address and L2CAP helpers)

### Build Dependencies

- `gnu99` compiler toolchain compatible with the target kernel build
- `git` (for versioning)
- `make`
- Target Linux kernel headers
- `dkms` (for installing and rebuilding the kernel module automatically)
- `c++20` compiler toolchain for userspace
- `cmake` (3.12 or newer)
- `pkg-config`

On Debian-based systems:

```sh
sudo apt install git build-essential dkms cmake pkg-config
sudo apt install libopus-dev libudev-dev libdbus-1-dev libbluetooth-dev
```

On Arch/CachyOS-based systems:

```sh
sudo pacman -S git base-devel dkms cmake pkgconf
sudo pacman -S opus systemd dbus bluez-libs
```

Install the header package matching your running kernel separately. If your
kernel was built with the LLVM toolchain, install the matching LLVM toolchain
instead of relying only on the default GNU toolchain.

## Kernel Module Install

Build the kernel module locally:

```sh
make -C module
```

Install or remove it through DKMS:

```sh
sudo make -C module install
sudo make -C module uninstall
```

Load the module:

```sh
sudo modprobe vds_hcd
```

Without a `max_port` parameter, vDS creates 4 virtual controller ports. To
override the number of virtual controller ports:

```sh
sudo modprobe vds_hcd max_port=2 # must be in the range `1..4`
```

## Userspace Install

Build the daemon and CLI:

```sh
cmake . -B build
make -C build
```

Install or remove the userspace tools:

```sh
sudo make -C build install
sudo make -C build uninstall
```

The installed tools are:

```sh
/usr/local/bin/vdsd
/usr/local/bin/vdsctl
```

To install and remove the `vdsd` systemd service with the userspace tools,
configure with `INSTALL_SERVICE=YES`:

```sh
cmake . -B build -DINSTALL_SERVICE=YES
make -C build
sudo make -C build install
sudo make -C build uninstall
```

When `INSTALL_SERVICE=YES` is used, CMake enables the `vdsd.service` systemd
service for future boots. The service is not started immediately after
installation. To start it without rebooting, run:

```sh
sudo systemctl restart vdsd.service
```

## Initial Bluetooth Pairing

> [!IMPORTANT]
>
> Current limitation: run `bluetoothd` with the input plugin disabled
> (`--noplugin=input`). vDS needs direct ownership of the Bluetooth HID Control
> and Interrupt L2CAP channels. If the normal BlueZ input plugin is active, it
> can claim the controller first and expose it as a regular Bluetooth gamepad
> instead of letting vDS bridge it as a virtual USB controller.
>
> In simpler terms, using vDS on Linux currently means giving up Bluetooth
> devices like mice or keyboards. :( A BlueZ userspace stack patch is being
> prepared to remove this limitation.
>
> In the meantime, the helper script `override-bluetoothd.sh` can install or
> remove the required `bluetooth.service` drop-in override:
>
> ```sh
> sudo ./override-bluetoothd.sh disable-input --restart
> sudo ./override-bluetoothd.sh enable-input --restart
> ```

Pair each physical controller once before registering it with `vdsctl`. Start
`bluetoothctl`, put the controller in Bluetooth pairing mode by holding Create
and PS until the light bar blinks rapidly, then use the MAC address printed by
`scan on`:

```text
agent NoInputNoOutput
default-agent
pairable on
scan on
pair XX:XX:XX:XX:XX:XX
trust XX:XX:XX:XX:XX:XX
scan off
quit
```

When re-pairing a controller that BlueZ already knows, run
`remove XX:XX:XX:XX:XX:XX` before `scan on`.

## Audio Setup

Configure the virtual controller audio output as 48 kHz 4-channel S16_LE PCM.
The exact setup differs by audio stack, such as PipeWire, PulseAudio, ALSA, or
JACK.

For PipeWire/WirePlumber, set the controller card profile to `pro-audio`. Find
the WirePlumber device id:

```sh
wpctl status
```

List the available profile indexes for that device:

```sh
pw-cli e <device-id> EnumProfile | awk '/Profile:index/{getline; idx=$2} /Profile:name/{getline; gsub(/"/, "", $2); print idx, $2}'
```

Set the `pro-audio` profile:

```sh
wpctl set-profile <device-id> <pro-audio-profile-index>
```

After setting the `pro-audio` profile, install the included WirePlumber rule. It
gives the virtual controller stable display names, sets the output node to 4
channels, disables channelmix normalization, and gives the microphone source a
low priority:

```sh
mkdir -p ~/.config/wireplumber/wireplumber.conf.d
cp 99-vds-dualsense-wireplumber.conf ~/.config/wireplumber/wireplumber.conf.d/
```

Restart the user audio services after changing this file:

```sh
systemctl --user restart pipewire pipewire-pulse wireplumber
```

## Input Setup

This step may not be required on every system, but `vds_hcd` is a platform
controller. Without an override, a DualSense touchpad connected through vDS can
be classified as an `internal` touchpad instead of an `external` touchpad like a
USB-connected controller. Install the included udev rule so the virtual
controller touchpad is classified like the real USB controller:

```sh
sudo cp 99-vds-dualsense-udev.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=input
```

## Testing

Check that the virtual USB controller enumerated:

```sh
lsusb -d 054c:
```

Check input devices and force-feedback support with standard tools:

```sh
evtest
fftest /dev/input/eventX
```

Check the virtual USB audio endpoint with standard ALSA tools:

```sh
aplay -l
speaker-test -D hw:<card-number>,<device-number> -c 4 -r 48000 -F S16_LE -t sine
```
