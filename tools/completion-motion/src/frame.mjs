// One deterministic scene model for the standalone reviewer and Remotion.
// Times are seconds; the simulated final judgement lands at 0.65 s.
export const duration = 3.8;
export const impactTime = 0.65;
export const directions = [
  {name: '01 / Breakthrough', description: 'A split-chevron strike. The title locks into place; Perfect adds a second, wider fracture.'},
  {name: '02 / Supernova', description: 'A circular bloom with buoyant lettering. Perfect crowns the ring with a starburst.'},
  {name: '03 / Ascension', description: 'Two ribbons rise from the judgement line and resolve into a luminous seal. Perfect adds a halo.'},
];
const clamp = (v) => Math.max(0, Math.min(1, v));
const ease = (v) => 1 - Math.pow(1 - clamp(v), 4);
const smooth = (v) => { const q = clamp(v); return q * q * (3 - 2 * q); };
const out = (t, at, length) => ease((t - at) / length);
const f = (v) => Number(v.toFixed(3));
const colors = ['#5cf5ff', '#ff70dc', '#ffe880', '#9a8aff', '#91ffcf', '#ffad8d'];
const line = (x1,y1,x2,y2,color,width=2,opacity=1) => `<path d="M${f(x1)} ${f(y1)}L${f(x2)} ${f(y2)}" fill="none" stroke="${color}" stroke-width="${width}" opacity="${f(opacity)}"/>`;
const circle = (x,y,r,color,width=2,opacity=1) => `<circle cx="${f(x)}" cy="${f(y)}" r="${f(Math.max(0,r))}" fill="none" stroke="${color}" stroke-width="${width}" opacity="${f(opacity)}"/>`;
const star = (x,y,r,color,rotation=0) => `<path d="M0 ${-r} Q${r*.18} ${-r*.18} ${r} 0 Q${r*.18} ${r*.18} 0 ${r} Q${-r*.18} ${r*.18} ${-r} 0 Q${-r*.18} ${-r*.18} 0 ${-r}Z" fill="${color}" transform="translate(${f(x)} ${f(y)}) rotate(${f(rotation)})"/>`;
const group = (body,opacity=1,transform='') => `<g opacity="${f(clamp(opacity))}" transform="${transform}">${body}</g>`;

function stage(seconds, reduced) {
  let s = `<rect width="1280" height="720" fill="#080c13"/><path d="M385 85H895L1100 590H180Z" fill="url(#lane)"/>`;
  for (let i=0;i<9;i++) {
    const x=385+i*510/8;
    s += line(x,85,180+i*920/8,590,'#273140',i===0||i===8?2:1,.75);
  }
  for(let i=0;i<7;i++) {
    const y=125+i*i*11;
    const spread=(y-85)/505*205;
    s += line(385-spread,y,895+spread,y,'#242d3a',1,.36);
  }
  s += line(180,590,1100,590,'#b9dcf0',3,.8);
  s += '<g font-family="MotionCondensed,Arial,sans-serif" fill="#8390a7"><text x="36" y="45" font-size="20" letter-spacing="3">ASOBMASHOW</text><text x="36" y="70" font-size="13" letter-spacing="2">MOTION STUDY / SYNTHETIC PLAYFIELD</text><text x="1244" y="45" font-size="16" text-anchor="end">7 KEY</text><text x="1244" y="70" font-size="13" text-anchor="end">FINAL JUDGEMENT AT 0.65s</text></g>';
  if(seconds<impactTime) {
    const y=220+370*smooth(seconds/impactTime);
    const w=40+(y-85)*.065;
    s += `<rect x="${640-w}" y="${f(y-9)}" width="${2*w}" height="9" rx="2" fill="#d9faff"/>`;
  }
  const hit=seconds-impactTime;
  if(hit>=0&&hit<.45) s+=group(circle(640,590,20+out(hit,0,.45)*130,'#c4faff',3), (1-hit/.45)*(reduced?.2:1));
  return s;
}

function lettering(t,kind,perfect,reduced) {
  const delay=kind===2?.27:.14;
  const p=out(t,delay,.33);
  const exit=smooth((t-2.25)/.45);
  if(p===0||exit===1)return '';
  const word=perfect?'PERFECT':'FULL COMBO';
  const size=perfect?106:94;
  const center=kind===2?300:330;
  let scale=reduced?1:(kind===1?1+Math.sin(clamp((t-delay)/.5)*Math.PI)*.16:1+.22*(1-p));
  const y=reduced?0:(kind===2?32*(1-p):kind===1?-25*(1-p):0);
  const skew=kind===0?-9:0;
  let text='';
  const face=kind===1?'url(#candy)':'url(#metal)';
  const main=`<text text-anchor="middle" dominant-baseline="central" font-family="MotionCondensed,Arial,sans-serif" font-size="${size}" font-weight="900" letter-spacing="${perfect?8:3}" paint-order="stroke" stroke-linejoin="round">${word}</text>`;
  text+=`<g fill="#080e28" stroke="#080e28" stroke-width="18" transform="translate(0 7)">${main}</g>`;
  text+=`<g fill="none" stroke="${kind===1?'#fff3bd':'#b6fcff'}" stroke-width="7">${main}</g>`;
  text+=`<g fill="${face}" stroke="#fff" stroke-width="1">${main}</g>`;
  if(!reduced&&kind===0&&t<.42)text+=`<g fill="none" stroke="#ff58c7" stroke-width="2" opacity="${f((1-p)*.8)}" transform="translate(${f((1-p)*-85)} 0)">${main}</g>`;
  const underline=out(t,delay+.15,.5);
  text+=line(-210*underline,69,210*underline,69,perfect?'#ffe9a8':'#75f9ef',2,.8);
  text+=`<text y="99" text-anchor="middle" fill="#dce9f6" font-family="MotionCondensed,Arial,sans-serif" font-size="17" letter-spacing="7" opacity="${f(out(t,delay+.25,.3))}">${perfect?'EVERY NOTE. ABSOLUTE.':'NOT A SINGLE BREAK.'}</text>`;
  return group(text,p*(1-exit),`translate(640 ${f(center+y)}) scale(${f(scale)}) skewX(${skew})`);
}

function breakthrough(t,perfect) {
  let s='';
  const p=out(t,0,.32), tail=1-smooth((t-1.4)/.8);
  // Converging brackets give the impact a direction rather than a generic pop.
  for(const side of [-1,1]) {
    const distance=270+180*(1-p);
    s+=group(`<path d="M0 -70L${side*55} 0L0 70L${side*18} 70L${side*75} 0L${side*18} -70Z" fill="${side<0?'#ff6ed8':'#60edff'}"/>`,p*tail,`translate(${640-side*distance} 330)`);
  }
  for(let i=0;i<22;i++) {
    const a=i/22*Math.PI*2+.1;
    const travel=out(t,.08+(i%3)*.025,.68);
    const r=120+travel*420, len=20+70*(1-travel);
    s+=line(640+Math.cos(a)*r,330+Math.sin(a)*r*.6,640+Math.cos(a)*(r+len),330+Math.sin(a)*(r+len)*.6,colors[i%2],i%3===0?4:1,Math.sin(travel*Math.PI)*.8);
  }
  s+=group(`<path d="M250 330L430 240H850L1030 330L850 420H430Z" fill="#162742" fill-opacity=".65" stroke="#72eefa" stroke-width="1"/>`,p*.6*tail);
  const shock=out(t,0,.65);
  s+=group(`<ellipse cx="640" cy="590" rx="${f(40+shock*490)}" ry="${f(4+shock*75)}" fill="none" stroke="#75efff" stroke-width="3"/>`,1-shock);
  if(perfect) {
    const q=out(t,.44,.7);
    s+=group(`<path d="M640 ${330-230*q}L${640+450*q} 330L640 ${330+230*q}L${640-450*q} 330Z" fill="none" stroke="#ffe5a6" stroke-width="2"/>`,Math.sin(q*Math.PI));
    s+=group(star(640,210,25,'#fff5ca'),out(t,.5,.2)*tail);
  }
  return s;
}

function supernova(t,perfect) {
  let s='';
  const reveal=out(t,0,.52), tail=1-smooth((t-1.7)/.6);
  for(let i=0;i<12;i++) {
    const a=i*Math.PI/6+t*.12;
    const r=150+reveal*50;
    const start=a+.04, end=a+Math.PI/6-.05;
    s+=`<path d="M${f(640+Math.cos(start)*r)} ${f(330+Math.sin(start)*r)}A${f(r)} ${f(r)} 0 0 1 ${f(640+Math.cos(end)*r)} ${f(330+Math.sin(end)*r)}" stroke="${colors[i%6]}" stroke-width="${perfect?9:5}" fill="none" opacity="${f(reveal*.8*tail)}"/>`;
  }
  s+=circle(640,330,182*reveal,'#fff3bc',1,reveal*.4*tail);
  for(let i=0;i<32;i++) {
    const a=i*2.39996;
    const q=out(t,.12+(i%5)*.035,.8+(i%3)*.12);
    const r=90+q*(140+(i%4)*34);
    const x=640+Math.cos(a)*r*1.35, y=330+Math.sin(a)*r+q*q*35;
    const opacity=Math.sin(q*Math.PI)*tail;
    s+=group(star(x,y,4+(i%4)*3,colors[i%6],i*15+t*30),opacity);
  }
  if(perfect) {
    const q=out(t,.48,.5);
    for(let i=0;i<8;i++) {
      const a=i*Math.PI/4-Math.PI/2;
      s+=group(star(640+Math.cos(a)*231,330+Math.sin(a)*231,16*q,'#fff3b0',45),q*tail);
    }
    s+=circle(640,330,205+out(t,.45,.7)*100,'#fff3b0',2,(1-out(t,.45,.7))*out(t,.45,.1));
  }
  return s;
}

function ascension(t,perfect) {
  let s='';
  const tail=1-smooth((t-1.8)/.65);
  for(let i=0;i<6;i++) {
    const p=out(t,i*.035,.65);
    const side=i%2?-1:1;
    // Ribbons lift from the strike line, then peel apart above the title.
    s+=`<path d="M640 590C${640+side*(290+i*12)} 490 ${640+side*(350-i*10)} 200 640 160" fill="none" stroke="${colors[i]}" stroke-width="${i<2?5:2}" pathLength="1" stroke-dasharray="${f(p*.75)} 1" stroke-dashoffset="${f(-out(t,.55,.7)*.85)}" opacity="${f(p*(1-out(t,.65,.8)))}"/>`;
  }
  const seal=out(t,.25,.6);
  for(let j=0;j<3;j++) {
    s+=`<ellipse cx="640" cy="310" rx="${f((200+j*14)*seal)}" ry="${f((78+j*15)*seal)}" fill="none" stroke="${colors[j]}" stroke-width="${j===0?3:1}" transform="rotate(${j*60-30} 640 310)" opacity="${f(seal*.45*tail)}"/>`;
  }
  for(let i=0;i<20;i++) {
    const q=out(t,.15+(i%5)*.065,1.2);
    const x=640+Math.sin(i*8.2)*(100+q*310);
    const y=570-q*(270+(i%4)*75);
    s+=group(star(x,y,3+i%5,colors[i%6],i*20),Math.sin(q*Math.PI)*.8);
  }
  if(perfect) {
    const q=out(t,.6,.5);
    s+=group(star(640,165,26,'#fff6c5'),q*tail);
    s+=`<ellipse cx="640" cy="175" rx="${f(90*q)}" ry="${f(22*q)}" fill="none" stroke="#fff0ae" stroke-width="3" opacity="${f(q*tail)}"/>`;
  }
  return s;
}

export function sceneSvg(seconds,kind=0,perfect=false,reduced=false) {
  const t=seconds-impactTime;
  const defs=`<defs><linearGradient id="lane" x2="0" y2="1"><stop stop-color="#121929"/><stop offset="1" stop-color="#0c1019"/></linearGradient><linearGradient id="metal" x2="0.2" y2="1"><stop stop-color="#fff"/><stop offset=".46" stop-color="#f4faff"/><stop offset=".5" stop-color="#93d7fb"/><stop offset="1" stop-color="#e4ffff"/></linearGradient><linearGradient id="candy"><stop stop-color="#ffd97d"/><stop offset=".35" stop-color="#fff6c2"/><stop offset=".7" stop-color="#ffc6f5"/><stop offset="1" stop-color="#90eeff"/></linearGradient><radialGradient id="aura"><stop stop-color="${kind===1?'#704b9e':'#23688c'}" stop-opacity=".48"/><stop offset="1" stop-color="#101525" stop-opacity="0"/></radialGradient></defs>`;
  let s=stage(seconds,reduced);
  if(t>=0&&t<2.9) {
    const fade=out(t,0,.2)*(1-smooth((t-2.15)/.55));
    s+=`<ellipse cx="640" cy="330" rx="510" ry="290" fill="url(#aura)" opacity="${f(fade)}"/>`;
    if(!reduced)s+=[breakthrough,supernova,ascension][kind](t,perfect);
    s+=lettering(t,kind,perfect,reduced);
  }
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1280 720" width="100%" height="100%" role="img" aria-label="${directions[kind].name}, ${perfect?'Perfect':'Full Combo'} animation">${defs}${s}</svg>`;
}
