#!/usr/bin/env python3
"""Build a capture tool from the pinned historical application, outside the checkout.
Run --check to compile the same harness against current sources and compare.
Capture only writes a new output directory: committed historical evidence is immutable.
"""
import argparse, hashlib, json, pathlib, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
REVISION = '68382de1627f321aa8a56c7a961d75b4f7b974b7'
HASHES = {'bms_parser.hpp': 'ce853b35c45f27b2fde0f3c2264ccdef0f6e01c148b005997837035cdb505c42',
          'bms_parser.cpp': 'ad33611897caa887c3e82dbb8edd6602ae40425647334d257acca0eda6c7433f'}
SOURCES = ['FileChecksum.cpp', 'ModernResult.cpp', 'ScoreProvenance.cpp', 'Uuid.cpp',
           'Utils.cpp', 'path.cpp', 'bms_parser.cpp',
           'replay/BeatorajaReplayCodec.cpp', 'replay/Base64Url.cpp', 'replay/GzipCodec.cpp',
           'replay/ReplayCapabilities.cpp', 'replay/ReplayPlayback.cpp', 'replay/ReplayPlaybackDriver.cpp',
           'replay/ReplayPlaybackMaterializer.cpp', 'replay/ReplaySetup.cpp',
           'replay/ReplaySetupAdapter.cpp', 'scene/play/CompiledGameplayJudge.cpp',
           'scene/play/GameplayCandidateRules.cpp', 'scene/play/GameplayDefinition.cpp',
           'scene/play/GameplayGaugeRules.cpp', 'scene/play/GameplayJudgeRules.cpp',
           'scene/play/GameplayNoteJudgeRole.cpp', 'scene/play/GameplayRulesetPolicy.cpp',
           'scene/play/GameplaySimulation.cpp', 'scene/play/SkinGameplayGraphState.cpp',
           'scene/play/Judge.cpp', 'scene/play/ReplayKeysoundSchedule.cpp']
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--check', action='store_true')
p.add_argument('--output', type=pathlib.Path)
args = p.parse_args()
with tempfile.TemporaryDirectory(prefix='asobmashow-replay-capture-') as scratch:
    scratch = pathlib.Path(scratch)
    if args.check:
        source = ROOT / 'src'
        fixtures = ROOT / 'tests/fixtures/historical_replays'
        manifest = json.loads((fixtures/'manifest.json').read_text())
        for name, expected in manifest['files'].items():
            if hashlib.sha256((fixtures/name).read_bytes()).hexdigest() != expected:
                raise RuntimeError('Immutable historical evidence changed: ' + name)
    else:
        if args.output is None or args.output.exists():
            p.error('capture requires --output naming a NEW directory')
        subprocess.run(['git', 'archive', REVISION, 'src'], cwd=ROOT,
                       stdout=(scratch / 'source.tar').open('wb'), check=True)
        subprocess.run(['tar', '-xf', str(scratch / 'source.tar'), '-C', str(scratch)], check=True)
        (scratch / 'yoga').symlink_to(ROOT / 'yoga', target_is_directory=True)
        (scratch / 'bgfx').symlink_to(ROOT / 'bgfx', target_is_directory=True)
        source = scratch / 'src'
        for name, expected in HASHES.items():
            assert hashlib.sha256((source/name).read_bytes()).hexdigest() == expected
        import shutil
        fixtures = args.output.resolve()
        shutil.copytree(ROOT/'tests/fixtures/historical_replays/charts', fixtures/'charts')
    command = ['c++', '-std=c++23', '-O1', '-pthread', '-I'+str(source),
               '-I'+str(ROOT/'yoga/lib'), '-I'+str(ROOT/'include'),
               '-I'+str(ROOT/'bgfx/bgfx/include'), '-I'+str(ROOT/'bgfx/bx/include'), '-I'+str(ROOT/'cmake-build-debug/vcpkg_installed/arm64-osx/include'),
               str(ROOT/'tests/historical_replay_compatibility.cpp')]
    if not args.check: command += ['-DHISTORICAL_REPLAY_CAPTURE']
    subprocess.run(['cc', '-O1', '-c', str(source/'MinizBridge.c'), '-o', str(scratch/'miniz.o')], check=True)
    command += [str(source/f) for f in SOURCES] + [str(scratch/'miniz.o'), '-lsqlite3']
    if __import__('sys').platform == 'darwin': command += ['-framework', 'CoreFoundation']
    command += ['-o', str(scratch/'replay-matrix')]
    subprocess.run(command, check=True)
    subprocess.run([str(scratch/'replay-matrix'), str(fixtures)] + ([] if args.check else ['--capture']), check=True)

    if not args.check:
        manifest = {'applicationRevision': REVISION,
                    'parserRevision': '14d7a2353102e7ebe3646fd581e1cad7afabdb66',
                    'parserArtifacts': HASHES,
                    'captureHarnessSha256': hashlib.sha256((ROOT/'tests/historical_replay_compatibility.cpp').read_bytes()).hexdigest(),
                    'files': {str(f.relative_to(fixtures)): hashlib.sha256(f.read_bytes()).hexdigest()
                              for f in sorted(fixtures.rglob('*')) if f.is_file()}}
        (fixtures/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
