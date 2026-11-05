import {readFileSync, writeFileSync, mkdirSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import {dirname, resolve} from 'node:path';
const root = dirname(fileURLToPath(import.meta.url));
const code = readFileSync(resolve(root,'src/frame.mjs'),'utf8').replaceAll('export ', '');
const font = readFileSync(resolve(root,'../../assets/fonts/roboto-condensed-bold.ttf')).toString('base64');
const judgementFont = readFileSync(resolve(root,'src/fonts/judgement-ascii.ttf')).toString('base64');
const achievementFont = readFileSync(resolve(root,'src/fonts/judgement-bold-ascii.otf')).toString('base64');
const html = `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Completion motion studies</title>
<style>
:root{color-scheme:dark;--foreground:#edf0f7;--muted-foreground:#99a4b6;--card:#141b27;--border:#303b50;--primary:#abeaf6;--primary-foreground:#0a1520;--radius:12px;--font-sans:system-ui,sans-serif}
@font-face{font-family:MotionCondensed;src:url(data:font/ttf;base64,${font}) format('truetype');font-weight:900}
@font-face{font-family:Judgement;src:url(data:font/ttf;base64,${judgementFont}) format('truetype')}
@font-face{font-family:Achievement;src:url(data:font/otf;base64,${achievementFont}) format('opentype');font-weight:700}
*{box-sizing:border-box}body{margin:0;color:var(--foreground);font-family:var(--font-sans)}p{color:var(--muted-foreground);font-size:13px;line-height:1.55}button,select{font:inherit;font-size:12px;color:var(--foreground);background:var(--card);border:1px solid var(--border);border-radius:8px;padding:10px 12px;cursor:pointer}button[aria-pressed=true]{color:var(--primary-foreground);background:var(--primary);border-color:var(--primary)}button:focus-visible,select:focus-visible,input:focus-visible{outline:2px solid var(--primary);outline-offset:3px}.directions{display:flex;gap:8px;flex-wrap:wrap;margin:0 0 12px}.directions button{flex:1;white-space:nowrap;text-align:left;padding:14px 12px}.stage{width:100%;aspect-ratio:16/9;overflow:hidden;border-radius:var(--radius);background:var(--card);border:1px solid var(--border)}.stage svg{display:block}.controls{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-top:12px}.controls span{flex:1}.timeline{display:flex;gap:10px;align-items:center;margin-top:14px;font:12px ui-monospace,monospace;color:var(--muted-foreground)}input[type=range]{flex:1;min-width:90px;accent-color:var(--primary)}.small{font-size:11px;margin-bottom:0}.mode{display:flex;gap:6px}#description{min-height:40px;margin:12px 0 0}@media(max-width:450px){.directions button{font-size:11px;padding:12px 8px}.controls span{display:none}}
</style></head><body>
<div class="directions" aria-label="Animation direction"><button data-kind="0" aria-pressed="true">01 / Impact</button><button data-kind="1" aria-pressed="false">02 / Sweep</button><button data-kind="2" aria-pressed="false">03 / Rise</button></div>
<div class="stage" id="stage"></div>
<p id="description"></p>
<div class="controls"><div class="mode" aria-label="Achievement"><button id="fc" aria-pressed="true">Full Combo</button><button id="perfect" aria-pressed="false">Perfect</button></div><span></span><button id="replay">Replay</button><button id="pause">Play</button><select id="speed" aria-label="Playback speed"><option value="1">1× speed</option><option value="0.5">0.5× speed</option><option value="0.25">0.25× speed</option></select><button id="reduced" aria-pressed="false">Reduced motion: Off</button></div>
<div class="timeline"><input type="range" id="scrub" aria-label="Animation time" min="0" max="3.8" step="0.0166667" value="0.95"><output id="time"></output></div>
<p class="small">Prototype · reconstructed playfield · silent · final note lands at 0.65s. Replay to judge motion; use Perfect to compare the extra finishing motion.</p>
<script>${code}
let kind=0, perfect=true, reduced=matchMedia('(prefers-reduced-motion: reduce)').matches;
let seconds=0.95, playing=false, speed=1, previous=0;
const el=id=>document.getElementById(id);
function draw(){el('stage').innerHTML=sceneSvg(seconds,kind,perfect,reduced);el('scrub').value=seconds;el('time').value=seconds.toFixed(2)+' / 3.80s';el('description').textContent=directions[kind].description;el('pause').textContent=playing?'Pause':'Play';el('fc').setAttribute('aria-pressed',!perfect);el('perfect').setAttribute('aria-pressed',perfect);el('reduced').setAttribute('aria-pressed',reduced);el('reduced').textContent='Reduced motion: '+(reduced?'On':'Off');document.querySelectorAll('[data-kind]').forEach(b=>b.setAttribute('aria-pressed',Number(b.dataset.kind)===kind));}
function replay(){seconds=0;playing=true;previous=performance.now();draw();}
document.querySelectorAll('[data-kind]').forEach(b=>b.onclick=()=>{kind=Number(b.dataset.kind);replay();});
el('fc').onclick=()=>{perfect=false;replay();};el('perfect').onclick=()=>{perfect=true;replay();};el('replay').onclick=replay;
el('pause').onclick=()=>{if(seconds>=duration)seconds=0;playing=!playing;previous=performance.now();draw();};
el('speed').onchange=e=>speed=Number(e.target.value);el('reduced').onclick=()=>{reduced=!reduced;draw();};
el('scrub').oninput=e=>{seconds=Number(e.target.value);playing=false;draw();};
function tick(now){if(playing){seconds=Math.min(duration,seconds+(now-previous)/1000*speed);if(seconds>=duration)playing=false;draw();}previous=now;requestAnimationFrame(tick);}
document.addEventListener('visibilitychange',()=>{if(document.hidden){playing=false;draw();}previous=performance.now();});
draw();requestAnimationFrame(tick);
</script></body></html>`;
mkdirSync(resolve(root,'out'),{recursive:true});
writeFileSync(resolve(root,'out/review.html'),html);
console.log(resolve(root,'out/review.html'));
