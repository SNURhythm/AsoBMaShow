import java.util.*;
import java.lang.reflect.*;
import bms.model.*;
import bms.player.beatoraja.play.*;

// The judgement runtime is the unchanged pinned JudgeManager.java. This driver
// supplies chart/model/input data and reports observable score and tick outcomes.
public class RuntimeReferenceProbe {
 @SuppressWarnings("unchecked")
 public static void main(String[]args) throws Exception {
  Scanner in=new Scanner(System.in);
  while(in.hasNextInt()) {
   int count=in.nextInt(),survival=in.nextInt();
   List<Note>[] notes=new List[8];for(int i=0;i<8;i++)notes[i]=new ArrayList<>();
   for(int i=0;i<count;i++){
    int lane=in.nextInt(),kind=in.nextInt();long time=in.nextLong(),tail=in.nextLong();
    if(kind==0)notes[lane].add(new NormalNote(time));
    else if(kind==4)notes[lane].add(new MineNote(time,5));
    else {LongNote h=new LongNote(time,kind,false),t=new LongNote(tail,kind,true);h.setPair(t);notes[lane].add(h);notes[lane].add(t);}
   }
   BMSPlayer p=new BMSPlayer();BMSModel m=new BMSModel();m.lanes=new Lane[8];
   for(int i=0;i<8;i++){notes[i].sort(Comparator.comparingLong(Note::getMicroTime));m.lanes[i]=new Lane(notes[i].toArray(Note[]::new));}
   p.judge=new JudgeManager(p);p.judge.init(m,p.resource);
   int edges=in.nextInt(),scratchKey=7;
   for(int i=0;i<edges;i++){
    int action=in.nextInt(),lane=in.nextInt();long time=in.nextLong();
    if(action>=4&&action<=7){p.main.input.edge(action<=5?7:8,action==4||action==6,time);continue;}
    if(action==1){p.main.input.edge(lane==7?scratchKey:lane,true,time);}
    if(action==2){p.main.input.edge(lane==7?scratchKey:lane,false,time);}
    if(action==3){p.main.input.edge(scratchKey,false,time);scratchKey=scratchKey==7?8:7;p.main.input.edge(scratchKey,true,time);}
    p.judge.update(time);
   }
   StringBuilder out=new StringBuilder();for(int j=0;j<6;j++)out.append(p.judge.getJudgeCount(j)).append(',');
   out.append(p.judge.getCombo()).append(',').append(p.gauge.mines).append(',').append(p.gauge.gains).append(',').append(p.gauge.losses);
   for(int j=0;j<6;j++)out.append(',').append(p.judge.getJudgeCount(j,true)).append(',').append(p.judge.getJudgeCount(j,false));
   System.out.println(out);
  }
 }
}
