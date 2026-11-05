// Design study using the built-in playfield's visual vocabulary.
// Palette: JudgementColors.h. Hit geometry: KeybombEffect.h.
// This is a reconstruction, not a native capture or scoring implementation.
export const duration = 3.8;
export const impactTime = 0.65;
export const directions = [
  {name: '01 / Impact', description: 'The final keybomb opens across the judgement line. The achievement settles into the judgement position. Perfect adds a second pulse.'},
  {name: '02 / Sweep', description: 'A short sweep connects the lanes and reveals the achievement. Perfect sends a return sweep through the text.'},
  {name: '03 / Rise', description: 'Light rises briefly from the keys, then clears around the achievement. Perfect resolves with a paired ring at the final note.'},
];
const clamp = (v) => Math.max(0, Math.min(1, v));
const ease = (v) => 1 - Math.pow(1 - clamp(v), 3);
const smooth = (v) => {const q=clamp(v); return q*q*(3-2*q);};
const out = (t,at,length) => ease((t-at)/length);
const f = (v) => Number(v.toFixed(3));
const teal='#56D2CA', lime='#B7F65C', white='#F8FDFF';
const group=(body,opacity=1,transform='')=>`<g opacity="${f(clamp(opacity))}" transform="${transform}">${body}</g>`;
const line=(x1,y1,x2,y2,color,width=2,opacity=1)=>`<path d="M${f(x1)} ${f(y1)}L${f(x2)} ${f(y2)}" fill="none" stroke="${color}" stroke-width="${f(width)}" opacity="${f(clamp(opacity))}"/>`;
const text=(label,x,y,size,color,font='Judgement',anchor='middle')=>`<text x="${x}" y="${y}" text-anchor="${anchor}" dominant-baseline="central" font-family="${font},sans-serif" font-size="${size}" fill="${color}">${label}</text>`;
const ellipse=(x,y,rx,ry,color,width,opacity)=>`<ellipse cx="${f(x)}" cy="${f(y)}" rx="${f(rx)}" ry="${f(ry)}" fill="none" stroke="${color}" stroke-width="${f(width)}" opacity="${f(clamp(opacity))}"/>`;

// Native keybomb ingredients: broken outer ring, inner ring, eight narrow shards,
// soft core and white cross. Bounded, frame-driven geometry, no particle emitter.
function keybomb(t,x,y,radius,color,paired=true) {
  if(t<0||t>=.46)return '';
  const p=clamp(t/.46), expansion=ease(p), fade=1-p*p;
  const r=radius*(.18+expansion*.82);
  let s=`<circle cx="${x}" cy="${y}" r="${f(radius*.8*(1-p*.45))}" fill="url(#hitGlow)" opacity="${f(fade)}"/>`;
  s+=`<circle cx="${x}" cy="${y}" r="${f(r)}" fill="none" stroke="${color}" stroke-width="${f(radius*.045*(1-p*.6))}" pathLength="40" stroke-dasharray="6 4" opacity="${f(fade*.9)}"/>`;
  if(paired)s+=ellipse(x,y,r*.73,r*.73,color,2,fade*.65);
  for(let i=0;i<8;i++) {
    const a=i*Math.PI/4+.23, dx=Math.cos(a),dy=Math.sin(a);
    const d=radius*(.28+expansion*(i%2?.8:1.15));
    const len=radius*(i%2?.18:.28)*(1-p), w=radius*.045*(1-p);
    const cx=x+dx*d,cy=y+dy*d;
    s+=`<path d="M${f(cx+dx*len)} ${f(cy+dy*len)}L${f(cx-dy*w)} ${f(cy+dx*w)}L${f(cx-dx*len)} ${f(cy-dy*len)}L${f(cx+dy*w)} ${f(cy-dx*w)}Z" fill="${i%2?color:white}" opacity="${f(fade)}"/>`;
  }
  const core=radius*.23*(1-p);
  s+=line(x-core*2.5,y,x+core*2.5,y,white,Math.max(.1,core*.22),fade*.8);
  s+=line(x,y-core*2.5,x,y+core*2.5,white,Math.max(.1,core*.22),fade*.8);
  return s;
}

function stage(seconds,perfect,reduced) {
  const completed=seconds>=impactTime;
  let s='<rect width="1280" height="720" fill="#080d13"/><path d="M380 76H900L1120 575H160Z" fill="#0d1013"/>';
  // Lane separators are deliberately faint; the native judgement line is white.
  for(let i=1;i<8;i++)s+=line(380+i*65,76,160+i*120,575,'#1c2128',1,.38);
  s+=line(380,76,900,76,'#cbd0d5',2);
  s+='<path d="M160 575H1120L1130 599H150Z" fill="#f8fdff"/>';
  s+='<rect x="568" y="63" width="144" height="12" fill="#cdd9e1" rx="2"/>';
  s+=text('240',595,50,16,'#f8fdff','MotionCondensed');
  s+=text('2500 ms',668,50,16,lime,'MotionCondensed');
  s+='<rect x="20" y="20" width="348" height="53" fill="#181d27" stroke="#242a34"/>';
  s+=text('Synthetic Built-in 7K',31,37,18,'#e4e7ee','MotionCondensed','start');
  s+=text('NORMAL / HARD',31,58,12,'#adb9cd','MotionCondensed','start');
  s+='<rect x="20" y="650" width="356" height="50" fill="#10151e" stroke="#242a34"/>';
  const combo=completed?812:811;
  const score=perfect?combo*2:combo*2-26;
  s+=text('SCORE '+score,31,667,22,'#e4e7ee','MotionCondensed','start');
  s+=text('COMBO '+combo,365,667,16,lime,'MotionCondensed','end');
  s+=text('PM AAA 1444',198,687,12,'#adb9cd','MotionCondensed');
  s+='<rect x="1144" y="195" width="19" height="331" rx="10" fill="#636a70" stroke="#b5c0cb" stroke-width=".6"/><rect x="1146" y="211" width="15" height="313" rx="8" fill="#e5edf5"/>';
  s+=text('100%',1115,221,12,white);
  s+=line(1141,420,1165,420,'#ffcc51',2);
  const counters=[['PGREAT',perfect?combo:combo-26,teal],['GREAT',perfect?0:26,lime],['GOOD',0,'#ffcc51'],['BAD',0,'#ff8460'],['POOR',0,'#ff6858'],['KPOOR',0,'#ff4e66']];
  counters.forEach(([label,value,color],i)=>{
    const y=232+i*38;
    s+=`<rect x="1197" y="${y}" width="63" height="33" fill="${value?'#182028':'#15171c'}" stroke="${color}" stroke-width=".5" opacity="${value?1:.25}"/>`;
    s+=text(label,1228,y+10,10,value?'#c6d0db':'#3a3e45','MotionCondensed');
    s+=text(String(value),1228,y+24,15,value?white:'#3a3e45','MotionCondensed');
  });
  if(!completed) {
    const p=clamp(seconds/impactTime),y=230+345*p;
    const x=700+(y-76)/499*115,w=30+(y-76)/499*24;
    s+=`<path d="M${f(x-w)} ${f(y)}H${f(x+w)}L${f(x+w+3)} ${f(y+15)}H${f(x-w-3)}Z" fill="#d9d9d9"/>`;
    s+=text('PGREAT 811',640,480,50,teal);
  } else if(!reduced)s+=keybomb(seconds-impactTime,815,587,80,teal);
  return s;
}

function lettering(t,kind,perfect,reduced) {
  const delay=kind===2?.12:.04,p=out(t,delay,.18);
  const alpha=p*(1-smooth((t-1.75)/.35));
  if(alpha<=0)return '';
  const scale=reduced?1:1+.075*(1-p);
  const y=480+(reduced?0:kind===2?16*(1-p):0);
  const color=perfect?teal:lime;
  const word=perfect?'PERFECT':'FULL COMBO';
  const label=text(word,0,0,perfect?70:64,color);
  let s=label;
  if(kind===1&&!reduced) {
    const reveal=out(t,.04,.34);
    s=`<clipPath id="textReveal"><rect x="-270" y="-55" width="${f(540*reveal)}" height="110"/></clipPath><g clip-path="url(#textReveal)">${label}</g>`;
  }
  // Perfect's return sweep is short, white, and confined to the actual letters.
  if(perfect&&!reduced&&t>.4&&t<.9) {
    const sweep=1-out(t,.4,.5);
    s+=`<clipPath id="textSweep"><rect x="${f(-280+560*sweep)}" y="-55" width="35" height="110"/></clipPath><g clip-path="url(#textSweep)">${text(word,0,0,70,white)}</g>`;
  }
  return group(s,alpha,`translate(640 ${f(y)}) scale(${f(scale)})`);
}

function impact(t,perfect) {
  const color=perfect?teal:lime;
  const p=out(t,.02,.38),fade=1-out(t,.14,.5);
  let s=line(640-480*p,587,640+480*p,587,color,7,fade*.8);
  s+=ellipse(640,587,25+440*p,4+60*p,color,2,fade*.7);
  if(perfect)s+=ellipse(640,587,25+440*out(t,.24,.5),4+60*out(t,.24,.5),teal,2,out(t,.24,.04)*(1-out(t,.3,.5)));
  for(let i=0;i<12;i++) {
    const q=out(t,.04+(i%3)*.018,.55),x=330+i*56;
    s+=line(x,566-q*(24+i%3*18),x,552-q*(28+i%3*18),color,2,(1-q)*p);
  }
  return s;
}

function sweep(t,perfect) {
  const color=perfect?teal:lime;
  let s='';
  for(let i=0;i<8;i++) {
    const age=t-i*.035;
    if(age<0||age>.36)continue;
    const x=220+i*120,q=out(age,0,.36);
    s+=line(x-38,586,x+38,586,color,6,1-q);
    s+=line(x,573,x,573-48*q,color,2,1-q);
  }
  if(perfect) {
    const q=out(t,.35,.5);
    s+=ellipse(815,587,10+120*q,3+35*q,teal,2,out(t,.35,.04)*(1-q));
  }
  return s;
}

function rise(t,perfect) {
  let s='';
  const color=perfect?teal:lime;
  for(let i=0;i<8;i++) {
    const age=t-Math.abs(i-4)*.018;
    if(age<0||age>.65)continue;
    const q=out(age,0,.65),x=220+i*120;
    const height=60+55*(1-Math.abs(i-3.5)/4);
    s+=`<path d="M${x-25} 574H${x+25}L${f(x+18)} ${f(574-height*q)}H${f(x-18)}Z" fill="url(#laneLight)" opacity="${f((1-q)*.8)}"/>`;
    s+=line(x,570-height*q,x,582-height*q,color,2,(1-q)*.8);
  }
  if(perfect)s+=keybomb(t-.24,815,587,105,teal);
  return s;
}

export function sceneSvg(seconds,kind=0,perfect=false,reduced=false) {
  const t=seconds-impactTime,color=perfect?teal:lime;
  const defs=`<defs><radialGradient id="hitGlow"><stop stop-color="#f8fdff" stop-opacity=".85"/><stop offset=".22" stop-color="#56d2ca" stop-opacity=".7"/><stop offset="1" stop-color="#56d2ca" stop-opacity="0"/></radialGradient><linearGradient id="laneLight" x2="0" y2="1"><stop stop-color="${color}" stop-opacity="0"/><stop offset="1" stop-color="${color}" stop-opacity=".7"/></linearGradient></defs>`;
  let s=stage(seconds,perfect,reduced);
  if(t>=0&&t<2.2) {
    if(!reduced)s+=[impact,sweep,rise][kind](t,perfect);
    s+=lettering(t,kind,perfect,reduced);
  }
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1280 720" width="100%" height="100%" role="img" aria-label="${directions[kind].name}, ${perfect?'Perfect':'Full Combo'} animation">${defs}${s}</svg>`;
}
