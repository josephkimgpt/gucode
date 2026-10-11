#!/usr/bin/env python3
"""Real PTY editing and terminal restoration against an offline API fixture."""
import atexit, fcntl, http.server, json, os, pathlib, pty, queue, select, signal, struct, subprocess, termios, threading, time, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
BINARY = os.environ.get('GUCODE_BINARY', str(ROOT/'build/gucode'))
requests = queue.Queue()
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        last=body['messages'][-1]
        requests.put(last['content'])
        delta={'content':'OK'};finish='stop'
        if last['role']=='user' and last['content']=='approval':
            delta={'tool_calls':[{'index':0,'id':'edit','type':'function','function':{'name':'write_file','arguments':json.dumps({'path':'approved.txt','content':'yes'})}}]};finish='tool_calls'
        self.send_response(200); self.end_headers()
        self.wfile.write(('data: '+json.dumps({'choices':[{'delta':delta,'finish_reason':finish}]})+'\n\ndata: [DONE]\n\n').encode())
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=server.serve_forever,daemon=True).start()
class Session:
    def __init__(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.master,self.slave=pty.openpty()
        fcntl.ioctl(self.slave,termios.TIOCSWINSZ,struct.pack('HHHH',24,40,0,0))
        self.before=termios.tcgetattr(self.slave)
        env={**os.environ,'DEEPSEEK_API_KEY':'offline-test','HOME':'/nonexistent','NO_COLOR':'1'}
        self.p=subprocess.Popen([BINARY,'-C',self.tmp.name,'--endpoint',f'http://127.0.0.1:{server.server_port}/chat','--no-color'],stdin=self.slave,stdout=self.slave,stderr=self.slave,env=env)
        atexit.register(lambda: self.p.poll() is None and self.p.kill())
        self.data=b''; self.prompt()
    def until(self,needle):
        deadline=time.monotonic()+15
        while needle not in self.data:
            assert time.monotonic()<deadline, (needle,self.data[-1000:])
            if select.select([self.master],[],[],0.1)[0]: self.data+=os.read(self.master,65536)
        pos=self.data.index(needle)+len(needle); result=self.data[:pos]; self.data=self.data[pos:]; return result
    def prompt(self): self.until(b'\x1b[?2004h')
    def task(self,keys,want):
        os.write(self.master,keys+b'\r')
        deadline=time.monotonic()+15
        while requests.empty():
            assert time.monotonic()<deadline, self.data[-1000:]
            if select.select([self.master],[],[],0.1)[0]: self.data+=os.read(self.master,65536)
        actual=requests.get_nowait(); assert actual==want,(actual,want)
        self.prompt()
    def close(self,keys=b'/exit\r',code=0):
        if keys:os.write(self.master,keys)
        assert self.p.wait(timeout=10)==code
        after=termios.tcgetattr(self.slave)
        # Darwin sets kernel-managed PENDIN when returning to canonical mode.
        after[3] &= ~getattr(termios,'PENDIN',0); self.before[3] &= ~getattr(termios,'PENDIN',0)
        assert after==self.before,('terminal attributes not restored',self.before,after)
        os.close(self.master);os.close(self.slave);self.tmp.cleanup()
s=Session()
s.task(b'world\x01hello ','hello world')
s.task(b'abd\x02c\x05!','abcd!')
s.task(b'bad\x15good','good')
s.task(b'hello wrong\x17world','hello world')
s.task(b'abcdef\x01\x06\x06\x0bXY\x19','abXYcdef')
s.task(b'1234\x1b[H\x1b[C\x1b[3~\x1b[F\x7f','13')
s.task(b'abc\x01\x04','bc')
s.task('가나'.encode()+b'\x7f'+ '다'.encode(),'가다')
s.task(b'\x1b[A\x05!','가다!')
s.task(b'\x1b[200~hello\nworld\x1b[201~','hello world')
s.task(b'\x1b[200~hello\xe2\x1b[201~','hello')
s.task(b'draft\x10\x0e updated\x10\x0e','draft updated')
s.task(b'one two\x1bbX\x1bf!','one Xtwo!')
s.task(b'a'*120+b'\x01X\x05Y','X'+'a'*120+'Y')
fcntl.ioctl(s.slave,termios.TIOCSWINSZ,struct.pack('HHHH',24,20,0,0))
s.task(b'abc\x0c\x01X','Xabc')
fcntl.ioctl(s.slave,termios.TIOCSWINSZ,struct.pack('HHHH',24,80,0,0))
# Approval uses the same editor and exact affirmative matching.
for typed,allowed in [(b'wrong\x15y',True),(b'yess',False)]:
    os.write(s.master,b'approval\r');s.until(b'Allow this action? [y/N] ')
    assert requests.get(timeout=5)=='approval'
    os.write(s.master,typed+b'\r');s.prompt()
    result=json.loads(requests.get(timeout=5));assert result['ok']==allowed,result
s.close()
for keys,code in [(b'\x03',130),(b'\x04',0),(b'\x1b[200~unfinished\x03',130),(b'\x1b[\x03',130)]:
    s=Session();s.close(keys,code)
s=Session();s.p.send_signal(signal.SIGTERM);s.close(None,130)
server.shutdown()
print('PASS: 15 editing cases, Unicode, paste, history, resize, EOF/Ctrl-C/SIGTERM restoration')
