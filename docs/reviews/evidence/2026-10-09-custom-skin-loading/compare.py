import argparse,json,pathlib,subprocess,time,statistics
parser=argparse.ArgumentParser()
parser.add_argument('--output',type=pathlib.Path,required=True)
parser.add_argument('--skin-root',type=pathlib.Path,required=True)
parser.add_argument('--samples',type=int,default=9)
options=parser.parse_args()
if options.samples < 3: parser.error('--samples must be at least 3')
work=options.output.resolve()
cases=[('litone-gameplay','LITONE12','Play/play7.luaskin'),('litone-select','LITONE12','Select/select.luaskin'),('modern-gameplay','ModernChic','play7_hw.luaskin'),('modern-select','ModernChic','musicselect.luaskin')]
rows=[]
for i,(name,skin,entry) in enumerate(cases):
 for mode in ('warm','cold'):
  for variant in (('before','after') if i%2==0 else ('after','before')):
   cmd=[str(work/f'probe-{variant}'),'--benchmark','--mode',mode,'--samples',str(options.samples),'--skin',str(options.skin_root/skin),'--entry',entry]
   start=time.monotonic();p=subprocess.run(cmd,text=True,capture_output=True)
   (work/f'{name}-{mode}-{variant}.stderr').write_text(p.stderr)
   if p.returncode: raise RuntimeError(f'{name} {mode} {variant}: {p.stdout}\n{p.stderr}')
   data=json.loads(p.stdout)
   row=dict(case=name,mode=mode,variant=variant,command=cmd,wallSeconds=time.monotonic()-start,result=data)
   rows.append(row);(work/'comparison.json').write_text(json.dumps(rows,indent=2)+'\n')
   samples=data['samplesMicros'][1:] if mode=='warm' else data['samplesMicros']
   print(name,mode,variant,'median_ms',statistics.median(samples)/1000,flush=True)
