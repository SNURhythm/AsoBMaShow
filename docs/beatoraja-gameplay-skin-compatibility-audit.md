# Beatoraja gameplay-skin compatibility audit

## Scope and source

- Upstream source: `/Users/xf/workspace/SNURhythm/beatoraja`
- Pinned revision: `c2ed5db1a46145ed10790c3872f717e95b59db9d`
- Scope: the gameplay Lua-table surface that Beatoraja loads through
  `JsonSkinLoader` and `JsonPlaySkinObjectLoader`.
- This is an evidence log. Every entry names the upstream class/method that
  establishes the behavior; it does not infer a rule from a skin sample.

## Source-level mismatches and missing implementations

0. **Gameplay skin time started from the chart clock, and iOS input began
   before authored geometry existed.**
   - Upstream: `TimerManager.setMainState` owns one MainState clock;
     `BMSPlayer` starts `TIMER_READY` on entering `STATE_READY`, and
     `Skin.drawAllObjects` evaluates ordinary destinations and timer-backed
     destinations against that same clock. `JsonPlaySkinObjectLoader`'s Note
     destination remains the skin's lane geometry.
   - Local (before this patch): `PlaySkinSession` fed destination animation
     from chart visual time, while live timers carried unshifted timestamps.
     iOS attempted to construct its raw touch router before the first skin
     submission had published Note/Slider geometry, then retained the legacy
     touch conversion.
   - Patch status: patched. The frame bridge derives one skin-state clock from
     `sceneStartMicros`; `TIMER_READY` (40), `TIMER_PLAY` (41), lane key/bomb,
     judge, custom-event, and destination clocks now share it. `GamePlayScene`
     chooses the first prep-metronome click as that origin when prep is active,
     preserving the earlier lane-indicator cue without consuming the skin's
     READY animation. On iOS, authority waits for the first successful skin
     frame, then builds its router from the published per-lane quadrilaterals;
     slider hit regions (including lane cover) continue through the same
     authored UI transform. `PlaySkinSession::touchLayout` now converts that
     transform to drawable-normalized coordinates exactly like the raw touch
     snapshot path.

1. **Post-decode dependency rejection has no upstream equivalent.**
   - Upstream: `JSONSkinLoader.loadJsonSkin` asks the object loader for each
     destination, skips a null object, and calls `Skin.add` otherwise.
     `Skin.prepare` subsequently removes only objects whose own `validate()`
     returns false. `SkinObject.validate()` checks only that a destination
     exists. `SkinGraph.validate()` and `SkinSlider.validate()` add only
     `SkinSource.validate()`.
   - Local (before this patch): `SkinModelValidator::validate` disabled
     optional objects or failed critical ones when its post-decode
     `validPayload` / `validDestination` checks found an empty sprite,
     unsupported numeric shape, missing resource, typed property, writer,
     timer, event, condition, nested Judge child, or Note geometry.
   - User-visible consequence: `graph_frame_customize` is rejected as
     `skin_lua_model_optional_object_disabled`, even though upstream would
     retain a `SkinGraph` with a null rate property and render it at zero.
   - Patch status: patched. The complete post-decode payload/destination
     admission pass has been removed. This specifically prevents
     `graph_frame_customize` from producing
     `skin_lua_model_optional_object_disabled`: an empty/missing Graph source
     stays cataloged, then Beatoraja-equivalent prepare/render handling omits
     only that draw. Resource/property/writer/timer/event/condition catalog
     lookups and Note/Judge cross-checks likewise cannot invalidate the skin.

2. **Local binding-catalog membership is treated as a validity condition.**
   - Upstream: `JsonSkinObjectLoader` passes `FloatPropertyFactory`,
     `IntegerPropertyFactory`, `StringPropertyFactory`, and event-factory
     results directly to object constructors. Those factories may return
     `null`; `SkinGraph.prepare`, `SkinSlider.prepare`, `SkinNumber.prepare`,
     and `SkinFloat.prepare` define their null behavior rather than rejecting
     the skin.
   - Local: `SkinModelValidator` filters binding IDs against the typed catalog
     and live callback generation, then uses the filtered sets to reject
     dependent objects and custom objects.
   - Patch status: patched. Every decoded binding ID remains in the model;
     runtime lookup keeps an unavailable source unavailable rather than
     making catalog admission fail.

3. **Text writer/editable fields are blocked at admission.**
   - Upstream: `JsonSkinObjectLoader.createText` always calls
     `SkinText.setWriter` and `SkinText.setEditable`; it does not reject a
     text object for either field.
   - Local: `SkinModelValidator` emits
     `skin_lua_model_text_interaction_unsupported` and disables that object.
   - Patch status: patched. Text editing remains a rendering/input TODO, but
     it no longer becomes a validation failure or suppresses unrelated skin
     output.

4. **Custom timer/event ranges and dependencies are locally fatal.**
   - Upstream: `JsonSkin.CustomTimer` and `JsonSkin.CustomEvent` carry `int`
     IDs; the JSON loader registers the authored objects and does not impose
     the local `10000..19999`, `1000..1999`, non-negative interval, or
     catalog-membership admission gates.
   - Local: `SkinModelValidator` treats those gates as a critical model error.
   - Patch status: patched. Decoded custom objects are retained; invocation
     continues to use the existing runtime bridge, which can report an
     unavailable callback at the point of use without invalidating the whole
     skin.

5. **Duplicate source handling diverges from `HashMap.put`.**
   - Upstream: `JSONSkinLoader.loadJsonSkin` inserts each `source` using
     `sourceMap.put`; the later declaration replaces the earlier declaration.
     `JsonSkinObjectLoader` separately resolves authored object categories by
     a forward scan, so repeated same-category object declarations are legal
     and the first matching declaration wins.
   - Local: `SkinModelValidator` has hard nonzero/unique resource, binding,
     and object identity checks. Some are internal IDs rather than authored
     IDs, but a duplicate that reaches this layer can turn the whole model
     invalid rather than applying the corresponding source rule.
   - Patch: decoder resolution must remain the authority for source/object
     naming. Validator identity checks will be limited to internal ownership
     invariants; no authored duplicate may be surfaced as a skin-validation
     failure.

6. **Destination field range checks are stricter than the loader.**
   - Upstream: `Skin.setDestination` and `SkinObject.setDestination` accept
     the authored integer blend/filter/center/angle/color fields. No loader
     validation rejects an otherwise loaded object for the local enum and byte
     checks.
   - Local: `LuaSkinTableDecoder::normalizeDestination` rejects unknown blend,
     filter, stretch, and color values before the object is admitted.
   - Patch status: queued. This requires an authored-value representation at
     the renderer boundary so unsupported raster operations can be omitted or
     mapped there without inventing a load-time rejection.

7. **Distribution Graph is loaded upstream but represented as blank locally.**
   - Upstream: `JsonSkinObjectLoader` creates `SkinDistributionGraph` for
     `graph.type < 0` (11-column form for `-1`, 28-column form otherwise).
   - Local: `normalizeSkinGraph` records
     `skin_lua_model_distribution_graph_unsupported` and uses
     `SkinBlankObject`.
   - Patch status: retained as a visible-model TODO. Its current blank
     placeholder must remain non-fatal; rendering needs a dedicated
     distribution-graph data source.

8. **GaugeGraph is absent from the gameplay decoder.**
   - Upstream: `JsonSkinObjectLoader` creates `SkinGaugeGraphObject` for a
     matching `gaugegraph` destination.
   - Local: `gaugegraph` is not decoded, so a matching destination is silently
     omitted.
   - Patch status: patched. Its identity is decoded and retained as an ordered
     `SkinBlankObject` with a warning until a graph renderer is implemented.

9. **JudgeGraph is accepted but not rendered.**
   - Upstream: `JsonSkinObjectLoader` creates `SkinNoteDistributionGraph`.
   - Local: it is decoded and retained as a blank placeholder with
     `skin_lua_model_judgegraph_unsupported`.
   - Patch status: no admission failure; dedicated renderer remains TODO.

10. **BPMGraph is accepted but not rendered.**
    - Upstream: `JsonSkinObjectLoader` creates `SkinBPMGraph`.
    - Local: it is decoded and retained as a blank placeholder with
      `skin_lua_model_bpmgraph_unsupported`.
    - Patch status: no admission failure; dedicated renderer remains TODO.

11. **HitErrorVisualizer and TimingVisualizer are accepted but not rendered.**
    - Upstream: `JsonSkinObjectLoader` creates `SkinHitErrorVisualizer` and
      `SkinTimingVisualizer`.
    - Local: both are decoded and retained as blank placeholders with warning
      diagnostics.
    - Patch status: no admission failure; dedicated renderer remains TODO.

12. **TimingDistributionGraph is absent from the gameplay decoder.**
    - Upstream: `JsonSkinObjectLoader` creates `SkinTimingDistributionGraph`
      for `timingdistributiongraph`.
    - Local: the array is not decoded, so its destination is silently omitted.
   - Patch status: patched. Its identity is decoded and retained as an ordered
     blank placeholder with a warning until a graph renderer is implemented.

13. **PMchara is accepted but not rendered.**
    - Upstream: `JsonPlaySkinObjectLoader` creates the matching PM-character
      object.
    - Local: `pmchara` is decoded and retained as a blank placeholder with
      `skin_lua_model_pmchara_unsupported`.
   - Patch status: no admission failure; dedicated animation support remains
     TODO.

14. **Integer `RateProperty` ranges were rejected after decode.**
    - Upstream: `SkinObject.RateProperty.get` evaluates `(max - min)` as a
      Java `int`, including zero and overflow results; it has no construction
      or validation rejection path. `SkinGraph` and `SkinSlider` keep the
      object whenever their `SkinSource` validates.
    - Local: `Skin2DRenderer::resolveRate` rejected zero and overflow spans
      with `skin.renderer.rate.range`, even after the model admission change.
   - Patch status: patched. The renderer now performs the same wrapping
     32-bit subtraction. If the resulting value cannot produce a bounded
     textured quad, only that draw is omitted; the skin/session remains
     active.

15. **A decoded object was revalidated as a whole, unlike Beatoraja's
    object-local lifecycle.**
    - Upstream: `Skin.prepare` removes each failed `SkinObject.validate()`
      object from the live list. For `SkinGraph`, `SkinSlider`, and
      `SkinImage`, source validity is determined by their own source class;
      `SkinGraph.prepare()` additionally sets only its own `draw` flag false
      when `source.getImage(...)` returns null. `SkinNumber`, `SkinFloat`,
      `SkinTextFont`, and play-field objects follow their own prepare/validate
      paths; there is no skin-wide critical/optional dependency classifier.
    - Local (before this patch): one generic validator imposed a synthetic
      “critical object” distinction over Graph, slider, number, float, note,
      gauge, cover, and Judge payload shapes. It could escalate a single
      unrenderable object into a fullscreen skin error.
    - Patch status: patched. The generic payload validator and its synthetic
      optional-object suppression list are gone. Decode-time Lua type safety
     and fixed resource budgets remain separate from model admission; each
     object now reaches its frame-local renderer/preparation path.

16. **Slider angle was narrowed to an unsigned byte and then treated as a
    submission error.**
    - Upstream: `JsonSkinObjectLoader` passes `JsonSkin.Slider.angle` directly
      to `SkinSlider`; `SkinSlider` stores it as an `int`. Its draw expression
      moves only for angles `0`, `1`, `2`, and `3`; every other integer draws
      at the authored destination. Its mouse switch likewise performs no
      write for non-cardinal angles.
    - Local (before this patch): `LuaSkinTableDecoder` rejected values outside
      `0..255`, narrowed accepted values to `uint8_t`, and the renderer made
      values above `3` fail a gameplay frame.
    - Patch status: patched. Slider angle remains a signed authored integer.
      Non-cardinal values submit a static knob and expose no slider input
      region, matching Beatoraja's draw and mouse-switch behavior.

17. **Judge wrappers without destination frames were discarded, and recent
    judgement duration used the wrong unit and polarity.**
    - Upstream: `JsonPlaySkinObjectLoader` constructs `SkinJudge` before the
      outer `JsonSkinObjectLoader.setDestination` call. `SkinJudge`'s
      constructor supplies its own destination, while `SkinJudge.prepare`
      prepares and draws its selected nested `SkinImage` / `SkinNumber`.
      Consequently an authored outer `{id = "judge"}` with no `dst` frames
      still renders the nested judge text and combo. `JudgeManager.updateMicro`
      stores `mfast / 1000` in `judgefast`; `IntegerPropertyFactory` exposes
      it through `judge_duration1` (525). Upstream `mfast > 0` is FAST.
    - Local (before this patch): the renderer dropped every non-Note empty
      wrapper destination, so common `SkinJudge` declarations rendered neither
      their judgement image nor combo count. It exposed Aso's `JudgeResult`
      timing in microseconds under property 525 and tested its inverse sign as
      FAST, producing oversized/wrong fast-slow values.
    - Patch status: patched. Empty Judge wrappers now retain only the
      constructor-backed nested-child path; wrappers with actual frames still
      suppress drawing when their conditions fail. Property 525 converts to
      signed milliseconds with Java-compatible truncation toward zero, and
      FAST/SLOW options 1242/1243 invert Aso's `JudgeResult::Diff` sign to
      match `JudgeManager`.

18. **Numeric destination `op` conditions bypassed the BooleanPropertyFactory
    split.**
    - Upstream: `SkinObject.setDrawCondition(int[])` passes every nonzero,
      unique numeric condition through `BooleanPropertyFactory` first. A
      recognized selector becomes a runtime draw condition; only a factory
      miss remains a static skin option. `JsonSkinObjectLoader` applies this
      behavior to ordinary destinations and child destinations of special
      gameplay objects.
    - Local (before this patch): `LuaSkinTableDecoder` preserved every numeric
      `op` entry as a static configuration condition, even when the pinned
      BooleanPropertyFactory catalog recognized it. Consequently recent-judge
      selectors such as `1242` (FAST) and `1243` (SLOW) were never evaluated
      against live gameplay state and their images were always suppressed.
    - Patch status: patched. Recognized numeric selectors now decode as typed
      Boolean properties before model normalization; unrecognized numeric
      selectors remain static skin options, preserving authored order.

## Patch order

1. Replace destination enum admission checks only after carrying the authored
   values safely through the renderer; do not guess a fallback blend/filter
   meaning at decode time.
2. Implement the rendering data/visual sources in findings 7 and 9–13 one at
   a time, each compared with its named upstream class.
