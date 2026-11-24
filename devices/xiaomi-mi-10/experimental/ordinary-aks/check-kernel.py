#!/usr/bin/env python3
"""Catch stale-source builds before temporary boot; not runtime qualification."""
import argparse
import hashlib
import json
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--kernel', type=Path, required=True)
p.add_argument('--report', type=Path, required=True)
p.add_argument('--baseline-report', type=Path, required=True)
a = p.parse_args()
data = a.kernel.read_bytes()
report = json.loads(a.report.read_text())
baseline = json.loads(a.baseline_report.read_text())
for marker in (b'com.hakios.aks.ordinary-lab',
               b'UMI ordinary AKS: isolated entitled client opened'):
    if marker not in data:
        raise SystemExit('Refused: isolated AKS gate is absent from this kernel')
if report.get('candidate_sha256') != hashlib.sha256(data).hexdigest():
    raise SystemExit('Refused: report does not describe this kernel')
if (not report.get('original_executable_sha256') or
        report['original_executable_sha256'] != baseline.get('original_executable_sha256') or
        report.get('original_sha256') != baseline.get('original_sha256') or
        report.get('images_modified') is not False or
        report.get('source_inputs_modified') is not False):
    raise SystemExit('Refused: original USB images differ from the baseline')
print('Kernel gate and original USB bytes verified; runtime qualification still required')
