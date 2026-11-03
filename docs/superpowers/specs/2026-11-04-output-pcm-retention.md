# Retain output PCM only

The user chose deleting native PCM after resampling. Keep ready output samples
and use those as the input to rare subsequent device-rate changes. No content
deduplication, compression, streaming, or callback resampling. Repeated device
rate changes can accumulate interpolation loss; this is the chosen tradeoff.

Release each native buffer immediately after that source and its BMSON slices
are prepared; never wait for the whole chart. Keep source/output handles aliased to one immutable output allocation;
update source rate/frame metadata together. Commit rate transitions atomically
as before, releasing the preceding allocation only after successful preparation.
Skin sounds adopt the same single-buffer policy at publication.

Preparation caches should replace native PCM with available prepared output,
respecting retention limits. Mark converted snapshots so BMSON can request original
samples when preparing new slices; slicing a resampled full track would change
initial slice boundaries and interpolation. Loading originals for a new slicing
operation is allowed; active sounds never need files or decompression to play.

Measure buffer lifetime with weak references and test initial output equivalence,
rate-transition failure, repeated changes, preview handoff and BMSON behavior.

BMSON prepares slices in the per-source loader callback before releasing its
original. New BMSON preparation can decode originals again using the existing
archive batch path. Unconverted hidden/expired sounds keep their sole PCM copy.
Preview deferred workers drop source references after each item. Retain original
duration as integer metadata to prevent frame-count growth on repeated changes.
