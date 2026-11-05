import {Composition} from 'remotion';
import {CompletionStudy} from './Composition';

export const RemotionRoot = () => <>
  <Composition id="Breakthrough-FC" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 0, perfect: false, reduced: false}} />
  <Composition id="Breakthrough-Perfect" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 0, perfect: true, reduced: false}} />
  <Composition id="Supernova-FC" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 1, perfect: false, reduced: false}} />
  <Composition id="Supernova-Perfect" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 1, perfect: true, reduced: false}} />
  <Composition id="Ascension-FC" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 2, perfect: false, reduced: false}} />
  <Composition id="Ascension-Perfect" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 2, perfect: true, reduced: false}} />
</>;
