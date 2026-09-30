# Application localization

The built-in interface supports English, Korean, and Japanese. On a new
installation the app uses the first supported language in SDL's device language
preferences, falling back to English. Settings → Misc → Language provides a
dropdown with System language, English, 한국어, and 日本語. Changes apply immediately, including when returning to an already open screen.

Language is saved in `application-ui-state.json`, independently of player
profiles. Existing files without a language preference keep the System default.
Unknown language values fall back to System without discarding toolbar state.

Use stable semantic IDs for application-owned labels and messages, for example
`i18n::tr("settings.audio.test_sound.label")`. `src/i18n/Messages.inc` contains
explicit `{ID, English, Korean, Japanese}` entries sorted by ID. Names follow
the owning screen or component, control or state, and purpose. For example,
`settings.language.change_notice` is separate from the language control label.
Do not use sentence text, hashes, random values, or numbered placeholders as IDs.
Keep IDs unchanged when editing English copy or moving implementation code.

Give independent controls separate contextual IDs even when their English text
matches: `settings.audio.apply.label` and `settings.display.apply.label` can be
translated independently. A control may reuse its ID when refreshing its text.
English, Korean, and Japanese are sibling catalog values; English text is
never used for lookup. Empty translation values fall back to English. Unknown
IDs remain visibly unchanged so mistakes can be diagnosed, and catalog tests
reject missing IDs.
Do not pass chart metadata, file paths, profile/playlist names, typed text,
serialized values, or third-party skin text through translation lookup.

Retained UI labels use `TextView::setLocalizedText(i18n::message("settings.audio.test_sound.label"))`.
The descriptor owns its ID and named arguments, so language changes can refresh
text in place without replacing views. Use nested `i18n::message` arguments for
translated values and raw strings for user content. Ordinary `setText` clears
any existing binding. Dropdown and context-menu labels also accept descriptors.
Scene language hooks refresh dynamic presentation while preserving input drafts,
selection, scroll, playback, and ongoing work.

For one-time sentences containing values, use `i18n::format` with an ID and named values:
`i18n::format("settings.display.preview.countdown.other", {{"seconds", "3"}})`.
The catalog value is `Reverting in {seconds} seconds`. Translate the entire sentence so that
word order can change. Replacement values are inserted verbatim, even when they
contain braces. Keep BMS standard grade, gauge, and judgement abbreviations when
players use them as technical terms.

The Korean font is Noto Sans KR Regular, unmodified, from Noto CJK Sans 2.004:
https://github.com/notofonts/noto-cjk/blob/Sans2.004/Sans/SubsetOTF/KR/NotoSansKR-Regular.otf
It is bundled as `assets/fonts/notosanskr.otf`; the upstream SIL Open Font License
is in `assets/legal/noto-sans-kr.txt`. TextView's fallback chain uses this font
when the primary font lacks Korean glyphs. All asset packaging uses the existing
assets directory, including Android and iOS.

Japanese uses the existing bundled Noto Sans JP fonts through TextView's primary
font and fallback chain. No additional font asset is needed.

Validation includes locale negotiation, preference persistence and migration,
English fallback, Korean and Japanese lookup, named substitution, catalog
consistency, and font coverage. Device-native system UI follows the operating
system language; third-party skins retain their own authored text.
