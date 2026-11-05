// Throwaway lane-space completion studies. Shared frame model for review/Remotion.
import {sceneSvg, impactTime} from './frame.mjs';
export const laneDirections = [
  {name:'01 / Wave', description:'One forceful crest launches from the judgement line and clears the lanes in about half a second. Perfect has a broader gold wake and a bright white leading edge.'},
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
    const q=unit(age/(kind===0&&!reduced?(perfect?.62:.52):(perfect?1.25:1.05)));
    const fade=kind===0&&!reduced?(1-unit((q-.25)/.75))**1.4:(1-q)**1.5;
    if(reduced) {
      effect=path([point(0,.98),point(1,.98),point(1,1),point(0,1)],color,.3*fade);
    } else if(kind===0) {
      const depth=1-decel(q)*.94;
      // One brief launch, then a continuous travelling crest; no second pulse.
      const launch=(1-unit(age/.12))**2;
      effect+=path([point(0,.93),point(1,.93),point(1,1),point(0,1)],color,launch*.38);
      effect+=path([point(0,.995),point(1,.995)],'#F8FDFF',launch*.95,perfect?8:6);
      // A continuous wake avoids visible alpha bands at the stronger intensity.
      const wakeEnd=depth+(perfect?.264:.216), clippedEnd=Math.min(1,wakeEnd);
      effect+=`<defs><linearGradient id="waveWake" gradientUnits="userSpaceOnUse" x1="0" y1="${n(point(0,depth)[1])}" x2="0" y2="${n(point(0,wakeEnd)[1])}"><stop stop-color="${color}" stop-opacity="${perfect?.65:.52}"/><stop offset=".5" stop-color="${color}" stop-opacity="${perfect?.23:.18}"/><stop offset="1" stop-color="${color}" stop-opacity="0"/></linearGradient></defs>`;
      effect+=path([point(0,depth),point(1,depth),point(1,clippedEnd),point(0,clippedEnd)],'url(#waveWake)',fade);
      effect+=path([point(0,depth),point(1,depth)],color,fade,perfect?9:7);
      effect+=path([point(0,depth),point(1,depth)],'#F8FDFF',fade*(perfect?.95:.7),perfect?3:2);
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
