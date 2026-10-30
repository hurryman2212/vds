# Adaptive realtime haptics v3

Windows vDS based on upstream ec531599dfdd02e14ce68efd8c0d2dcb439894a5,
including all v2 fidelity, sequencing and queue fixes.

## Operation

Adaptive realtime is the only playback policy. The first packet has no fixed
prefill. Positive USB arrival jitter raises the target quickly, with a 2 ms
margin and 60 ms safety ceiling. After two stable seconds, the target decreases
2 ms per 200 ms. Queue length and stale-packet deadlines track the target.
An empty queue before the next packet deadline preserves the playback cadence.
Long input interruptions reset the estimator. This is an operating heuristic,
not a guarantee of zero latency or a mathematically optimal delay; Bluetooth
audio still uses roughly 10.7 ms packets and device/driver latency remains.

Transport failures propagate to the existing reconnect supervisor. Output
completion stalls time out after two seconds. Reconnection creates fresh queues,
timing and protocol state, while retaining the saved strength setting.

In administrator PowerShell:

```powershell
& 'C:\Program Files\vDS\vdsctl.exe' gain 125
& 'C:\Program Files\vDS\vdsctl.exe' tuning
```

Gain accepts integer percentages 0–300, applies live to all controllers, and
persists in `C:\ProgramData\vDS\vdsd.db.tuning.json`. 100 preserves the v2 level;
0 mutes rumble. Haptic PCM is amplified before signed 8-bit quantization;
ordinary rumble magnitudes are scaled separately. Speaker audio and adaptive
trigger effects are unaffected. Values above 100 can clip strong effects;
diagnostic logs record clipped samples and the current latency target.

## Validation and rollback

Portable LLVM/MinGW builds both daemon and CLI. Regression tests cover gain,
signed saturation, protocol flags/CRC, immediate startup, normal pacing,
jitter growth/decay/reset, stale eviction, queue races, transport error
propagation, reconnect-state reset and settings validation/persistence.
Hardware power-cycle recovery and end-to-end latency still require an in-game
test; simulated failures do not measure physical Bluetooth reconnection.

`install.ps1` backs up both installed executables, verifies hashes and installs
the replacement. It also checks live gain and persistence across service restart,
then returns gain to 100. See `install-Install.log` for actual results.
To restore the previous build from administrator PowerShell:

```powershell
& .\install.ps1 -Mode Restore
```

The source remains in `../../.tmp/vds-inspect`; `source.patch` is cumulative
against the upstream commit, and its `diagnostics/build.ps1` rebuilds/tests using
the portable compiler under `../../.tmp/vds-tools`.
