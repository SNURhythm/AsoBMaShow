import {readFileSync, writeFileSync, mkdirSync} from 'node:fs';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {execFileSync} from 'node:child_process';
const root = dirname(fileURLToPath(import.meta.url));
const motion = resolve(root, '../completion-motion');
const out = resolve(root, 'out');
mkdirSync(out, {recursive:true});
const fontDefs = [
  ['MotionCondensed', resolve(root, '../../assets/fonts/roboto-condensed-bold.ttf'), 900],
  ['Judgement', resolve(motion, 'src/fonts/judgement-ascii.ttf'), 400],
  ['Achievement', resolve(motion, 'src/fonts/judgement-bold-ascii.otf'), 700],
].map(([name, source, weight]) => {
  const target = resolve(out, `${name}.woff2`);
  execFileSync(process.env.PYFTSUBSET || 'pyftsubset', [source, '--unicodes=U+0020-007E',
    '--flavor=woff2', `--output-file=${target}`, '--layout-features=*']);
  return `@font-face{font-family:${name};src:url(data:font/woff2;base64,${readFileSync(target).toString('base64')}) format('woff2');font-weight:${weight};font-display:block}`;
}).join('\n');
const code = ['frame.mjs', 'lane-frame.mjs', 'max-frame.mjs'].map(file =>
  readFileSync(resolve(motion, 'src', file), 'utf8').replace(/^import .*;$/gm, '').replaceAll('export ', '')).join('\n');
const names = ['victory-previous', 'victory-crowd'];
const tracks = Object.fromEntries(names.map(name => [name,
  `data:audio/mp4;base64,${readFileSync(resolve(out, `${name}-preview.m4a`)).toString('base64')}`]));
const html = `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Completion sound auditions</title><style>
:root{color-scheme:dark;--foreground:#e9edf3;--muted-foreground:#a5afbc;--card:#151c26;--border:#35404d;--primary:#b5ebf1;--primary-foreground:#0b151a;--radius:12px;--font-sans:system-ui,sans-serif}
${fontDefs}
*{box-sizing:border-box}body{margin:0;font-family:var(--font-sans);color:var(--foreground)}
.versions{display:flex;gap:8px;margin-bottom:12px}.versions button{flex:1}.choices{display:flex;gap:8px;margin-bottom:12px}button{font:inherit;font-size:13px;border:1px solid var(--border);border-radius:8px;background:var(--card);color:var(--foreground);padding:12px;cursor:pointer}.choices button{flex:1}
button[aria-pressed=true]{background:var(--primary);color:var(--primary-foreground);border-color:var(--primary)}button:focus-visible{outline:2px solid var(--primary);outline-offset:3px}
.stage{width:100%;aspect-ratio:16/9;border-radius:var(--radius);overflow:hidden;background:var(--card)}.stage svg{display:block;width:100%;height:100%}
.note{font-size:14px;line-height:1.5;margin:12px 0;color:var(--foreground);min-height:42px}.meta{font-size:12px;line-height:1.5;color:var(--muted-foreground);margin:10px 0 0}
audio{width:100%;margin:0;display:block}.controls{display:flex;gap:8px;margin-top:10px}.controls button{flex:1}.status{font-size:12px;color:var(--muted-foreground);min-height:18px;margin:8px 0}
@media(max-width:450px){button{font-size:12px;padding:10px 7px}.note{font-size:13px}}
</style></head><body>
<div class="choices" aria-label="Choose a sound palette">
<button data-choice="victory-previous" aria-pressed="false">Previous</button>
<button data-choice="victory-crowd" aria-pressed="true">Revised · roar + claps</button>
</div>
<div id="stage" class="stage" role="img" aria-label="MAX celebration animation"></div>
<p id="note" class="note"></p>
<audio id="audio" controls preload="metadata" aria-label="Selected sound with animation preroll"></audio>
<div class="controls"><button id="replay">Play celebration</button><button id="all">Play both</button></div>
<p id="status" class="status" aria-live="polite">Ready — press Play to hear.</p>
<p class="meta">Revised: 2.90 s, including the applause tail · 0.65 s lead-in<br>Previous and revised matched in listening level · local audition.<br>CC0 sources: <a href="https://opengameart.org/content/hyper-ultra-fanfare" target="_blank" rel="noopener">Zane Little Music</a> and <a href="https://freesound.org/people/qubodup/sounds/182572/" target="_blank" rel="noopener">qubodup</a>.</p>
<script>
${code}
const tracks = ${JSON.stringify(tracks)};
const descriptions = {
 'victory-previous':['previous version','The accepted 1.30-second direction, retained at a matched listening level.'],
 'victory-crowd':['revised celebration','A short melodic pickup leads into the same victory hit. A stronger crowd roar opens behind it, and applause continues after the music.']
};
let selected='victory-crowd', frameRequest=0, sequence=false;
const player=document.getElementById('audio'), viewport=document.getElementById('stage');
const status=document.getElementById('status');
function draw(t){viewport.innerHTML=maxSceneSvg(t,0);}
function tick(){draw(player.currentTime);if(!player.paused&&!player.ended)frameRequest=requestAnimationFrame(tick);}
function select(name){
 cancelAnimationFrame(frameRequest);player.pause();selected=name;player.src=tracks[name];
 document.querySelectorAll('[data-choice]').forEach(b=>b.setAttribute('aria-pressed',String(b.dataset.choice===name)));
 document.getElementById('note').textContent=descriptions[name][1];
 document.getElementById('replay').textContent='Play '+descriptions[name][0];
 viewport.setAttribute('aria-label','MAX animation with '+descriptions[name][0]);
 draw(1.05);status.textContent='Ready — press Play to hear.';
}
async function play(){player.currentTime=0;try{await player.play();}catch{status.textContent='Press the audio player’s Play button to enable sound.';}}
document.querySelectorAll('[data-choice]').forEach(b=>b.addEventListener('click',()=>{sequence=false;select(b.dataset.choice);play();}));

document.getElementById('replay').onclick=()=>{sequence=false;play();};
document.getElementById('all').onclick=()=>{sequence=true;select('victory-previous');play();};
player.addEventListener('play',()=>{status.textContent='Playing '+descriptions[selected][0];cancelAnimationFrame(frameRequest);tick();});
player.addEventListener('pause',()=>{cancelAnimationFrame(frameRequest);status.textContent='Paused';});
player.addEventListener('seeked',()=>draw(player.currentTime));
player.addEventListener('ended',()=>{
 cancelAnimationFrame(frameRequest);
 if(sequence&&selected==='victory-previous'){select('victory-crowd');play();}
 else {sequence=false;draw(1.05);status.textContent='Finished — replay or compare another sound.';}
});
select(selected);
</script></body></html>`;
writeFileSync(resolve(out, 'celebration-review.html'), html);
console.log(resolve(out, 'celebration-review.html'));
