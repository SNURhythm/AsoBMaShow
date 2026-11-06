// Calm completion study only. FINISH acknowledges reaching the end, not a lamp.
import {stage, impactTime} from './frame.mjs';
import {laneSceneSvg} from './lane-frame.mjs';
export const finishDirections = [
  {name:'01 / Finish', description:'A quiet acknowledgement: smaller lettering lifts gently into place, holds, then fades. No burst, mark, or travelling wave.'},
  {name:'02 / Full Combo', description:'Compare the neutral finish with the stronger Rise lettering and soft lime Wave.'},
  {name:'03 / Perfect', description:'Compare with Perfect’s gold Wave, signature mark, and single celebratory pulse.'},
];
export function finishSceneSvg(seconds,kind=0,_perfect=false,reduced=false) {
  void _perfect; // Shared reviewer signature; the direction selects the outcome.
  if(kind>0)return laneSceneSvg(seconds,0,kind===2,reduced);
  const age=seconds-impactTime;
  const clampFinish=v=>Math.max(0,Math.min(1,v));
  const smoothFinish=v=>{const q=clampFinish(v);return q*q*(3-2*q);};
  const opacity=smoothFinish(age/.32)*(1-smoothFinish((age-1.45)/.45));
  const lift=reduced?0:6*(1-smoothFinish(age/.32));
  const laneOpacity=reduced?0:smoothFinish(age/.18)*(1-smoothFinish((age-.25)/.55))*.16;
  const defs=`<defs>
    <radialGradient id="finishDim"><stop stop-color="#000" stop-opacity=".55"/><stop offset=".55" stop-color="#000" stop-opacity=".55"/><stop offset="1" stop-color="#000" stop-opacity="0"/></radialGradient>
    <radialGradient id="finishGlow"><stop stop-color="#cdd9e1" stop-opacity=".07"/><stop offset="1" stop-color="#cdd9e1" stop-opacity="0"/></radialGradient>
    <linearGradient id="finishLane" x1="0" y1="1" x2="0" y2="0"><stop stop-color="#cdd9e1"/><stop offset="1" stop-color="#cdd9e1" stop-opacity="0"/></linearGradient>
    <radialGradient id="hitGlow"><stop stop-color="#f8fdff" stop-opacity=".85"/><stop offset=".22" stop-color="#56d2ca" stop-opacity=".7"/><stop offset="1" stop-color="#56d2ca" stop-opacity="0"/></radialGradient>
  </defs>`;
  const overlay=age<0?'':`<path d="M204 475H1076L1120 575H160Z" fill="url(#finishLane)" opacity="${laneOpacity}"/>
    <g opacity="${opacity}"><ellipse cx="640" cy="285" rx="380" ry="120" fill="url(#finishDim)"/>
    <ellipse cx="640" cy="285" rx="260" ry="65" fill="url(#finishGlow)"/>
    <text x="640" y="${285+lift}" text-anchor="middle" dominant-baseline="central" font-family="Achievement,sans-serif" font-size="62" font-weight="700" letter-spacing="8" fill="#dce3e8">FINISH</text></g>`;
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1280 720" width="100%" height="100%" role="img" aria-label="Calm Finish prototype">${defs}${stage(seconds,false,reduced,true)}${overlay}</svg>`;
}
