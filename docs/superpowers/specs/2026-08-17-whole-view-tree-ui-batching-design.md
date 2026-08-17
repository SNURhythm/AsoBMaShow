# Whole-View-Tree UI Batching Design

## Goal

Render the ordinary `View` UI tree through ordered persistent GPU batches
instead of allocating a bgfx transient vertex and index buffer for every
widget primitive. This removes standard UI rendering from the shared
transient-buffer budget, reduces draw calls, and preserves the current visual
stacking order exactly.

Gameplay rendering, video/BGA compositing, and the existing skin quad batcher
are out of scope. The latter already has its own batching and preflight path.

## Evidence

The general UI traversal in `View::render` currently submits independently
allocated transient geometry for view decorations. Its descendants do the
same for button chrome, images, text, text-input selection and caret geometry,
and scrollbars. A large scene therefore consumes the bgfx transient pools with
many small, aligned allocations and separate submits. The existing
`SimpleBatchRenderer` and `TexBatchRenderer` only batch inside the local
object that created them; neither can batch across the view tree.

The configured bgfx transient budgets are 16 MiB for vertices and 4 MiB for
indices. Enlarging those budgets would only defer exhaustion and would not
remove the allocation and submission overhead.

## Design

### Whole-tree scope and ordering

Each scene creates one UI batching scope around its ordinary view traversal.
Every supported view draw appends to that scope in the same depth-first order
in which it is currently submitted. The batcher never sorts or otherwise
reorders commands.

The current batch is emitted before any property that changes rendering
semantics: vertex format, shader program, texture/sampler, blend state,
scissor rectangle, or transform. It is also emitted before and after existing
specialized/direct bgfx UI rendering. Consequently, nested backgrounds,
children, overlays, and later siblings retain their current painter order.

Scene custom rendering is an explicit boundary: the scene flushes pending UI
work before `renderScene()` and begins a fresh batch for any views rendered
after it.

### Persistent UI buffers

`UiBatchRenderer` owns persistent bgfx dynamic vertex/index buffers for the
two UI vertex formats already in use:

- colored geometry (`PosColorVertex`) for fills, borders, selections, carets,
  and non-textured rounded shapes;
- textured geometry (`PosTexCoord0Vertex`/`PosTexVertex`, unified where their
  layouts are compatible) for image and text quads.

It accumulates CPU-side geometry until a state boundary or safe batch-size
limit. It uploads the completed batch to the matching dynamic buffers and
submits it. A batch is capped below the 16-bit indexable vertex limit, so a
large same-state run is split into independently valid chunks. Dynamic-buffer
allocation or update failure skips only that batch, logs a rate-limited
diagnostic, and leaves the renderer usable for later frames; it must not fall
back to the transient API.

The batcher has explicit `begin`, `flush`, and `end` operations. `RenderContext`
exposes only queueing and flush-boundary helpers to view implementations, so
ownership remains at the render-pass level rather than spreading across
widgets.

### View integrations

The implementation routes the common view-tree primitives through the batcher:

- `View` fill, gradient, border, rounded-rect, and shadow submissions;
- `Button` background and border geometry;
- `ImageView` image and rounded-image quads;
- `TextView` text quads;
- `TextInputBox` selection, caret, and composition underline geometry;
- `ScrollView` and `RecyclerView` scrollbars.

Rounded shapes retain their existing triangle-fan tessellation. They use the
colored or textured batch appropriate to their current shader, and preserve
their existing antialias/MSAA and uniform behavior. Unsupported custom paths
remain direct submissions behind an explicit flush boundary and can be
migrated independently later.

## Error Handling and Lifecycle

- The batcher is lazy-initialized after bgfx is available and destroys its
  dynamic buffers during renderer teardown.
- Invalid textures or zero-area geometry are ignored as today.
- Every state change flushes before modifying uniforms or bindings so no
  queued draw observes another draw's state.
- Upload capacity is bounded and chunks are reset after each submit; no
  unbounded per-frame CPU accumulation is allowed.
- The scene scope always flushes at exit, including when a view subtree is
  empty. A teardown path discards pending CPU geometry without issuing bgfx
  calls.

## Testing and Verification

Introduce a focused renderer test with a recording backend seam. It verifies:

1. adjacent compatible commands from different nested views coalesce;
2. texture, shader, scissor, transform, and custom-render boundaries split
   batches without changing submission order;
3. a run exceeding the 16-bit vertex limit splits into valid chunks;
4. uploads use the dynamic-buffer backend and never invoke transient-buffer
   allocation;
5. invalid resources and upload failures skip the affected batch without
   corrupting later submissions.

Update existing view-layout or renderer tests where needed to exercise the
scene-wide scope. Build the focused target, then run the relevant CTest tests
and a full parallel CTest pass. A desktop build of `main` confirms that the
production bgfx path compiles with the platform renderer.

## Alternatives Rejected

### Per-widget transient batching

Local `SimpleBatchRenderer`/`TexBatchRenderer` instances reduce only the work
inside one widget. They cannot coalesce parent, child, or sibling work and
still consume the shared transient pools, so they do not address the root
failure mode.

### A scene-wide transient batch

An ordered transient command queue would reduce calls but would still allocate
the same geometry from bgfx's shared per-frame transient pool. It improves
fragmentation and alignment overhead but cannot guarantee that a complex UI
does not hit the transient limit.

### Larger transient pools

Increasing bgfx limits increases memory usage and postpones failure without
reducing UI draw-call or allocation overhead.
