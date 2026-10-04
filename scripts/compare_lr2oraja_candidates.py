#!/usr/bin/env python3
"""Compare native LR2 candidates with pinned JudgeManager/JudgeAlgorithm control flow.

Only the reference candidate scanner and MultiBadCollector are transplanted. Note
stand-ins supply timestamp/state/type; they do not implement judgement behavior.
The actual reference JudgeProperty and JudgeAlgorithm classes are compiled unchanged.
"""
import argparse
import hashlib
import itertools
import pathlib
import random
import subprocess
import tempfile

REFERENCE_COMMIT = "5233be081abee2a7f824b78aed0d783847deeb2f"
ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(args, **kwargs):
    return subprocess.run(args, check=True, text=True, **kwargs)


def write_java(build, reference):
    play = reference / "core/src/bms/player/beatoraja/play"
    hashes = {}
    for name in ("JudgeManager.java", "JudgeProperty.java", "JudgeAlgorithm.java"):
        relative = "core/src/bms/player/beatoraja/play/" + name
        current = (play / name).read_text()
        pinned = run(["git", "-C", str(reference), "show", REFERENCE_COMMIT + ":" + relative],
                     capture_output=True).stdout
        if current != pinned:
            raise SystemExit(f"Reference source differs from pinned commit: {relative}")
        hashes[name] = hashlib.sha256(current.encode()).hexdigest()
    manager = (play / "JudgeManager.java").read_text()
    scanner = manager[manager.index("                    for (Note judgenote = state.lanemodel.getNote();"):manager.index("                    multiBadCollector.filter(tnote);")]
    scanner = scanner.replace("for (Note judgenote = state.lanemodel.getNote(); judgenote != null; judgenote = state.lanemodel.getNote())", "for (Note judgenote : notes)")
    collector = manager[manager.index("    private final class MultiBadCollector {"):manager.rfind("\n}")]
    collector = collector.replace("private final class MultiBadCollector", "private static final class MultiBadCollector")
    collector = collector.replace('System.out.println("ERROR: UNABLE TO FIND TNOTE");', "/* diagnostic has no effect on selection */")
    java = '''import java.util.*;
import bms.model.*;
import bms.player.beatoraja.play.*;
import bms.player.beatoraja.play.JudgeProperty.MissCondition;
public class ReferenceCandidateProbe {
 public static void main(String[] args) {
  Scanner input = new Scanner(System.in);
  JudgeAlgorithm[] algorithms = {JudgeAlgorithm.Lowest,JudgeAlgorithm.Combo,JudgeAlgorithm.Duration,JudgeAlgorithm.Score};
  while(input.hasNextInt()) {
   int rank=input.nextInt(),rate=input.nextInt(),mode=input.nextInt(),count=input.nextInt();
   Note[] notes=new Note[count];
   for(int i=0;i<count;i++) {
    long time=input.nextLong(); boolean longHead=input.nextInt()!=0; int state=input.nextInt();
    notes[i]=longHead?new LongNote(i,time,state):new Note(i,time,state);
   }
   JudgeAlgorithm algorithm=algorithms[mode];
   long pmtime=0; long[][] mjudge=JudgeProperty.LR2.getJudge(JudgeProperty.NoteType.NOTE,rank,new int[]{rate,rate,rate});
   long mjudgestart=0,mjudgeend=0;
   for(long[] window:mjudge){mjudgestart=Math.min(mjudgestart,window[0]);mjudgeend=Math.max(mjudgeend,window[1]);}
   MissCondition miss=MissCondition.ALWAYS; Note tnote=null; int judge=0;
   MultiBadCollector multiBadCollector=new MultiBadCollector(); multiBadCollector.setJudge(mjudge);
''' + scanner + '''
   multiBadCollector.filter(tnote);
   StringBuilder out=new StringBuilder().append(tnote==null?-1:tnote.id).append(',').append(tnote==null?-1:judge);
   if(tnote!=null) for(int i=multiBadCollector.arrayStart;i<multiBadCollector.size;i++)out.append(',').append(multiBadCollector.noteList[i].id);
   System.out.println(out);
  }
 }
''' + collector + "\n}\n"
    (build / "ReferenceCandidateProbe.java").write_text(java)
    model = build / "bms/model"
    model.mkdir(parents=True)
    (model / "Note.java").write_text("package bms.model; public class Note { public int id; long time; int state; public Note(int i,long t,int s){id=i;time=t;state=s;} public long getMicroTime(){return time;} public int getState(){return state;} public int getPlayTime(){return 0;} }")
    (model / "LongNote.java").write_text("package bms.model; public class LongNote extends Note { public LongNote(int i,long t,int s){super(i,t,s);} public boolean isEnd(){return false;} }")
    (model / "MineNote.java").write_text("package bms.model; public class MineNote extends Note { public MineNote(int i,long t,int s){super(i,t,s);} }")
    run(["javac", "-d", str(build), str(play / "JudgeProperty.java"), str(play / "JudgeAlgorithm.java"), *map(str, model.glob("*.java")), str(build / "ReferenceCandidateProbe.java")])
    return hashes


def cases():
    # Every two-note type/state combination at the NORMAL LR2 tier edges,
    # including rejected LN heads, exact edges, repeated timestamps and scan limits.
    times = [-200001,-200000,-150000,-100001,-100000,-40001,-40000,-18001,-18000,-1,0,1,18000,18001,40000,40001,100000,100001,150000,200000,200001,999999,1000000]
    for pair in itertools.combinations_with_replacement(times, 2):
        for states in itertools.product(range(4), repeat=2):
            notes=[(t,s//2,s%2) for t,s in zip(pair,states)]
            for mode in range(4):
                yield (75,100,mode,notes)
    rng = random.Random(5233)
    # Larger clusters exercise collector filtering and all four priority modes
    # at custom rates and ranks; input is sorted exactly like the lane model.
    for _ in range(10000):
        rank=rng.choice([1,25,33,50,60,75,99,100,125,200,400])
        rate=rng.choice([0,1,33,50,75,100,125,200])
        notes=sorted([(rng.randint(-210000,1100000),rng.randrange(2),rng.randrange(2)) for _ in range(rng.randrange(1,8))],key=lambda n:n[0])
        for mode in range(4):
            yield (rank,rate,mode,notes)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-repo",type=pathlib.Path,default=ROOT.parent/"lr2oraja-endlessdream")
    args=parser.parse_args()
    commit=run(["git","-C",str(args.reference_repo),"rev-parse","HEAD"],capture_output=True).stdout.strip()
    if commit!=REFERENCE_COMMIT:
        raise SystemExit(f"Expected reference {REFERENCE_COMMIT}, found {commit}")
    inputs=list(cases())
    text="".join(f"{rank} {rate} {mode} {len(notes)} "+" ".join(f"{t} {long} {played}" for t,long,played in notes)+"\n" for rank,rate,mode,notes in inputs)
    with tempfile.TemporaryDirectory(prefix="aso-candidate-parity-") as directory:
        build=pathlib.Path(directory)
        source_hash=write_java(build,args.reference_repo)
        binary=build/"candidate_probe"
        run(["clang++","-std=c++20","-O2","-I"+str(ROOT/"src/scene/play"),str(ROOT/"tests/fixtures/lr2oraja_gameplay/candidate_probe.cpp"),str(ROOT/"src/scene/play/GameplayCandidateRules.cpp"),str(ROOT/"src/scene/play/GameplayJudgeRules.cpp"),str(ROOT/"src/scene/play/CompiledGameplayJudge.cpp"),"-o",str(binary)])
        reference=run(["java","-cp",str(build),"ReferenceCandidateProbe"],input=text,capture_output=True).stdout.splitlines()
        native=run([str(binary)],input=text,capture_output=True).stdout.splitlines()
        # Check every role's complete timing table independently of selection.
        fixtures=ROOT/"tests/fixtures/lr2oraja_gameplay"
        run(["javac","-cp",str(build),"-d",str(build),str(fixtures/"WindowProbe.java")])
        window_binary=build/"window_probe"
        run(["clang++","-std=c++20","-O2","-I"+str(ROOT/"src/scene/play"),str(fixtures/"window_probe.cpp"),str(ROOT/"src/scene/play/GameplayJudgeRules.cpp"),"-o",str(window_binary)])
        reference_windows=run(["java","-cp",str(build),"WindowProbe"],capture_output=True).stdout.splitlines()
        native_windows=run([str(window_binary)],capture_output=True).stdout.splitlines()
    mismatches=[]
    if len(native)!=len(inputs) or len(reference)!=len(inputs):
        raise SystemExit(f"Unexpected rows: inputs={len(inputs)}, Java={len(reference)}, native={len(native)}")
    for index,(expected,actual) in enumerate(zip(reference,native)):
        if expected!=actual:
            if len(mismatches)<10: print(f"case={inputs[index]} reference={expected} native={actual}")
            mismatches.append(index)
    print(f"reference={commit}")
    for name, digest in source_hash.items():
        print(f"{name}.sha256={digest}")
    print(f"Candidate comparisons={len(inputs)} mismatches={len(mismatches)}")
    if len(reference_windows)!=72000 or len(native_windows)!=len(reference_windows):
        raise SystemExit("Unexpected timing-window row count")
    window_mismatches=sum(expected!=actual for expected,actual in zip(reference_windows,native_windows))
    print(f"Timing-window comparisons={len(reference_windows)} mismatches={window_mismatches}")
    return bool(mismatches or window_mismatches)

if __name__=="__main__":
    raise SystemExit(main())
