#!/usr/bin/env python3
"""Actual precision checks; requires the NVIDIA NVENC test host. No viewer/display change."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
from smoke import ROOT

with tempfile.TemporaryDirectory(prefix='waybridge-hdr-check-') as runtime:
    result=subprocess.run([str(ROOT/'target/debug/waybridge'),'--check-hdr'],env={**os.environ,'XDG_RUNTIME_DIR':runtime},text=True,capture_output=True,check=True,timeout=30)
    report=json.loads(result.stdout)
    assert report['streaming_hdr'] is False and report['current_capture_bits']==8,report
    assert report['gl_rgb10']['distinct_red_values']==1024,report
    assert report['gl_fp16']['distinct_red_values']==1024 and report['gl_fp16']['max_green']==2,report
    for name in ('nvenc_hevc_main10','nvenc_av1_main10'):
        assert report[name]['verified'] and report[name]['decoded_bits']==10 and report[name]['distinct_luma_values']>256,report
    assert not list(Path(runtime).iterdir()),'preflight must not create a session/token/socket'
    target=ROOT/'build/diagnostics/hdr-preflight.json';target.parent.mkdir(parents=True,exist_ok=True)
    target.write_text(json.dumps(report,indent=2)+'\n')
    print('PASS: real RGB10 and FP16 1024-value render/capture, FP16 >SDR white, actual HEVC/AV1 P010 Main10 >256 luma levels; current stream correctly reports SDR/partial and opens no viewer listener')
