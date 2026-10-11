#!/usr/bin/env python3
"""Deterministic adverse-path tests, supplementary to real DeepSeek acceptance."""
import http.server, json, os, pathlib, subprocess, tempfile, threading, time, sys
root=pathlib.Path(__file__).resolve().parents[1]
binary=os.environ.get('GUCODE_BINARY',str(root/'build/gucode'))
class Server(http.server.ThreadingHTTPServer):
 daemon_threads=True
 def handle_error(self,request,client_address):
  if not isinstance(sys.exc_info()[1],(BrokenPipeError,ConnectionResetError)):super().handle_error(request,client_address)
class Handler(http.server.BaseHTTPRequestHandler):
 def log_message(self,*a):pass
 def do_POST(self):
  body=json.loads(self.rfile.read(int(self.headers['Content-Length'])).decode('utf-8'))
  mode=self.path[1:]
  self.server.last_request=body
  self.server.last_authorization=self.headers.get("Authorization")
  self.send_response(429 if mode=='rate' else 200);self.send_header('Content-Type','text/event-stream');self.end_headers()
  if mode=='rate':self.wfile.write(b'{"error":{"message":"rate limited"}}');return
  if mode=='escape_error':self.wfile.write(b'data: {"error":{"message":"\\u001b[2Jowned"}}\n\n');return
  def event(obj):
   b=('data: '+json.dumps(obj,ensure_ascii=False)+'\r\n\r\n').encode()
   # Every byte split independently, including UTF-8 and JSON string escapes.
   for off in range(0,len(b),3):self.wfile.write(b[off:off+3]);self.wfile.flush()
  last=body['messages'][-1]
  if last['role']=='tool':
   self.server.last_tool=json.loads(last['content'])
   event({'choices':[{'delta':{'content':'Verified ✓'},'finish_reason':None}]})
   event({'choices':[{'delta':{},'finish_reason':'stop'}]});self.wfile.write(b'data: [DONE]\n\n');return
  tools={
   'binary':('shell',{'command':'cat binary.dat'}),
   'read_binary':('read_file',{'path':'binary.dat'}),
   'edit':('edit_file',{'path':'a.txt','old_text':'old','new_text':'new'}),
   'duplicate':('edit_file',{'path':'a.txt','old_text':'old','new_text':'new'}),
   'deny':('write_file',{'path':'denied.txt','content':'must not appear'}),
   'outside':('read_file',{'path':'../outside.txt'}),
   'timeout':('shell',{'command':'sleep 30'}),
   'output':('shell',{'command':"yes x | head -c 80000"}),
   'env':('shell',{'command':'test -z "${DEEPSEEK_API_KEY:-}" && printf key-not-inherited'}),
   'broken':('write_file',{'path':'broken.txt','content':'never'}),
   'raw_nul':('write_file',{'path':'broken.txt','content':'never'}),
   'missing_done':('write_file',{'path':'broken.txt','content':'never'}),
   'after_done':('write_file',{'path':'broken.txt','content':'never'}),
   'trailing_json':('write_file',{'path':'broken.txt','content':'never'}),
   'nul':('write_file',{'path':'broken.txt','content':'never\x00suffix'}),
   'key':('read_file',{'path':'key-alias'}),
   'session':('read_file',{'path':'session.json'}),
   'flood':('shell',{'command':'yes x'}),
  }
  name,args=tools.get(mode,('list_files',{'path':'.'}));args=json.dumps(args)
  if mode=='trailing_json':args+='garbage'
  if mode=='after_done':
   event({'choices':[{'delta':{},'finish_reason':'stop'}]});self.wfile.write(b'data: [DONE]\n\n')
  event({'choices':[{'delta':{'tool_calls':[{'index':0,'id':'call_1','type':'function','function':{'name':name,'arguments':args[:4]}}]},'finish_reason':None}]})
  event({'choices':[{'delta':{'tool_calls':[{'index':0,'function':{'arguments':args[4:]}}]},'finish_reason':None}]})
  if mode=='broken':return
  event({'choices':[{'delta':{},'finish_reason':'tool_calls'}]})
  if mode=='raw_nul':self.wfile.write(b'data: [DONE]\x00garbage\n\n')
  elif mode!='missing_done':self.wfile.write(b'data: [DONE]\n\n')
server=Server(('127.0.0.1',0),Handler);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
passed=[]
try:
 with tempfile.TemporaryDirectory(prefix='gucode-regression-') as td:
  parent=pathlib.Path(td);os.environ['HOME']=str(parent/'home');(parent/'home').mkdir();(parent/'outside.txt').write_text('outside-secret');p=parent/'workspace';p.mkdir()
  for mode in ['edit','duplicate','deny','outside','timeout','output','env','broken','missing_done','after_done','trailing_json','nul','key','session','flood','raw_nul','escape_error','agents_symlink','agents_hardlink','rate']:
   (p/'a.txt').write_text('old old' if mode=='duplicate' else 'old');server.last_tool=None
   args=[binary,'--endpoint',f'http://127.0.0.1:{server.server_port}/{mode}','--no-color','--command-timeout','1','-p','test']
   if mode!='deny':args.insert(1,'--yes')
   if mode=='key':
    (p/'key').write_text('test-only');os.link(p/'key',p/'key-alias');args[1:1]=['--key-file','key']
   if mode=='session':args[1:1]=['--session','session.json']
   if mode.startswith('agents_'):
    (p/'secret').write_text('TEST-SECRET-MUST-NOT-ENTER-PROMPT')
    if mode=='agents_symlink':(p/'AGENTS.md').symlink_to(p/'secret')
    else:os.link(p/'secret',p/'AGENTS.md')
    args[1:1]=['--key-file','secret']
   start=time.monotonic();r=subprocess.run(args,cwd=p,env={**os.environ,'DEEPSEEK_API_KEY':'test-only'},capture_output=True,text=True,timeout=15)
   if mode in ['broken','missing_done','after_done','trailing_json','nul','raw_nul','escape_error','rate']:
    assert r.returncode!=0,(mode,r.stdout,r.stderr);assert not (p/'broken.txt').exists();assert server.last_tool is None
   else:
    assert r.returncode==0,(mode,r.stdout,r.stderr);tool=server.last_tool;assert tool is not None,mode
    if mode=='edit':assert (p/'a.txt').read_text()=='new'
    if mode=='duplicate':assert (p/'a.txt').read_text()=='old old' and not tool['ok']
    if mode=='deny':assert not (p/'denied.txt').exists() and not tool['ok']
    if mode=='outside':assert not tool['ok'] and 'outside-secret' not in r.stdout
    if mode in ['timeout','flood']:assert not tool['ok'] and tool['terminated'] and time.monotonic()-start<8
    if mode=='output':assert 'truncated' in tool['output'] and len(tool['output'])<65700 and tool['ok'] and not tool['terminated']
    if mode in ['key','session']:assert not tool['ok']
    if mode=='env':assert tool['ok'] and tool['output']=='key-not-inherited'
   if mode.startswith('agents_'):
    assert 'TEST-SECRET-MUST-NOT-ENTER-PROMPT' not in json.dumps(server.last_request)
    (p/'AGENTS.md').unlink()
   if mode=='escape_error':assert '\x1b' not in r.stderr and '^[' in r.stderr
   passed.append(mode)
  default=parent/'home/.guc/creds/deepseek-api-key';default.parent.mkdir(parents=True)
  explicit=parent/'explicit-key';explicit.write_text('explicit-test')
  def credential_case(expected, extra=(), home=str(parent/'home'), envkey='env-test'):
   env={**os.environ,'HOME':home,'DEEPSEEK_API_KEY':envkey}
   server.last_authorization=None
   r=subprocess.run([binary,'--yes','--endpoint',f'http://127.0.0.1:{server.server_port}/env','-C',str(p),'-p','test',*extra],env=env,capture_output=True,text=True,timeout=10)
   assert r.returncode==0,(r.stdout,r.stderr)
   assert server.last_authorization=='Bearer '+expected
   assert server.last_tool['output']=='key-not-inherited'
  default.write_text('default-test\r\n')
  credential_case('default-test',['--key-file',str(explicit)])
  credential_case('default-test',envkey='')
  (p/'key-alias').unlink();os.link(default,p/'key-alias')
  r=subprocess.run([binary,'--yes','--endpoint',f'http://127.0.0.1:{server.server_port}/key','-C',str(p),'-p','test'],env={**os.environ,'DEEPSEEK_API_KEY':'env-test'},capture_output=True,text=True,timeout=10)
  assert r.returncode==0 and not server.last_tool['ok']
  for invalid in ['', 'bad\nkey', 'bad\x00key', 'x'*4097]:
   default.write_text(invalid);server.last_authorization=None
   r=subprocess.run([binary,'--key-file',str(explicit),'-p','test'],env={**os.environ,'DEEPSEEK_API_KEY':'env-test'},capture_output=True,text=True,timeout=10)
   assert r.returncode==2 and server.last_authorization is None
  default.unlink()
  credential_case('explicit-test',['--key-file',str(explicit)])
  credential_case('env-test')
  credential_case('env-test',home='')
  passed.append('default-key-precedence-fallback-validation-protection')
 with tempfile.TemporaryDirectory(prefix='gucode-yolo-') as td:
  for mode in ['edit','env']:
   p=pathlib.Path(td);(p/'a.txt').write_text('old');server.last_tool=None
   r=subprocess.run([binary,'--yolo','--endpoint',f'http://127.0.0.1:{server.server_port}/{mode}','-p','test'],cwd=p,env={**os.environ,'DEEPSEEK_API_KEY':'test-only'},input='',capture_output=True,text=True,timeout=10)
   assert r.returncode==0 and server.last_tool['ok'],(r.stdout,r.stderr)
   assert 'Allow this action?' not in r.stdout
   if mode=='edit':assert (p/'a.txt').read_text()=='new'
  passed.append('yolo-edits-and-shell-without-prompts')
 with tempfile.TemporaryDirectory(prefix='gucode-utf8-') as td:
  p=pathlib.Path(td)
  cases=[b'\x00asm\xff\xfe\x80tail',b'\xc0\xaf\xe0\x80\x80\xed\xa0\x80\xf4\x90\x80\x80\xf5\x80\x80\x80',
         ('Hello 한글 ✓ 😀').encode(),b'x'*999+'😀'.encode(),b'x'*999+b'\xfftail',b'x'*4095+'😀'.encode()+b'end',b'x'*65535+'😀'.encode()]
  for data in cases:
   (p/'binary.dat').write_bytes(data);server.last_tool=None
   r=subprocess.run([binary,'--yolo','--endpoint',f'http://127.0.0.1:{server.server_port}/binary','--session','saved.json','-p','test'],cwd=p,env={**os.environ,'DEEPSEEK_API_KEY':'test-only'},capture_output=True,text=True,timeout=10)
   assert r.returncode==0,(r.stdout,r.stderr)
   expected=data[:65536].replace(b'\x00',b'?').decode('utf-8',errors='replace')
   # Replacement grouping can differ, but all valid text and suffixes survive.
   output=server.last_tool['output'];output.encode('utf-8')
   if b'\xed' not in data:assert output==expected+('\n[output truncated at 65536 bytes]' if len(data)>65536 else '')
   assert 'tail' not in expected or output.endswith('tail')
   json.loads((p/'saved.json').read_text(encoding='utf-8'))
   (p/'saved.json').unlink()
  (p/'binary.dat').write_bytes(b'before\xffafter')
  r=subprocess.run([binary,'--yolo','--endpoint',f'http://127.0.0.1:{server.server_port}/read_binary','-p','test'],cwd=p,env={**os.environ,'DEEPSEEK_API_KEY':'test-only'},capture_output=True,text=True,timeout=10)
  assert r.returncode==0 and server.last_tool['output']=='before�after'
  (p/'saved.json').write_bytes(b'[{"role":"user","content":"old"},{"role":"assistant","content":"binary \xed\xa0\x80 \xff tail","bad\xffkey":"value"}]')
  r=subprocess.run([binary,'--yolo','--endpoint',f'http://127.0.0.1:{server.server_port}/env','--session','saved.json','-p','continue'],cwd=p,env={**os.environ,'DEEPSEEK_API_KEY':'test-only'},capture_output=True,text=True,timeout=10)
  assert r.returncode==0 and 'repaired invalid UTF-8' in r.stderr,(r.stdout,r.stderr)
  saved=json.loads((p/'saved.json').read_text(encoding='utf-8'))
  assert saved[1]['content'].endswith('tail') and '�' in saved[1]['content']
  passed.append('binary-output-unicode-boundaries-and-session-repair')
 for endpoint in ['http://localhost:80@attacker.example/','http://127.0.0.1:123bad/path','http://localhost:999999/','file:///tmp/key']:
   r=subprocess.run([binary,'--endpoint',endpoint,'-p','x'],env={**os.environ,'DEEPSEEK_API_KEY':'test-only'},capture_output=True,timeout=5)
   assert r.returncode==2
 passed.append('endpoint-authority')
 for fd in [0,1,2]:
  server.last_tool=None
  r=subprocess.run([binary,'--yes','--endpoint',f'http://127.0.0.1:{server.server_port}/env','-p','test'],env={**os.environ,'DEEPSEEK_API_KEY':'test-only'},capture_output=True,timeout=10,preexec_fn=lambda:os.close(fd))
  assert r.returncode==0 and server.last_tool['ok'] and server.last_tool['output']=='key-not-inherited',(fd,r.stdout,r.stderr,server.last_tool)
 passed.append('closed-standard-descriptors')
 print('PASS:',', '.join(passed))
finally:server.shutdown();server.server_close()
