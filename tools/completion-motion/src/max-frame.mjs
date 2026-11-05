// MAX studies: all-PGREAT completion. Authoring only; no native scoring changes.
import {stage, impactTime} from './frame.mjs';
import {laneSceneSvg} from './lane-frame.mjs';
export const maxDirections = [
  {name:'01 / MAX · Focus', description:'A decisive MAX burst: large white lettering, a brilliant star, wide teal/gold rings, and 24 flying shards. The lane wash and title strike together.'},
  {name:'02 / MAX · Lift', description:'Seven trails launch into an upward celebration of white, teal, and gold. Large MAX lettering and a brilliant star hold as the shards fall away.'},
  {name:'03 / Perfect', description:'The existing gold Perfect is the reference. MAX uses an all-PGREAT sample; this Perfect sample includes GREATs.'},
];
const maxClamp = v => Math.max(0,Math.min(1,v));
const maxEase = v => 1-(1-maxClamp(v))**3;
const maxSmooth = v => {const q=maxClamp(v);return q*q*(3-2*q);};
const maxNum = v => Number(v.toFixed(3));
const maxStroke = (x,y,xx,yy,width,alpha,color='#56D2CA') => `<path d="M${maxNum(x)} ${maxNum(y)}L${maxNum(xx)} ${maxNum(yy)}" fill="none" stroke="${color}" stroke-width="${width}" opacity="${maxNum(maxClamp(alpha))}"/>`;
const maxShard = (x,y,length,width,angle,color,alpha) => `<path d="M${-length} 0L0 ${-width}L${length} 0L0 ${width}Z" transform="translate(${maxNum(x)} ${maxNum(y)}) rotate(${maxNum(angle)})" fill="${color}" opacity="${maxNum(maxClamp(alpha))}"/>`;
export function maxSceneSvg(seconds,kind=0,_perfect=false,reduced=false) {
  void _perfect;
  if(kind===2)return laneSceneSvg(seconds,0,true,reduced);
  const t=seconds-impactTime;
  const exit=1-maxSmooth((t-2.55)/.5);
  const onset=kind===0?.14:.08;
  const entrance=maxEase((t-onset)/.2);
  const opacity=entrance*exit;
  let light='', motion='', letters='', mark='';
  const defs=`<defs>
    <clipPath id="maxLane"><path d="M380 76H900L1120 575H160Z"/></clipPath>
    <radialGradient id="maxDim"><stop stop-color="#000" stop-opacity=".72"/><stop offset=".55" stop-color="#000" stop-opacity=".72"/><stop offset="1" stop-color="#000" stop-opacity="0"/></radialGradient>
    <radialGradient id="maxGlow"><stop stop-color="#56D2CA" stop-opacity=".3"/><stop offset="1" stop-color="#56D2CA" stop-opacity="0"/></radialGradient>
    <radialGradient id="maxStarGlow"><stop stop-color="#fff3bf" stop-opacity=".5"/><stop offset=".3" stop-color="#FFCC51" stop-opacity=".16"/><stop offset="1" stop-color="#FFCC51" stop-opacity="0"/></radialGradient>
    <linearGradient id="maxWash" x1="0" y1="1" x2="0" y2="0"><stop stop-color="#56D2CA" stop-opacity="0"/><stop offset=".45" stop-color="#56D2CA" stop-opacity=".5"/><stop offset=".75" stop-color="#e8fffc" stop-opacity=".7"/><stop offset="1" stop-color="#56D2CA" stop-opacity="0"/></linearGradient>
    <linearGradient id="maxTrail" x1="0" y1="0" x2="0" y2="1"><stop stop-color="#e8fffc" stop-opacity=".9"/><stop offset=".4" stop-color="#56D2CA" stop-opacity=".35"/><stop offset="1" stop-color="#56D2CA" stop-opacity="0"/></linearGradient>
    <radialGradient id="hitGlow"><stop stop-color="#f8fdff" stop-opacity=".85"/><stop offset=".22" stop-color="#56d2ca" stop-opacity=".7"/><stop offset="1" stop-color="#56d2ca" stop-opacity="0"/></radialGradient>
  </defs>`;
  if(t>=0&&t<3.05) {
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
      const core=maxEase((t-.14)/.04)*(1-maxSmooth((t-.14)/.5));
      motion+=maxStroke(-230-230*release,0,230+230*release,0,5*(1-release)+1,core*.8,'#F8FDFF');
      const wave=maxEase((t-.14)/.8), wash=maxEase((t-.14)/.06)*(1-maxSmooth((t-.55)/.4));
      const top=575-600*wave;
      light=`<rect x="150" y="${maxNum(top)}" width="980" height="290" fill="url(#maxWash)" opacity="${maxNum(wash)}"/>`;
    } else if(!reduced) {
      const rise=maxEase(t/.5), fade=1-maxSmooth((t-.35)/.65);
      for(let i=-3;i<=3;i++) {
        const tipY=575-350*rise+Math.abs(i)*24;
        const x=640+i*(88-14*rise);
        light+=`<path d="M${x-11} ${maxNum(tipY)}H${x+11}L${x+30} 575H${x-30}Z" fill="url(#maxTrail)" opacity="${maxNum(maxEase(t/.08)*fade*.85)}"/>`;
        light+=maxStroke(x,tipY,x,Math.min(575,tipY+55*(1-rise)+12),3,fade*maxEase(t/.06),i%2?'#FFCC51':'#F8FDFF');
      }
    }
    if(!reduced) {
      const burstAge=t-(kind===0?.14:.18), q=maxEase(burstAge/1.25);
      const burstAlpha=maxEase(burstAge/.06)*(1-maxSmooth(burstAge/1.4));
      for(let i=0;i<24;i++) {
        const a=i/24*Math.PI*2+.13;
        const x=Math.cos(a)*(170+q*(235+i%3*22));
        const y=kind===0?Math.sin(a)*(55+q*155):
          -20-Math.abs(Math.sin(a))*(35+q*195)+q*q*100;
        const color=i%3===0?'#F8FDFF':i%3===1?'#56D2CA':'#FFCC51';
        motion+=maxShard(x,y,(i%3===0?27:18)*(1-q)+4,i%3===0?4:2.5,
          a*180/Math.PI+(kind===1?q*90:0),color,burstAlpha);
      }
      const ring=maxEase(burstAge/.75), ringAlpha=maxEase(burstAge/.035)*(1-maxSmooth(burstAge/.95));
      for(let i=0;i<2;i++) {
        motion+=`<ellipse cx="0" cy="${kind===1?-30*ring:0}" rx="${maxNum(150+ring*(i===0?340:290))}" ry="${maxNum(45+ring*(i===0?120:165))}" fill="none" stroke="${i===0?'#56D2CA':'#FFCC51'}" stroke-width="${maxNum((i===0?6:3)*(1-ring)+1)}" opacity="${maxNum(ringAlpha*(i===0?.9:.65))}"/>`;
      }
    }
    const starScale=reduced?1:1+.3*(1-maxEase((t-onset)/.4));
    const starSpread=reduced?0:16*(1-entrance);
    for(let i=0;i<4;i++) {
      mark+=`<g transform="rotate(${i*90}) translate(0 ${maxNum(-starSpread)})"><path d="M0 -43L9 -11L0 0L-9 -11Z" fill="${i%2?'#FFCC51':'#F8FDFF'}"/></g>`;
      mark+=`<g transform="rotate(${i*90+45})"><path d="M0 -31L4 -12L0 -7L-4 -12Z" fill="#56D2CA"/></g>`;
    }
    mark=`<g transform="translate(0 -118) scale(${maxNum(starScale)})">${mark}</g>`;
    for(let i=0;i<3;i++) {
      const x=(i-1)*114;
      const y=reduced?0:kind===0?0:40*(1-maxEase((t-onset)/.34));
      const scale=reduced?1:1+(kind===0?.24:.12)*(1-maxEase((t-onset)/.32));
      letters+=`<g transform="translate(${x} ${maxNum(y)}) scale(${maxNum(scale)})"><text text-anchor="middle" dominant-baseline="central" font-family="Achievement,sans-serif" font-weight="700" font-size="140" fill="#F8FDFF">${'MAX'[i]}</text></g>`;
    }
    // A short PGREAT-colored baseline holds the composition after the motion clears.
    letters+=maxStroke(-175,86,175,86,3,.8,'#56D2CA');
    letters+=maxShard(-188,86,6,3,0,'#FFCC51',.9)+maxShard(188,86,6,3,0,'#FFCC51',.9);
  }
  const dim=maxEase(t/.16)*exit;
  const overlay=t<0?'':`<g clip-path="url(#maxLane)">${light}</g>
    <ellipse cx="640" cy="255" rx="490" ry="215" fill="url(#maxDim)" opacity="${maxNum(dim)}"/>
    <ellipse cx="640" cy="285" rx="350" ry="130" fill="url(#maxGlow)" opacity="${maxNum(opacity)}"/>
    <ellipse cx="640" cy="167" rx="105" ry="100" fill="url(#maxStarGlow)" opacity="${maxNum(opacity)}"/>
    <g transform="translate(640 285)">${motion}<g opacity="${maxNum(opacity)}">${mark}${letters}</g></g>`;
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1280 720" width="100%" height="100%" role="img" aria-label="${maxDirections[kind].name} prototype">${defs}${stage(seconds,true,reduced,false,true)}${overlay}</svg>`;
}
