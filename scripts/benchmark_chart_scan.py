#!/usr/bin/env python3
"""Compare Scan workspace allocations, throughput and fresh library scans.

Needs the sibling parser repo's current performance harness and an existing Ninja
chart_library_scanner_tests build. Optimizes benchmark objects with -O2, keeps
private inputs/results outside git, and fixes the seed in benchmark snapshots.
Do not run timing samples while builds or other benchmarks are active.
"""
import argparse,concurrent.futures,hashlib,json,pathlib,shlex,shutil,sqlite3,statistics,subprocess,tempfile
root=pathlib.Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline',required=True)
parser.add_argument('--files-from',type=pathlib.Path,required=True)
parser.add_argument('--parser-repo',type=pathlib.Path,default=root.parent/'bms-parser-cpp')
parser.add_argument('--build-dir',type=pathlib.Path,default=root/'cmake-build-debug')
parser.add_argument('--output',type=pathlib.Path)
parser.add_argument('--samples',type=int,default=7)
parser.add_argument('--skip-build',action='store_true')
parser.add_argument('--build-only',action='store_true')
parser.add_argument('--baseline-workspace',action='store_true',
                    help='Use ScanWorkspace in a baseline that already supports it')
options=parser.parse_args()
if options.samples<1: raise ValueError('Samples must be positive')
out=(options.output or pathlib.Path(tempfile.mkdtemp(prefix='asobmashow-scan-'))).resolve()
build=options.build_dir.resolve()
if out.is_relative_to(root) or out.is_relative_to(build):
 raise ValueError('Output must be outside the repository and dependency build')
out.mkdir(parents=True,exist_ok=True)
revision=subprocess.check_output(['git','rev-parse','--verify','--end-of-options',options.baseline+'^{commit}'],cwd=root,text=True).strip()
files=[pathlib.Path(p).expanduser().resolve() for p in options.files_from.read_text().splitlines() if p]
if not files or any(not p.is_file() for p in files): raise ValueError('Manifest must contain existing charts')
source_names=('bms_parser.hpp','bms_parser.cpp','ChartLibraryScanner.cpp')
harness_names=('performance.cpp','ChartSnapshot.h')
provenance=dict(baseline=revision,baseline_workspace=options.baseline_workspace,build_dir=str(build),
 current={n:hashlib.sha256((root/'src'/n).read_bytes()).hexdigest() for n in source_names},
 harness={n:hashlib.sha256((options.parser_repo/'test'/n).read_bytes()).hexdigest() for n in harness_names},
 runner=hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest())
if options.skip_build:
 if json.loads((out/'build-provenance.json').read_text())!=provenance:
  raise ValueError('Saved binaries do not match the requested sources, baseline or runner')
# Copy chart files only. The source library is never scanned or modified.
library=out/'library'
if library.exists(): shutil.rmtree(library)
library.mkdir()
for i,path in enumerate(files): shutil.copy2(path,library/(str(i)+path.suffix))
(out/'manifest.txt').write_text('\n'.join(str(p) for p in files)+'\n')
if not options.skip_build:
 for v in ('baseline','current'):
  d=out/v;d.mkdir(exist_ok=True)
  for name in ('bms_parser.hpp','bms_parser.cpp','ChartLibraryScanner.cpp'):
   data=subprocess.check_output(['git','show',revision+':src/'+name],cwd=root) if v=='baseline' else (root/'src'/name).read_bytes()
   if name=='bms_parser.cpp':
    # Benchmark-only seed substitution makes RANDOM charts comparable.
    assert b'Seed = seeder();' in data
    data=data.replace(b'Seed = seeder();',b'Seed = 12345;')
   (d/name).write_bytes(data)
  for name in ('performance.cpp','ChartSnapshot.h'): shutil.copy(options.parser_repo/'test'/name,d/name)
harness=out/'scanner.cpp'
harness.write_text('''#include "ChartLibraryScanner.h"
#include "ArchiveFile.h"
#include <chrono>
#include <iostream>
int main(int argc, char **argv) {
 if(argc!=3) return 2;
 ChartRepository repository(argv[2]);
 archive_file::setArchiveIndexCacheDirectory(std::filesystem::path(argv[2]).parent_path()/"cache");
 if(!repository.EnsureReady()) return 3;
 auto session=repository.OpenSession(); if(!session) return 4;
 ChartLibraryScanner scanner;
 auto start=std::chrono::steady_clock::now();
 auto result=scanner.ScanWithResult(*session,{argv[1]});
 auto end=std::chrono::steady_clock::now();
 std::cout<<"SCAN_BENCH "<<std::chrono::duration<double,std::milli>(end-start).count()<<" "<<result.changedCount<<" "<<session->CountAllChartMeta()<<std::endl;
 std::cout<<archive_file::debugLogText()<<std::endl;
 return result.completed && result.committed ? 0:5;
}
''')
if not options.skip_build:
 entries=[e for e in json.loads((build/'compile_commands.json').read_text()) if 'CMakeFiles/chart_library_scanner_tests.dir/' in e['command']]
 objects={};jobs=[];variants={}
 for entry in entries:
  args=shlex.split(entry['command']);orig=args[args.index('-o')+1];dest=(out/orig).resolve()
  if not dest.is_relative_to(out): raise ValueError('Object path escapes output directory')
  dest.parent.mkdir(parents=True,exist_ok=True);objects[orig]=str(dest)
  args[args.index('-o')+1]=str(dest)
  args=[a for a in args if a not in ('-g','-O0','-O1','-O2','-O3','-Os','-Oz','-Ofast')]+['-O2','-g0']
  source=pathlib.Path(entry['file'])
  if source.name=='chart_library_scanner_tests.cpp': args[args.index('-c')+1]=str(harness)
  if source.name in ('bms_parser.cpp','ChartLibraryScanner.cpp'):
   variants[str(dest)]=source.name
   for v in ('baseline','current'):
    cmd=args.copy();cmd[cmd.index('-c')+1]=str(out/v/source.name);cmd[cmd.index('-o')+1]=str(out/v/(source.name+'.o'));cmd+=['-iquote',str(root/'src')];jobs.append((cmd,entry['directory']))
  else:jobs.append((args,entry['directory']))
 for v in ('baseline','current'):
  d=out/v
  args=['clang++','-std=c++17','-O2','-g0','-pthread','-DWITH_AMALGAMATION=1','-DBMS_PARSER_VERBOSE=0','-DBMS_BENCH_SCAN=1','-DBMS_BENCH_ALLOCATIONS=1','-I'+str(d),str(d/'performance.cpp'),str(d/'bms_parser.cpp'),'-o',str(d/'parser')]
  if v=='current' or options.baseline_workspace:args+=['-DBMS_BENCH_WORKSPACE=1']
  jobs.append((args,str(root)))
 def run(job):
  r=subprocess.run(job[0],cwd=job[1],capture_output=True,text=True)
  if r.returncode:raise RuntimeError(r.stdout+r.stderr)
 with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:list(pool.map(run,jobs))
 ninja=next(l.split('=',1)[1] for l in (build/'CMakeCache.txt').read_text().splitlines() if l.startswith('CMAKE_MAKE_PROGRAM:FILEPATH='))
 tokens=shlex.split(subprocess.check_output([ninja,'-C',str(build),'-t','commands','chart_library_scanner_tests'],text=True).splitlines()[-1]);assert tokens[:2]==[':','&&'] and tokens[-2:]==['&&',':'] and tokens.count('&&')==2
 for v in ('baseline','current'):
  args=[objects.get(a,a) for a in tokens[2:-2]]
  args=[str(out/v/(variants[a]+'.o')) if a in variants else a for a in args];args[args.index('-o')+1]=str(out/v/'scanner');run((args,str(build)))
 (out/'commands.json').write_text(json.dumps(jobs,indent=2))
 (out/'build-provenance.json').write_text(json.dumps(provenance,indent=2)+'\n')

if options.build_only:
 print('Built benchmark binaries:',out)
 raise SystemExit(0)

def invoke(args,label):
 result=subprocess.check_output([str(a) for a in args],cwd=root,text=True,stderr=subprocess.STDOUT)
 with (out/'raw.log').open('a') as log: log.write(label+'\n'+result+'\n')
 return result

# Fingerprint every metadata field and feature flag before measuring throughput.
for mode in ('scan','scan-file'):
 for v in ('baseline','current'):
  invoke([out/v/'parser',out/'manifest.txt',mode,1,1,out/(v+'-'+mode+'.tsv')],'equivalence '+v+' '+mode)
 if (out/('baseline-'+mode+'.tsv')).read_bytes()!=(out/('current-'+mode+'.tsv')).read_bytes():
  raise RuntimeError('Parser metadata differs in '+mode)
records=[]
for mode in ('scan','scan-file'):
 for workers in (1,4):
  samples={'baseline':[],'current':[]}
  for sample in range(options.samples):
   for v in (('baseline','current') if sample%2==0 else ('current','baseline')):
    text=invoke([out/v/'parser',out/'manifest.txt',mode,workers,1],f'{mode} {workers} {v} {sample}')
    line=next(l for l in text.splitlines() if l.startswith('round='))
    values=dict(t.split('=',1) for t in line.split())
    samples[v].append({k:float(values[k]) for k in ('wall_ms','allocations','requested_bytes')})
  records.append(dict(case=mode,workers=workers,samples=samples))

# Fresh database each time includes discovery, parsing, upserts and final commit.
# Compare every chart_meta column except wall-clock insertion time.
expected=None
samples={'baseline':[],'current':[]}
for sample in range(options.samples):
 for v in (('baseline','current') if sample%2==0 else ('current','baseline')):
  db=out/f'{v}-{sample}.db'
  for suffix in ('','-wal','-shm'): pathlib.Path(str(db)+suffix).unlink(missing_ok=True)
  text=invoke([out/v/'scanner',library,db],f'library {v} {sample}')
  line=next(l for l in text.splitlines() if l.startswith('SCAN_BENCH '))
  ms,changed,count=map(float,line.split()[1:])
  with sqlite3.connect(db) as connection:
   columns=[r[1] for r in connection.execute('PRAGMA table_info(chart_meta)') if r[1]!='add_date']
   rows=connection.execute('SELECT '+','.join('"'+c+'"' for c in columns)+' FROM chart_meta ORDER BY path').fetchall()
  if expected is None: expected=rows
  elif rows!=expected: raise RuntimeError('Stored chart metadata differs')
  if changed!=len(files) or count!=len(files): raise RuntimeError('Some charts were not stored')
  samples[v].append(dict(wall_ms=ms,charts=count))
records.append(dict(case='fresh-library',samples=samples))
for record in records:
 record['median']={v:{key:statistics.median(s[key] for s in samples) for key in samples[0]} for v,samples in record['samples'].items()}
 print(json.dumps({k:v for k,v in record.items() if k!='samples'}),flush=True)
(out/'results.json').write_text(json.dumps(dict(baseline=revision,charts=len(files),bytes=sum(p.stat().st_size for p in files),fixed_seed=12345,baseline_workspace=options.baseline_workspace,parser_metadata_equal=True,stored_metadata_equal=True,results=records),indent=2)+'\n')
print('Results:',out)
