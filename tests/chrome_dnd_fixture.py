"""Real browser DOM fixture and private CDP transport; input uses Win32/Wayland."""
import http.server
import json
from pathlib import Path
import select
import subprocess
import threading
import time
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
TEXT='Waybridge 中文 α text'
HTML='''<!doctype html><meta charset="utf-8"><title>Waybridge Chromium DND</title>
<style>body{margin:0;font:22px monospace;background:#eef4ff}#source{position:absolute;left:24px;top:24px;white-space:pre}input,textarea{position:absolute;left:24px;top:120px;width:400px;height:64px;font:22px monospace;box-sizing:border-box}textarea{top:220px}#reject{position:absolute;left:24px;top:340px;width:400px;height:64px;background:#ddd;user-select:none}</style>
<div id="source">Waybridge 中文 α text</div><input id="target"><textarea id="area"></textarea><div id="reject">Reject target</div>
<script>
document.title+=' '+new URLSearchParams(location.search).get('side');
window.events=[];
for(const type of ['dragstart','dragenter','dragover','dragleave','drop','dragend','input'])document.addEventListener(type,e=>{events.push({type,target:e.target.id,types:e.dataTransfer?[...e.dataTransfer.types]:[],effect:e.dataTransfer?.dropEffect,at:performance.now()});},true);
window.prepare=()=>{document.querySelector('#target').value='';document.querySelector('#area').value='';let range=document.createRange();range.selectNodeContents(document.querySelector('#source'));let s=getSelection();s.removeAllRanges();s.addRange(range);window.events=[];return true;};
window.layout=()=>{let out={dpr:devicePixelRatio,top:outerHeight-innerHeight,inset:(outerWidth-innerWidth)/2};for(const id of ['source','target','area','reject']){let r=document.getElementById(id).getBoundingClientRect();out[id]={x:r.x+Math.min(50,r.width/2),y:r.y+r.height/2};}return out;};
</script>'''

class PageServer:
    def __enter__(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                body=HTML.encode();self.send_response(200);self.send_header('Content-Type','text/html; charset=utf-8');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
            def log_message(self,*args):pass
        self.server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
        self.port=self.server.server_address[1];self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start();return self
    def __exit__(self,*args):self.server.shutdown();self.server.server_close();self.thread.join()

class Cdp:
    def __init__(self,port,title='Waybridge Chromium DND'):
        deadline=time.monotonic()+20
        while True:
            try:
                pages=json.load(urllib.request.urlopen(f'http://127.0.0.1:{port}/json/list',timeout=1))
                page=next(p for p in pages if p.get('type')=='page' and title in p.get('title',''))
                url=page['webSocketDebuggerUrl'];url=url.replace(url.split('/')[2],f'127.0.0.1:{port}');break
            except (OSError,StopIteration):
                if time.monotonic()>deadline:raise
                time.sleep(.1)
        self.process=subprocess.Popen(['node',str(ROOT/'tests/chrome_cdp_bridge.mjs'),url],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        self.sequence=0
    def call(self,method,**params):
        self.sequence+=1
        self.process.stdin.write(json.dumps(dict(id=self.sequence,method=method,params=params))+'\n');self.process.stdin.flush()
        if not select.select([self.process.stdout],[],[],15)[0]:raise TimeoutError('private CDP request timed out: '+method)
        line=self.process.stdout.readline()
        if not line:raise RuntimeError(self.process.stderr.read())
        result=json.loads(line)
        if 'error' in result:raise RuntimeError(result['error'])
        return result['result']
    def evaluate(self,expression):
        result=self.call('Runtime.evaluate',expression=expression,returnByValue=True,awaitPromise=True)
        if 'exceptionDetails' in result:raise RuntimeError(result['exceptionDetails'])
        return result['result'].get('value')
    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:self.process.kill();self.process.wait()
