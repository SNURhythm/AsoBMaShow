import {AbsoluteFill, useCurrentFrame, useVideoConfig} from 'remotion';
import {sceneSvg} from './frame.mjs';
import {laneSceneSvg} from './lane-frame.mjs';
import {finishSceneSvg} from './finish-frame.mjs';
import {maxSceneSvg} from './max-frame.mjs';
import './font.css';

type Props = {direction: number; perfect: boolean; reduced: boolean; laneSpace?: boolean; finishStudy?: boolean; maxStudy?: boolean};
export const CompletionStudy = ({direction, perfect, reduced, laneSpace = false, finishStudy = false, maxStudy = false}: Props) => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  return <AbsoluteFill dangerouslySetInnerHTML={{__html: (maxStudy ? maxSceneSvg : finishStudy ? finishSceneSvg : laneSpace ? laneSceneSvg : sceneSvg)(frame / fps, direction, perfect, reduced)}} />;
};
