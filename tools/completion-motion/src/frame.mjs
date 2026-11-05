// Design study using the built-in playfield's visual vocabulary.
// Palette: JudgementColors.h. Hit geometry: KeybombEffect.h.
// This is a reconstruction, not a native capture or scoring implementation.
export const duration = 3.8;
export const impactTime = 0.65;
// Independent presentation anchor; never derived from judgement text/line settings.
const achievementX = 640, achievementY = 285;
export const directions = [
  {name: '01 / Impact', description: 'Two strokes converge, the lettering lands, and short shards release outward. Perfect answers with a second expanding pulse.'},
  {name: '02 / Sweep', description: 'A quick pass reveals the lettering in sequence. Fine trails clear outward; Perfect adds a return glint and a wider finish.'},
  {name: '03 / Rise', description: 'The lettering lifts into place as narrow trails fan upward. Perfect adds a brief burst above the title.'},
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

// All achievement geometry uses local coordinates around the independent anchor.
function lettering(t,kind,perfect,reduced) {
  const delay=.08;
  const exit=smooth((t-2.05)/.4);
  const word=perfect?'PERFECT':'FULL COMBO';
  const size=perfect?82:74, advance=perfect?58:52;
  const widths=[...word].map(c=>c===' '?26:advance);
  const totalWidth=widths.reduce((sum,width)=>sum+width,0);
  const color=perfect?teal:lime;
  let s='';
  for(let i=0;i<word.length;i++) {
    if(word[i]===' ')continue;
    const stagger=reduced?0:kind===1?i*.022:Math.abs(i-(word.length-1)/2)*.009;
    const age=t-delay-stagger, p=out(age,0,.23);
    if(age<=0)continue;
    const settle=reduced?0:Math.sin(clamp(age/.48)*Math.PI)*Math.exp(-age*5);
    const x=-totalWidth/2+widths.slice(0,i).reduce((sum,width)=>sum+width,0)+widths[i]/2;
    const y=reduced?0:(kind===2?30:12)*(1-p)-5*settle;
    const scale=reduced?1:1+.1*settle;
    const glyph=`<text text-anchor="middle" dominant-baseline="central" font-family="Achievement,sans-serif" font-weight="700" font-size="${size}" fill="${white}">${word[i]}</text>`;
    s+=group(glyph,p*(1-exit),`translate(${f(x)} ${f(y)}) scale(${f(scale)})`);
  }
  // One short accent below the word anchors its weight without a persistent badge.
  const width=(perfect?202:235)*out(t,.16,.35);
  s+=line(-width,57,width,57,color,2,out(t,.16,.12)*(1-smooth((t-1.1)/.65)));
  // A narrow highlight crosses the actual lettering, not a screen-sized flare.
  if(perfect&&!reduced&&t>.55&&t<1.1) {
    const x=-270+540*smooth((t-.55)/.55);
    s+=`<clipPath id="glint"><rect x="${f(x)}" y="-54" width="27" height="108"/></clipPath><g clip-path="url(#glint)">${group(s.replaceAll(`fill="${white}"`, `fill="${color}"`),.9)}</g>`;
  }
  return s;
}

function shard(x,y,length,width,angle,color,opacity) {
  return group(`<path d="M${-length} 0L0 ${-width}L${length} 0L0 ${width}Z" fill="${color}"/>`,opacity,`translate(${f(x)} ${f(y)}) rotate(${f(angle)})`);
}

function fragments(t,perfect,kind) {
  let s='';
  const color=perfect?teal:lime;
  const count=perfect?16:10;
  for(let i=0;i<count;i++) {
    const age=t-.18-(i%4)*.035;
    if(age<0||age>1.2)continue;
    const q=out(age,0,1.2),a=i/count*Math.PI*2+.2;
    const x=Math.cos(a)*(190+q*(80+i%3*22));
    const y=Math.sin(a)*(40+q*(50+i%4*12))-(kind===2?q*60:0);
    const opacity=Math.sin(clamp(age/.12)*Math.PI/2)*(1-q)*.85;
    s+=shard(x,y,(i%3===0?11:5)*(1-q)+1,1.2,a*180/Math.PI,i%3===0?white:color,opacity);
  }
  return s;
}

function impact(t,perfect) {
  const color=perfect?teal:lime;
  let s='';
  const p=out(t,0,.24), release=out(t,.22,.55);
  for(const side of [-1,1]) {
    const x=side*(320-85*p+110*release);
    s+=shard(x,0,42*(1-release)+4,3,0,color,(1-release)*p);
    s+=line(x-side*50,-16,x-side*15,-16,white,1,(1-release)*p*.6);
  }
  const q=out(t,.1,.65);
  s+=ellipse(0,0,150+190*q,28+66*q,color,1.5,out(t,.1,.05)*(1-q)*.7);
  if(perfect) {
    const r=out(t,.48,.65);
    s+=ellipse(0,0,190+180*r,42+80*r,teal,1.5,out(t,.48,.05)*(1-r)*.8);
  }
  return s+fragments(t,perfect,0);
}

function sweep(t,perfect) {
  const color=perfect?teal:lime;
  let s='';
  for(let i=0;i<2;i++) {
    const q=out(t,.03+i*.07,.5),y=i===0?-52:57;
    const x=-290+580*q;
    s+=line(x-100*(1-q),y,x,y,color,i===0?2:3,Math.sin(q*Math.PI));
    s+=shard(x,y,12,2,0,white,Math.sin(q*Math.PI));
  }
  if(perfect) {
    const q=out(t,.55,.5),x=290-580*q;
    s+=line(x,-52,x+70*(1-q),-52,teal,2,Math.sin(q*Math.PI));
  }
  return s+fragments(t,perfect,1);
}

function rise(t,perfect) {
  const color=perfect?teal:lime;
  let s='';
  for(let i=0;i<7;i++) {
    const age=t-Math.abs(i-3)*.025;
    if(age<0||age>.7)continue;
    const q=out(age,0,.7),x=(i-3)*(60+q*12),y=90-q*(170-Math.abs(i-3)*14);
    s+=line(x,y+25*(1-q),x,y,color,2,Math.sin(q*Math.PI)*.7);
    s+=shard(x,y,8*(1-q),1,90,white,Math.sin(q*Math.PI)*.6);
  }
  if(perfect) {
    const q=out(t,.48,.65);
    for(let i=0;i<6;i++) {
      const a=i*Math.PI/3, r=10+q*46;
      s+=shard(Math.cos(a)*r,-87+Math.sin(a)*r*.6,7*(1-q)+1,1.4,a*180/Math.PI,teal,out(t,.48,.06)*(1-q));
    }
  }
  return s+fragments(t,perfect,2);
}

export function sceneSvg(seconds,kind=0,perfect=false,reduced=false) {
  const t=seconds-impactTime,color=perfect?teal:lime;
  const defs=`<defs><radialGradient id="achievementGlow"><stop stop-color="${color}" stop-opacity=".12"/><stop offset="1" stop-color="${color}" stop-opacity="0"/></radialGradient><radialGradient id="hitGlow"><stop stop-color="#f8fdff" stop-opacity=".85"/><stop offset=".22" stop-color="#56d2ca" stop-opacity=".7"/><stop offset="1" stop-color="#56d2ca" stop-opacity="0"/></radialGradient><linearGradient id="laneLight" x2="0" y2="1"><stop stop-color="${color}" stop-opacity="0"/><stop offset="1" stop-color="${color}" stop-opacity=".7"/></linearGradient></defs>`;
  let s=stage(seconds,perfect,reduced);
  if(t>=0&&t<2.5) {
    let effect='';
    // Local low-opacity light separates the title from a busy BGA; no panel.
    const haze=out(t,0,.22)*(1-smooth((t-1.5)/.8));
    if(!reduced)effect+=`<ellipse cx="0" cy="0" rx="330" ry="125" fill="url(#achievementGlow)" opacity="${f(haze)}"/>`;
    if(!reduced)effect+=[impact,sweep,rise][kind](t,perfect);
    effect+=lettering(t,kind,perfect,reduced);
    s+=group(effect,1,`translate(${achievementX} ${achievementY})`);
  }
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1280 720" width="100%" height="100%" role="img" aria-label="${directions[kind].name}, ${perfect?'Perfect':'Full Combo'} animation">${defs}${s}</svg>`;
}
