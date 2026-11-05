// Throwaway lane-space completion studies. Shared frame model for review/Remotion.
import {sceneSvg, impactTime} from './frame.mjs';
export const laneDirections = [
  {name:'01 / Wave', description:'A single wide crest travels from the judgement line toward the horizon. Perfect adds a sharper white leading edge.'},
  {name:'02 / Rails', description:'Lane boundaries lift together, leaving short luminous tails. Perfect carries the rails farther with a stronger shared onset.'},
  {name:'03 / Fan', description:'A fan opens from the final hit across the lane surface. Perfect spreads wider with a few separated shards.'},
];
const unit = v => Math.max(0,Math.min(1,v));
const decel = v => 1-(1-unit(v))**3;
const n = v => Number(v.toFixed(3));
// Perspective coordinates: u across the field, depth from horizon to judgement.
const point = (u,depth) => [640+(u-.5)*(520+440*depth),76+499*depth];
const path = (points,color,opacity,width=0) => `<path d="M${points.map(p=>p.map(n).join(' ')).join('L')}${width?'':'Z'}" ${width?`fill="none" stroke="${color}" stroke-width="${width}"`:`fill="${color}"`} opacity="${n(unit(opacity))}"/>`;
export function laneSceneSvg(seconds,kind=0,perfect=false,reduced=false) {
  const age=seconds-impactTime;
  const color=perfect?'#FFCC51':'#B7F65C';
  let effect='';
  if(age>=0&&age<1.55) {
    const q=unit(age/(perfect?1.25:1.05));
    const fade=(1-q)**1.5;
    if(reduced) {
      effect=path([point(0,.98),point(1,.98),point(1,1),point(0,1)],color,.3*fade);
    } else if(kind===0) {
      const depth=1-decel(q)*.94;
      // A band of diminishing alpha follows the crest toward the horizon.
      for(let i=0;i<12;i++) {
        const a=Math.min(1,depth+i*.012), b=Math.min(1,depth+(i+1)*.012);
        effect+=path([point(0,a),point(1,a),point(1,b),point(0,b)],color,(1-i/12)*fade*.24);
      }
      effect+=path([point(0,depth),point(1,depth)],color,fade*.85,perfect?4:3);
      if(perfect)effect+=path([point(.08,depth),point(.92,depth)],'#F8FDFF',fade*.85,1.5);
    } else if(kind===1) {
      const depth=1-decel(q)*(perfect?.94:.78);
      for(let i=0;i<=8;i++) {
        const u=i/8;
        for(let j=0;j<10;j++) {
          const a=Math.min(1,depth+j*.018), b=Math.min(1,depth+(j+1)*.018);
          effect+=path([point(u,a),point(u,b)],color,fade*(1-j/10)*.65,perfect?4:3);
        }
        const p=point(u,depth), r=(2+3*depth)*(perfect?1.2:1);
        effect+=path([[p[0],p[1]-r*2],[p[0]+r,p[1]],[p[0],p[1]+r*2],[p[0]-r,p[1]]],'#F8FDFF',fade*.85);
      }
    } else {
      const origin=point(.682,1), reach=decel(q), count=perfect?11:7;
      for(let i=0;i<count;i++) {
        const target=point(.03+.94*i/(count-1),.12+Math.abs(i-(count-1)/2)*.035);
        const tip=[origin[0]+(target[0]-origin[0])*reach,origin[1]+(target[1]-origin[1])*reach];
        const tail=[origin[0]+(target[0]-origin[0])*reach*.66,origin[1]+(target[1]-origin[1])*reach*.66];
        effect+=path([tail,tip],color,fade*.65,perfect?3:2);
        effect+=path([[tip[0]-2,tip[1]+7],[tip[0],tip[1]-7],[tip[0]+2,tip[1]+7]],i%2?color:'#F8FDFF',fade*.85);
      }
    }
  }
  const end=unit((age-(perfect?2.35:2.05))/.4);
  const dim=decel(age/.16)*(1-end*end*(3-2*end));
  const backdrop=age>=0?`<ellipse cx="640" cy="${perfect?257:285}" rx="480" ry="200" fill="url(#laneTitleDim)" opacity="${n(dim)}"/>`:'';
  const overlay=`<defs><clipPath id="laneSpace"><path d="M380 76H900L1120 575H160Z"/></clipPath><radialGradient id="laneTitleDim"><stop stop-color="#000" stop-opacity=".68"/><stop offset=".6" stop-color="#000" stop-opacity=".68"/><stop offset="1" stop-color="#000" stop-opacity="0"/></radialGradient></defs><g clip-path="url(#laneSpace)">${effect}</g>${backdrop}`;
  return sceneSvg(seconds,2,perfect,reduced,overlay);
}
