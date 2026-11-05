import {AbsoluteFill, useCurrentFrame, useVideoConfig} from 'remotion';
import {sceneSvg} from './frame.mjs';
import {laneSceneSvg} from './lane-frame.mjs';
import './font.css';

type Props = {direction: number; perfect: boolean; reduced: boolean; laneSpace?: boolean};
export const CompletionStudy = ({direction, perfect, reduced, laneSpace = false}: Props) => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  return <AbsoluteFill dangerouslySetInnerHTML={{__html: (laneSpace ? laneSceneSvg : sceneSvg)(frame / fps, direction, perfect, reduced)}} />;
};
