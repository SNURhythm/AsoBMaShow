import pathlib,subprocess,shlex,os
root=pathlib.Path.cwd();build=root/'cmake-build-debug';tmp=pathlib.Path(os.environ.get('LATENCY_AUDIT_OUT', '/tmp/asobmashow-latency-audit'));tmp.mkdir(parents=True, exist_ok=True)
evidence=pathlib.Path(__file__).resolve().parent
lines=subprocess.check_output(['ninja','-C',str(build),'-t','commands','realtime_gameplay_worker_tests'],text=True).splitlines()
link=shlex.split(lines[-1])[2:-2]
objects=[]
for arg in link:
 if not arg.endswith('.o'):continue
 if '/tests/' in arg:continue
 source=arg.split('.dir/')[1][:-2]
 obj=tmp/(source.replace('/','_')+'.o')
 cmd=['c++','-std=c++23','-O2','-DNDEBUG','-DBX_CONFIG_DEBUG=0','-Isrc','-Icmake-build-debug/vcpkg_installed/arm64-osx/include','-Icmake-build-debug/vcpkg_installed/arm64-osx/include/SDL2','-c',source,'-o',str(obj)]
 subprocess.run(cmd,check=True);objects.append(str(obj))
probe=tmp/'worker_probe.o'
subprocess.run(['c++','-std=c++23','-O2','-Isrc','-Icmake-build-debug/vcpkg_installed/arm64-osx/include','-c',str(evidence/'worker_probe.cpp'),'-o',str(probe)],check=True)
result=[];skip=False
for arg in link:
 if skip:skip=False;continue
 if arg=='-o':skip=True;continue
 if arg.endswith('.o'):continue
 result.append(arg)
result += objects+[str(probe),'-o',str(tmp/'worker_probe')]
subprocess.run(result,cwd=build,check=True)
