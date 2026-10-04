#!/usr/bin/env python3
"""Run focused parser-consumer sanitizers using an existing desktop configure.

Run from the repository root, for example:
  python3 tests/run_parser_consumer_sanitizers.py visual metronome projection gameplay
This compiles scratch executables directly; it does not invoke Ninja or mutate
CMake's build outputs. Logs and executables are written beneath the printed
temporary directory.
"""
import json, pathlib, shlex, subprocess, sys, tempfile

sources={
 'visual':['tests/playfield_chart_visual_model_tests.cpp','src/bms_parser.cpp','src/scene/play/PlayfieldChartVisualModel.cpp'],
 'metronome':['tests/prep_metronome_tests.cpp','src/bms_parser.cpp','src/PrepMetronome.cpp'],
 'projection':['tests/playfield_projection_tests.cpp','src/bms_parser.cpp','src/scene/play/PlayfieldChartVisualModel.cpp','src/scene/play/PlayfieldProjection.cpp'],
 'gameplay':['tests/gameplay_simulation_tests.cpp','src/bms_parser.cpp']+['src/scene/play/'+n+'.cpp' for n in ['CompiledGameplayJudge','GameplayJudgeRules','GameplayGaugeRules','GameplayNoteJudgeRole','GameplayCandidateRules','GameplayDefinition','GameplaySimulation','SkinGameplayGraphState','Judge','RhythmLaneInputController']],
}
targets=sys.argv[1:] or list(sources)
if any(target not in sources for target in targets):
    raise SystemExit('Targets: ' + ', '.join(sources))
with open('cmake-build-debug/compile_commands.json') as stream:
    entries=json.load(stream)
output=pathlib.Path(tempfile.mkdtemp(prefix='asobmashow-consumer-sanitizers-'))
print('Output: ' + str(output), flush=True)
for target in targets:
 entry=next(x for x in entries if x['file'].endswith('/'+sources[target][0]))
 flags=[a for a in shlex.split(entry['command']) if a.startswith(('-I','-D'))]
 binary=str(output/(target+'-tests'))
 command=['clang++','-std=c++23','-O1','-g','-pthread','-fsanitize=address,undefined,float-cast-overflow,signed-integer-overflow','-fno-sanitize-recover=all',*flags,*sources[target],'-o',binary]
 with open(output/(target+'-build.log'),'w') as log:
  print(shlex.join(command),file=log,flush=True)
  subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
 with open(output/(target+'-run.log'),'w') as log:
  subprocess.run([binary],stdout=log,stderr=subprocess.STDOUT,check=True)
 print(target+': ASan/UBSan passed',flush=True)
