import {AbsoluteFill, useCurrentFrame, useVideoConfig} from 'remotion';
import {sceneSvg} from './frame.mjs';
import './font.css';

type Props = {direction: number; perfect: boolean; reduced: boolean};
export const CompletionStudy = ({direction, perfect, reduced}: Props) => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  return <AbsoluteFill dangerouslySetInnerHTML={{__html: sceneSvg(frame / fps, direction, perfect, reduced)}} />;
};
