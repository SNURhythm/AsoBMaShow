import bms.player.beatoraja.play.JudgeProperty;
public class WindowProbe { public static void main(String[] args) {
 for(int rank=1;rank<=400;rank++) for(int rate: new int[]{0,1,33,50,75,99,100,101,125,200}) {
  int context=0;
  for(JudgeProperty.NoteType type: JudgeProperty.NoteType.values()) {
   long[][] w=JudgeProperty.LR2.getJudge(type,rank,new int[]{rate,rate,rate});
   for(int i=0;i<w.length;i++) System.out.println(rank+","+rate+","+context+","+i+","+(-w[i][1])+","+(-w[i][0]));
   context++;
  }
 }
}}
