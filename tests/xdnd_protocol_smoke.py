#!/usr/bin/env python3
"""Private X server protocol checks against the production XDND source."""
import os,subprocess,tempfile
from pathlib import Path
from smoke import ROOT

def main():
 subprocess.run(['cargo','build','--offline','--example','xdnd-protocol-probe'],cwd=ROOT,check=True)
 with tempfile.TemporaryDirectory(prefix='xdnd-protocol-')as temporary:
  log=open(Path(temporary)/'x.log','w');read,write=os.pipe();x=subprocess.Popen(['Xvfb','-displayfd',str(write),'-screen','0','1280x800x24','-nolisten','tcp','-ac'],pass_fds=[write],stdout=log,stderr=log);os.close(write)
  with os.fdopen(read)as pipe:display=pipe.readline().strip()
  try:subprocess.run([str(ROOT/'target/debug/examples/xdnd-protocol-probe'),display],cwd=ROOT,check=True,timeout=60)
  finally:x.terminate();x.wait(timeout=5);log.close()
if __name__=='__main__':main()
