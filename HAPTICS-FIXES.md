# Windows haptics repair v2

Based on upstream 0.4.0-rc1, commit ec531599dfdd02e14ce68efd8c0d2dcb439894a5.

The measured baseline retained 98.04% of haptic RMS amplitude but queued approximately
22-24 chunks (235-256 ms) during ZZZ gameplay. These fixes do not increase gain.

## Changes

- Six-chunk startup target (64 ms), 80 ms maximum startup wait, eight-chunk
  freshness limit, 85 ms maximum completed-chunk queue age. These are software
  buffer limits, not guaranteed end-to-end latency. Smaller buffering can expose
  USB/Bluetooth stalls; monitor underflows and stale drops during gameplay.
- Audio chunks are removed under the queue lock before transmission, avoiding a
  race with producer-side eviction. Packet state is refreshed under the output
  submission lock so it does not use a snapshot taken before a newer state send.
- Preserve game-requested motor-power and haptic-low-pass updates, correct the
  byte-36 motor nibbles, and forward improved-rumble-only magnitudes.
- Round signed PCM to nearest, ties away from zero, saturating to signed 8-bit.
  Silence remains zero. No gain multiplier, dither, or resampler replacement.
- Windows writer assigns one shared 4-bit report sequence immediately before
  transmission and recalculates the checksum over the logical report length,
  leaving HID padding outside that checksum. Queued/dropped packets do not
  consume report sequence numbers. Speaker/mic payload counters are unchanged.
- Cache only the exact synchronously completed controller-state snapshot.
  Failures remain retryable; asynchronous audio invalidates the state cache.
- Bound the secondary HID audio queue to two reports and discard reports older
  than 30 ms. Report queued/completed/coalesced counts, audio completions, queue
  age, and Windows write duration. Windows completion is not controller playback
  acknowledgement. Treat a short HID write as failure, never as a byte stream.
- Once-per-second conversion metrics include RMS, peaks, queue age, and state.

## Build and checks

Standard Windows CMake build: configure as described in README-WINDOWS.md with
`-DVDS_BUILD_TESTS=ON`, build Release, then run `ctest --test-dir <build-dir> -C Release`.
The CMake test wiring has not been exercised in this environment (no CMake/VS).

The locally exercised build uses `diagnostics/build.ps1`, llvm-mingw
20261020 (ucrt-x86_64) under `../vds-tools`, Opus 1.5.2 public headers under
`diagnostics/include/opus`, and the installed Opus DLL. C++ runtime is linked
statically; the existing Opus DLL is not replaced. The script creates an import
library from that DLL's exports and runs both tests.

`metrics_test.cc` covers conversion boundaries, saturation, fragmented audio,
motor/filter gating and clearing, improved-rumble magnitudes, and mixed-report
sequence wrap/checksum handling. `scheduler_test.cc` uses the actual Windows
daemon scheduler with a fake Bluetooth transport; it covers delivery failures,
state races/deduplication, startup buffering, age/size limits, and producer/send
queue races. No physical device access is performed by these tests.

## Install and rollback

Run `diagnostics/install-fixes.ps1 -Mode Install` as administrator. It backs up
the original daemon, verifies hashes, installs v2, and starts the existing service.
If startup fails it restores the original executable. It does not reinstall
drivers, change service startup type, or alter controller registration.

Run `diagnostics/install-fixes.ps1 -Mode Restore` as administrator to restore the
verified backup and the service's pre-install running/stopped state. Existing
session/backup files are deliberately not overwritten.

The original baseline capture and scripts remain under diagnostics. Do not use
the old `finish-test.ps1` to remove v2; it deliberately rejects a different hash.
`analyze.py` can summarize either baseline or v2 markers from a supplied log file.

## Remaining validation

Hardware gameplay must confirm perceived timing/strength and inspect underflow,
stale-drop, HID coalescing, and completed-write age counters. Quantization changes
cannot establish mechanical force fidelity. The moving-average resampler remains
unchanged because its measured amplitude loss in this capture was only 0.25%.
