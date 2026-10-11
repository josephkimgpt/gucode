#!/usr/bin/env python3
"""A real paid DeepSeek implementation task, scored independently of model claims."""
import json, os, pathlib, subprocess, tempfile, time
root=pathlib.Path(__file__).resolve().parents[1]
if not os.environ.get('DEEPSEEK_KEY_FILE'):raise SystemExit('Set DEEPSEEK_KEY_FILE to a private credential file')
key=pathlib.Path(os.environ['DEEPSEEK_KEY_FILE']).resolve()
with tempfile.TemporaryDirectory(prefix='gucode-project-') as td:
 p=pathlib.Path(td)
 (p/'stats.c').write_text('#include <stdio.h>\nint main(void){puts("TODO");return 0;}\n')
 (p/'Makefile').write_text('stats: stats.c\n\tcc -Wall -Wextra -std=c99 stats.c -o stats\n')
 prompt='Implement stats.c as a portable C command-line utility reading signed decimal 64-bit integers, one per line, from stdin. Ignore blank lines and allow surrounding whitespace. On valid input output exactly: count=N sum=S min=L max=H followed by a newline. On empty input output count=0 followed by a newline. On malformed input, numeric overflow, or sum overflow, print a diagnostic to stderr and exit nonzero. Do not emit a success summary for invalid input. Use the existing Makefile. Create and run a shell regression test covering normal input, negative numbers, blank input, malformed input and overflow. Inspect, implement, build, and test using tools.'
 started=time.time()
 r=subprocess.run([str(root/'build/gucode'),'--key-file',str(key),'--yes','--no-color','--session','session.json','--max-turns','24','-p',prompt],cwd=p,capture_output=True,text=True,timeout=400)
 print(r.stdout);print(r.stderr);assert r.returncode==0
 subprocess.run(['make'],cwd=p,check=True,capture_output=True,timeout=30)
 cases=[('7\n-2\n0\n','count=3 sum=5 min=-2 max=7\n'),(' \n\t\n','count=0\n'),('  -4  \n10\n\n','count=2 sum=6 min=-4 max=10\n'),('9223372036854775807\n','count=1 sum=9223372036854775807 min=9223372036854775807 max=9223372036854775807\n')]
 for data,expected in cases:
  out=subprocess.run(['./stats'],input=data,cwd=p,capture_output=True,text=True,timeout=5)
  assert out.returncode==0 and out.stdout==expected,(data,out.returncode,out.stdout,out.stderr)
 bad=['12junk\n','9223372036854775808\n','9223372036854775807\n1\n','-9223372036854775808\n-1\n']
 for data in bad:
  out=subprocess.run(['./stats'],input=data,cwd=p,capture_output=True,text=True,timeout=5)
  assert out.returncode!=0 and out.stderr and not out.stdout,(data,out.returncode,out.stdout,out.stderr)
 session=json.loads((p/'session.json').read_text());calls=[c['function']['name'] for m in session for c in m.get('tool_calls',[])]
 report={'provider':'DeepSeek','model':os.environ.get('DEEPSEEK_MODEL','deepseek-flash'),'task':'Implement and verify a signed-64-bit statistics CLI','independent_valid_cases':len(cases),'independent_invalid_cases':len(bad),'passed':True,'tool_calls':calls,'duration_seconds':round(time.time()-started,2),'time':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())}
 (root/'docs/research/deepseek/native-project.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
