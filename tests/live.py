#!/usr/bin/env python3
"""Opt-in paid DeepSeek acceptance. Credentials and transcripts are not retained."""
import json, os, pathlib, subprocess, tempfile, time
root=pathlib.Path(__file__).resolve().parents[1]
if not os.environ.get('DEEPSEEK_KEY_FILE'):raise SystemExit('Set DEEPSEEK_KEY_FILE to a private credential file')
key=pathlib.Path(os.environ['DEEPSEEK_KEY_FILE']).resolve()
with tempfile.TemporaryDirectory(prefix='gucode-live-') as td:
 p=pathlib.Path(td)
 (p/'calc.c').write_text('#include <stdio.h>\nint sum(int a,int b){return a-b;}\nint main(void){printf("%d\\n",sum(7,5));return 0;}\n')
 prompt='Fix the bug in calc.c so sum performs addition. Compile with cc, run it and verify the answer is 12. Add a shell regression test named test.sh that checks the result and exits nonzero on failure. Run that test. Do not just describe the changes; use tools to make and test them.'
 started=time.time()
 args=[str(root/'build/gucode'),'--key-file',str(key),'--yes','--no-color','--session','session.json','--max-turns','16','-p',prompt]
 r=subprocess.run(args,cwd=p,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=300)
 print(r.stdout);print(r.stderr)
 assert r.returncode==0, r.returncode
 assert (p/'test.sh').is_file()
 check=subprocess.run(['sh','test.sh'],cwd=p,capture_output=True,text=True,timeout=15)
 assert check.returncode==0,check.stdout+check.stderr
 out=subprocess.check_output(['./calc'],cwd=p,text=True,timeout=10).strip()
 assert out=='12',out
 session=json.loads((p/'session.json').read_text())
 names=[c['function']['name'] for m in session for c in m.get('tool_calls',[])]
 assert 'shell' in names and ('edit_file' in names or 'write_file' in names), names
 # Resume must recall previous task and execute a new verification.
 r2=subprocess.run(args[:-2]+['-p','Run the regression test we just created again and report its result.'],cwd=p,capture_output=True,text=True,timeout=180)
 print(r2.stdout);print(r2.stderr);assert r2.returncode==0
 report={'provider':'DeepSeek','model':os.environ.get('DEEPSEEK_MODEL','deepseek-flash'),'native':True,'bugfix':True,'independent_result':out,'regression_test':True,'session_resume':True,'tool_calls':names,'duration_seconds':round(time.time()-started,2),'time':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())}
 dest=root/'docs/research/deepseek/native.json';dest.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
