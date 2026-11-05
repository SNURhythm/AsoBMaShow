// MAX studies: all-PGREAT completion. Authoring only; no native scoring changes.
import {stage, impactTime} from './frame.mjs';
import {laneSceneSvg} from './lane-frame.mjs';
export const maxDirections = [
  {name:'01 / MAX · Focus', description:'Short strokes converge, then release once around a crisp MAX. White lettering and PGREAT teal give it a precise, decisive peak.'},
  {name:'02 / MAX · Lift', description:'Three light trails rise from the judgement line into the lettering, then open outward. A taller, more flowing culmination.'},
  {name:'03 / Perfect', description:'The existing gold Perfect is the reference. MAX uses an all-PGREAT sample; this Perfect sample includes GREATs.'},
];
const maxClamp = v => Math.max(0,Math.min(1,v));
const maxEase = v => 1-(1-maxClamp(v))**3;
const maxSmooth = v => {const q=maxClamp(v);return q*q*(3-2*q);};
const maxNum = v => Number(v.toFixed(3));
const maxStroke = (x,y,xx,yy,width,alpha,color='#56D2CA') => `<path d="M${maxNum(x)} ${maxNum(y)}L${maxNum(xx)} ${maxNum(yy)}" fill="none" stroke="${color}" stroke-width="${width}" opacity="${maxNum(maxClamp(alpha))}"/>`;
export function maxSceneSvg(seconds,kind=0,_perfect=false,reduced=false) {
  void _perfect;
  if(kind===2)return laneSceneSvg(seconds,0,true,reduced);
  const t=seconds-impactTime;
  const exit=1-maxSmooth((t-2.45)/.45);
  const onset=kind===0?.14:.08;
  const entrance=maxEase((t-onset)/.2);
  const opacity=entrance*exit;
  let light='', motion='', letters='';
  const defs=`<defs>
    <clipPath id="maxLane"><path d="M380 76H900L1120 575H160Z"/></clipPath>
    <radialGradient id="maxDim"><stop stop-color="#000" stop-opacity=".72"/><stop offset=".55" stop-color="#000" stop-opacity=".72"/><stop offset="1" stop-color="#000" stop-opacity="0"/></radialGradient>
    <radialGradient id="maxGlow"><stop stop-color="#56D2CA" stop-opacity=".2"/><stop offset="1" stop-color="#56D2CA" stop-opacity="0"/></radialGradient>
    <linearGradient id="maxWash" x1="0" y1="1" x2="0" y2="0"><stop stop-color="#56D2CA" stop-opacity=".4"/><stop offset="1" stop-color="#56D2CA" stop-opacity="0"/></linearGradient>
    <linearGradient id="maxTrail" x1="0" y1="0" x2="0" y2="1"><stop stop-color="#e8fffc" stop-opacity=".6"/><stop offset=".4" stop-color="#56D2CA" stop-opacity=".2"/><stop offset="1" stop-color="#56D2CA" stop-opacity="0"/></linearGradient>
    <radialGradient id="hitGlow"><stop stop-color="#f8fdff" stop-opacity=".85"/><stop offset=".22" stop-color="#56d2ca" stop-opacity=".7"/><stop offset="1" stop-color="#56d2ca" stop-opacity="0"/></radialGradient>
  </defs>`;
  if(t>=0&&t<2.9) {
    if(!reduced&&kind===0) {
      const gather=maxEase(t/.14), release=maxEase((t-.14)/.55);
      const alpha=maxEase(t/.045)*(1-maxSmooth((t-.14)/.65));
      // One contraction and one outward release, with no second flash.
      for(const side of [-1,1]) {
        const x=side*(310-150*gather+230*release);
        for(const row of [-1,0,1]) {
          const y=row*(22+42*release);
          motion+=maxStroke(x,y,x+side*(38-28*release),y+row*12, row===0?3:1.5,alpha*(row===0?1:.65),row===0?'#F8FDFF':'#56D2CA');
        }
      }
      const core=maxEase((t-.14)/.06)*(1-maxSmooth((t-.14)/.4));
      motion+=maxStroke(-180-100*release,0,180+100*release,0,2,core*.45,'#F8FDFF');
      const wash=maxEase((t-.14)/.08)*(1-maxSmooth((t-.14)/.65));
      light=`<path d="M204 475H1076L1120 575H160Z" fill="url(#maxWash)" opacity="${maxNum(wash*.45)}"/>`;
    } else if(!reduced) {
      const rise=maxEase(t/.46), fade=1-maxSmooth((t-.28)/.55);
      for(let i=-1;i<=1;i++) {
        const tipY=575-290*rise+Math.abs(i)*18;
        const x=640+i*(100-20*rise);
        light+=`<path d="M${x-9} ${maxNum(tipY)}H${x+9}L${x+24} 575H${x-24}Z" fill="url(#maxTrail)" opacity="${maxNum(maxEase(t/.08)*fade*.6)}"/>`;
        light+=maxStroke(x,tipY,x,Math.min(575,tipY+40*(1-rise)+8),2,fade*maxEase(t/.06),'#F8FDFF');
      }
      const open=maxEase((t-.28)/.55), alpha=maxEase((t-.28)/.08)*(1-maxSmooth((t-.28)/.65));
      for(const side of [-1,1])for(let i=0;i<4;i++) {
        const x=side*(145+open*(85+i*30)), y=-25-open*(20+i*15);
        motion+=maxStroke(x,y,x-side*(15+10*(1-open)),y+14, i%2?1.5:2.5,alpha*.8,i%2?'#56D2CA':'#F8FDFF');
      }
    }
    for(let i=0;i<3;i++) {
      const x=(i-1)*92;
      const y=reduced?0:kind===0?0:30*(1-maxEase((t-onset)/.34));
      const scale=reduced?1:kind===0?1+.18*(1-maxEase((t-onset)/.28)):1;
      letters+=`<g transform="translate(${x} ${maxNum(y)}) scale(${maxNum(scale)})"><text text-anchor="middle" dominant-baseline="central" font-family="Achievement,sans-serif" font-weight="700" font-size="108" fill="#F8FDFF">${'MAX'[i]}</text></g>`;
    }
    // A short PGREAT-colored baseline holds the composition after the motion clears.
    letters+=maxStroke(-138,72,138,72,2,.65,'#56D2CA');
  }
  const dim=maxEase(t/.16)*exit;
  const overlay=t<0?'':`<g clip-path="url(#maxLane)">${light}</g>
    <ellipse cx="640" cy="285" rx="440" ry="165" fill="url(#maxDim)" opacity="${maxNum(dim)}"/>
    <ellipse cx="640" cy="285" rx="310" ry="100" fill="url(#maxGlow)" opacity="${maxNum(opacity)}"/>
    <g transform="translate(640 285)">${motion}<g opacity="${maxNum(opacity)}">${letters}</g></g>`;
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1280 720" width="100%" height="100%" role="img" aria-label="${maxDirections[kind].name} prototype">${defs}${stage(seconds,true,reduced,false,true)}${overlay}</svg>`;
}
