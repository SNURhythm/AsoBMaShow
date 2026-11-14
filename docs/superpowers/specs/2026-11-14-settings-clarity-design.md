# Settings clarity

The user approved all six improvements from the settings-clarity proposal: visible scope, one editor per setting, timing dependency explanations, descriptive navigation names, editing before maintenance, and section reset/copy actions plus search.

Preserve stored meaning and the native C++ view system. Show the active profile and label each group according to its actual ownership (profile-wide, orientation-wide/shared across skins, built-in appearance, or custom skin/mode/orientation). Built-in settings are not uniformly per mode: geometry and effects helpers mix shared and mode-specific values, so label their groups accurately.

Rename Timing to Calibration, Lane to Scroll & visibility, and Skins to Appearance & skins. Appearance offers skin editing, shared overlays, and package management. Built-in HUD retains built-in text/timing/gauge appearance; shared indicator/counter settings have one reusable editor, linked from skin editors and available in preview. Preview uses matching names and builders.

Keep skin selection and controls first. Move hashes, paths, and validation detail into details/troubleshooting and package maintenance into management. Actionable errors and in-progress import/replacement controls must remain reachable.

Timing settings explain the difference between enabling a skin's display and selecting which judgements show it. Only claim an inactive condition when metadata establishes it; an LR2 Judge Detail option does not establish whether the skin has its own authored timing display.

Reset and orientation-copy actions operate on named appearance sections, including built-in appearance, shared overlays, scroll/visibility and custom-skin timing/options/framing. Use orientation-specific defaults and preserve unrelated fields, other modes, skins, profile settings and the source orientation. Copy only the same custom skin and mode, using the skin controller/activation pipeline for catalog changes. Actions must clearly name their destination and require an in-app confirmation before replacing a section. Search provides localized setting/section names and useful aliases, routes to the relevant editor without changing saved skin selection, and keeps query editing stable.

No new worktree, deployment, unrelated Xcode edits, whole-file formatting, or external assets. Commit and push verified changes to the current branch and update PR #188.
