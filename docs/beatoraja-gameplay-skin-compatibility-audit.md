# Beatoraja gameplay-skin compatibility audit

## Scope and source

- Upstream source: `/Users/xf/workspace/SNURhythm/beatoraja`
- Pinned revision: `c2ed5db1a46145ed10790c3872f717e95b59db9d`
- Scope: the gameplay Lua-table surface that Beatoraja loads through
  `JsonSkinLoader` and `JsonPlaySkinObjectLoader`.
- This is an evidence log. Every entry names the upstream class/method that
  establishes the behavior; it does not infer a rule from a skin sample.

## Source-level mismatches and missing implementations

1. **Post-decode dependency rejection has no upstream equivalent.**
   - Upstream: `JSONSkinLoader.loadJsonSkin` asks the object loader for each
     destination, skips a null object, and calls `Skin.add` otherwise.
     `Skin.prepare` subsequently removes only objects whose own `validate()`
     returns false. `SkinObject.validate()` checks only that a destination
     exists. `SkinGraph.validate()` and `SkinSlider.validate()` add only
     `SkinSource.validate()`.
   - Local: `SkinModelValidator::validate` disables optional objects or fails
     critical ones when `validPayload` or `validDestination` cannot resolve a
     resource, typed property, writer, timer, event, or condition.
   - User-visible consequence: `graph_frame_customize` is rejected as
     `skin_lua_model_optional_object_disabled`, even though upstream would
     retain a `SkinGraph` with a null rate property and render it at zero.
   - Patch status: patched. Resource/property/writer/timer/event/condition
     catalog lookups no longer disable an otherwise decoded object. The
     renderer/resource layer remains responsible for omitting a
     non-renderable draw at the frame where it cannot be drawn.

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

## Patch order

1. Replace destination enum admission checks only after carrying the authored
   values safely through the renderer; do not guess a fallback blend/filter
   meaning at decode time.
2. Implement the rendering data/visual sources in findings 7 and 9–13 one at
   a time, each compared with its named upstream class.
