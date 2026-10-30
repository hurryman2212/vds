"""Summarize baseline conversion measurements; reads only the diagnostic log."""
import json
import re
import sys
from pathlib import Path

path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).with_name('capture.log')
lines = path.read_text(encoding='utf-8-sig', errors='replace').splitlines()
markers = [i for i, line in enumerate(lines) if any(marker in line for marker in ('HAPTIC_DIAG_BASELINE_V1','HAPTIC_FIXES_V2','HAPTIC_FIXES_V3'))]
if markers:
    lines = lines[markers[-1]:]
rows = []
for line in lines:
    if 'HAPTIC_DIAG baseline=' not in line:
        continue
    values = dict(re.findall(r'(\w+)=([^\s]+)', line))
    rows.append({k: float(v) for k, v in values.items() if k not in ('state', 'usb_report')})
active = [r for r in rows if r['raw_rms'] > 1]
raw = sum(r['raw_rms'] ** 2 * r['chunks'] for r in active)
averaged = sum(r['averaged_rms'] ** 2 * r['chunks'] for r in active)
quantized = sum(r['quantized_rms'] ** 2 * r['chunks'] for r in active)
report = {
    'measured_intervals': len(rows), 'active_intervals': len(active),
    'gain_percent_values': sorted({int(r.get('gain_percent',100)) for r in rows}),
    'target_latency_us_range': [min((r.get('target_latency_us',0) for r in rows),default=0), max((r.get('target_latency_us',0) for r in rows),default=0)],
    'clipped_samples': sum(r.get('clipped_samples',0) for r in rows),
    'retained_rms_after_averaging': (averaged/raw)**0.5 if raw else None,
    'retained_rms_after_conversion': (quantized/raw)**0.5 if raw else None,
    'max_input_peak_s16': max((r['raw_peak'] for r in active), default=0),
    'max_output_peak_s8': max((r['output_peak_s8'] for r in active), default=0),
    'power_save_states_during_active_intervals': sorted({int(r['haptic_power_save']) for r in active}),
    'motor_reduction_states_during_active_intervals': sorted({int(r['motor_reduction']) for r in active}),
    'note': 'Power flags are snapshots; RMS is signal amplitude, not motor force. Conversion RMS includes configured gain; mixed-gain intervals are aggregated.'
}
print(json.dumps(report, indent=2))
