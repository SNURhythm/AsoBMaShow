# SDL3 migration design

Migrate the existing application to the checked-in SDL3 and SDL3_ttf fork release
branches. Preserve gameplay scoring/timing, saved input bindings and display
settings, text composition, mobile file import, native window rendering and
application lifecycle. Keep bgfx, the audio backends, parser, replay formats and
existing main-loop architecture. This implements the migration discussed and
authorized in the session; it is not a new renderer or audio project.

Build both SDL components from the pinned fork submodules on desktop and Android;
update the existing Xcode framework integration on iOS. Keep clean release
branches upstream-only and commit necessary patches to the asobmashow branches.
Do not introduce SDL2 compatibility aliases or keep a second SDL runtime linked.

Port API names and signatures directly. Audit boolean return values and borrowed
event text, 64-bit nanosecond timestamps, stable device/display IDs, float pointer
coordinates, surface pixel-format access, and window-scoped text input. Convert
SDL timestamps into the existing steady-clock microsecond gameplay domain
without losing submillisecond precision or truncating at 32 bits. Preserve saved
binding identity and application display-index settings through explicit mapping.

Keep the native UIKit touch sink and asynchronous controller contract until an
upstream replacement is proven equivalent. Port composition behavior and the
scene external-display regression where upstream does not provide equivalence.
Remove unused historical fork changes instead of porting them speculatively.
Font creation/use/destruction must obey SDL3_ttf's thread-affinity contract.

Validation: desktop build and full CTest; focused input timestamp, device registry,
touch cancellation, text/IME/font lifetime and display-setting tests; unsigned iOS
release verification and scene simulator smoke; Android signed build-only and
emulator lifecycle/import smoke. No distribution uploads. Windows/Linux and
physical-device timing validation are reported explicitly if unavailable.
