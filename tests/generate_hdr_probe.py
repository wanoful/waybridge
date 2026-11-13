#!/usr/bin/env python3
"""Regenerate own Main10/PQ access units used to verify Windows P010 hardware output."""
import struct,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1]
raw=struct.pack('<H',512<<6)*(256*256+256*128)
out=['// Generated from our own 256x256 P010/PQ gray NVENC frame.\n#pragma once\n#include <cstdint>\n']
for name,codec,fmt in [('HevcHdrCapabilitySample','hevc_nvenc','hevc'),('Av1HdrCapabilitySample','av1_nvenc','obu')]:
    cmd=['ffmpeg','-v','error','-f','rawvideo','-pixel_format','p010le','-video_size','256x256','-i','pipe:0','-frames:v','1','-c:v',codec,'-preset','p1','-tune','ull','-rc','constqp','-qp','1','-color_primaries','bt2020','-color_trc','smpte2084','-colorspace','bt2020nc','-color_range','tv']
    if fmt=='hevc':cmd+=['-profile:v','main10','-aud','1','-bsf:v','filter_units=remove_types=38']
    else:cmd+=['-bsf:v','av1_metadata=td=insert:delete_padding=1']
    data=subprocess.run(cmd+['-f',fmt,'pipe:1'],input=raw,check=True,capture_output=True).stdout
    out.append(f'inline constexpr uint8_t {name}[] = {{\n')
    for i in range(0,len(data),16):out.append('    '+', '.join(f'0x{x:02x}' for x in data[i:i+16])+',\n')
    out.append('};\n')
(root/'client/hdr_capability_samples.h').write_text(''.join(out))
