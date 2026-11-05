import {Composition} from 'remotion';
import {CompletionStudy} from './Composition';

export const RemotionRoot = () => <>
  <Composition id="Impact-FC" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 0, perfect: false, reduced: false}} />
  <Composition id="Impact-Perfect" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 0, perfect: true, reduced: false}} />
  <Composition id="Sweep-FC" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 1, perfect: false, reduced: false}} />
  <Composition id="Sweep-Perfect" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 1, perfect: true, reduced: false}} />
  <Composition id="Rise-FC" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 2, perfect: false, reduced: false}} />
  <Composition id="Rise-Perfect" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 2, perfect: true, reduced: false}} />
  <Composition id="Lane-Wave-FC" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 0, perfect: false, reduced: false, laneSpace: true}} />
  <Composition id="Lane-Wave-Perfect" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 0, perfect: true, reduced: false, laneSpace: true}} />
  <Composition id="Lane-Rails-FC" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 1, perfect: false, reduced: false, laneSpace: true}} />
  <Composition id="Lane-Rails-Perfect" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 1, perfect: true, reduced: false, laneSpace: true}} />
  <Composition id="Lane-Fan-FC" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 2, perfect: false, reduced: false, laneSpace: true}} />
  <Composition id="Lane-Fan-Perfect" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 2, perfect: true, reduced: false, laneSpace: true}} />
  <Composition id="Calm-Finish" component={CompletionStudy} durationInFrames={228} fps={60} width={1280} height={720} defaultProps={{direction: 0, perfect: false, reduced: false, finishStudy: true}} />
</>;
