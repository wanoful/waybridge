#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cuda_root="${WAYBRIDGE_CUDA_ROOT:-/opt/cuda}"
ptx_file=$(mktemp)
trap 'rm -f "$ptx_file"' EXIT
"$cuda_root/bin/nvcc" --ptx --gpu-architecture=compute_75 -O3 native/gpu_kernel.cu -o "$ptx_file"
python - "$ptx_file" <<'PY'
from pathlib import Path
import sys
import json
ptx = Path(sys.argv[1]).read_text()
Path('native/gpu_kernel_ptx.h').write_text('// Generated from gpu_kernel.cu by scripts/build-gpu-kernel.sh.\nstatic const char wb_gpu_kernel_ptx[] =\n' + '\n'.join('    ' + json.dumps(line+'\n') for line in ptx.splitlines()) + ';\n')
PY
