#!/usr/bin/env python3
"""Real independent Wayland source/target: MIME feedback and stale offer isolation."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from smoke import ROOT, Viewer
from drag_out_smoke import wait_for
from session_fixture import SessionFixture


def main():
    subprocess.run(['bash', 'tests/build-fixture.sh'], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix='wb-dnd-feedback-') as directory, SessionFixture(Path(directory)/'fixture') as fixture:
        root = Path(directory)
        runtime = Path(fixture.env['XDG_RUNTIME_DIR'])
        token = root/'token'; token.write_text('feedback-'+'a'*40); token.chmod(0o600)
        payload = root/'source.txt';payload.write_text('fixture')
        uris = root/'uris';uris.write_text(payload.as_uri()+'\r\n')
        with socket.socket() as free:
            free.bind(('127.0.0.1',0));port = free.getsockname()[1]
        env = {**fixture.env,'WAYLAND_DISPLAY':'feedback-0'}
        children = [];viewer = None
        source_log = root/'source.log';target_log = root/'target.log'
        with (root/'server.log').open('w+') as hostlog, source_log.open('w+') as srclog, target_log.open('w+') as dstlog:
            def source_lines(prefix):
                return [line for line in source_log.read_text().splitlines() if line.startswith(prefix)]
            def motion(ident,x):viewer.send(type='pointer',id=ident,x=x,y=70)
            def button(ident,down):viewer.send(type='button',id=ident,button=272,down=down)
            def feedback(x,expected):
                before = len(source_lines('SOURCE_TARGET '))
                motion(second,x)
                wait_for(lambda:len(source_lines('SOURCE_TARGET '))>before)
                assert source_lines('SOURCE_TARGET ')[-1] == 'SOURCE_TARGET '+expected, source_log.read_text()
            try:
                host = subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--socket','feedback-0','--listen',f'127.0.0.1:{port}',
                    '--token-file',str(token),'--codec','png','--no-audio','--no-xwayland'],env=env,stdout=hostlog,stderr=hostlog);children.append(host)
                wait_for(lambda:(runtime/'feedback-0').exists())
                viewer = Viewer(port,token.read_text())
                source = subprocess.Popen([str(ROOT/'build/tests/test-app')],env={**env,'WAYBRIDGE_TEST_DRAG_URIS':str(uris),
                    'WAYBRIDGE_TEST_DND_SOURCE_FEEDBACK':'1'},stdout=srclog,stderr=srclog);children.append(source)
                first = viewer.until(lambda k,m:k=='control' and m['type']=='window')['id']
                viewer.until(lambda k,m:k=='frame' and m[0]==first)
                target = subprocess.Popen([str(ROOT/'build/tests/test-app')],env={**env,'WAYBRIDGE_TEST_DND_FEEDBACK':'1',
                    'WAYBRIDGE_TEST_DROP':str(root/'delivered')},stdout=dstlog,stderr=dstlog);children.append(target)
                second = viewer.until(lambda k,m:k=='control' and m['type']=='window' and m['id']!=first)['id']
                viewer.until(lambda k,m:k=='frame' and m[0]==second)
                motion(first,50);button(first,True);time.sleep(.1)
                feedback(50,'text/uri-list')
                feedback(150,'none')
                feedback(250,'none')
                assert 'SOURCE_TARGET application/x-unsupported' not in source_log.read_text()
                feedback(50,'text/uri-list')
                before = len(source_lines('SOURCE_TARGET '));actions = source_lines('SOURCE_ACTION ')
                viewer.send(type='leave',id=second)
                wait_for(lambda:'TARGET_LEFT_STALE' in target_log.read_text())
                wait_for(lambda:len(source_lines('SOURCE_TARGET '))>before)
                # The target intentionally sends valid Accept and a different
                # SetActions after leave; neither may alter the active source.
                time.sleep(.15)
                viewer.send(type='ping',nonce='stale-left')
                viewer.until(lambda k,m:k=='control' and m['type']=='pong' and m['nonce']=='stale-left')
                assert source_lines('SOURCE_TARGET ')[before:] == ['SOURCE_TARGET none'], source_log.read_text()
                assert source_lines('SOURCE_ACTION ') == actions, source_log.read_text()
                feedback(50,'text/uri-list')
                button(second,False)
                wait_for(lambda:'SOURCE_FINISHED' in source_log.read_text())
                wait_for(lambda:(root/'delivered').exists() and (root/'delivered').read_text()==uris.read_text())
                wait_for(lambda:'TARGET_FINISHED_STALE' in target_log.read_text())
                time.sleep(.15)
                assert source_lines('SOURCE_TARGET ')[-1] == 'SOURCE_TARGET text/uri-list',source_log.read_text()
                assert source_lines('SOURCE_ACTION ')[-1] == 'SOURCE_ACTION 1',source_log.read_text()
                assert source_log.read_text().count('SOURCE_FINISHED')==1 and 'SOURCE_CANCELLED' not in source_log.read_text()
                # A fresh rejected MIME must cancel, rather than inheriting the
                # completed source's accepted target or chosen action.
                motion(first,50);button(first,True);time.sleep(.1)
                feedback(250,'none');button(second,False)
                wait_for(lambda:source_log.read_text().count('SOURCE_CANCELLED')==1)
                assert source_log.read_text().count('SOURCE_FINISHED')==1
                assert all(child.poll() is None for child in children)
                assert 'protocol error' not in (root/'server.log').read_text().lower()
                print('PASS independent Wayland source MIME accept/nil/unsupported, leave reset, inactive and finished offer isolation, Copy finish and fresh rejection')
            except Exception:
                print('SOURCE\n'+source_log.read_text());print('TARGET\n'+target_log.read_text());print('SERVER\n'+(root/'server.log').read_text())
                raise
            finally:
                if viewer:viewer.sock.close()
                for child in reversed(children):
                    if child.poll() is None:child.terminate()
                for child in children:
                    try:child.wait(timeout=5)
                    except subprocess.TimeoutExpired:child.kill();child.wait()

if __name__=='__main__':main()
