# Whole-document translation and reader links

The user wants simpler/faster translation with Source choices Auto detect/Japanese/English/Korean, and clickable URLs in Readme and Legal. They explicitly require scrolling never to activate URLs and requested whole-text detection/translation.

Pass the complete text to the selected platform detector once in Auto mode, bypass detection for manual selection, then translate the complete text once with a single model pair. Remove the per-segment detection, paragraph sampling and custom chunking. Keep cancellation/request identity, model-download consent, output size bounds and Apple high-fidelity preference. Source and target dropdowns share a row; file and encoding share another.

Reuse one virtualized document reader for both surfaces. Parse HTTP(S)/www URLs once per text change, map full URL ranges across wrapped lines, underline hit regions, and open only on a matched press/release with no intervening drag, scrolling or cancellation. Link rows remain passive so the recycler owns scrolling; taps stopping momentum do not activate links. Use the platform URL opener.

Validate native gesture tests, source UI/cancellation integration, platform builds and real-device whole-text translation and URL scrolling.

## Translation modal and reading positions

The user requested moving source, target and quality controls into a dedicated
translation modal so their purpose is unambiguous, leaving the reader's Translate
action. Use an in-app overlay with a title, vertically stacked language selectors,
a labeled quality section when supported, and Cancel/Translate actions. Keep
controls scrollable on small screens and actions visible. Editing options alone
must preserve the displayed result; confirmation starts translation. Back/Escape
closes a dropdown first, then the translation modal, then the reader.

Switching Original/Translated shares one proportional reading position, as clarified
by the user: divide the current offset by the current scrollable range and apply
that fraction to the other version's range after reflow. Top/bottom remain aligned;
a non-scrollable version retains the shared fraction for switching back. New
files/encodings reset the position. The existing native Apple translation host is unchanged. Whole-text
platform calls expose no intermediate percentage, so use stage indicators.

## Standard-mode URL protection

The user subsequently requested URL tokenization only for Standard translation,
because it corrupts links while High fidelity benefits from complete context.
Detect language from the whole input as before. In Standard mode (also Android),
split only around recognized HTTP(S)/www URLs, keep URLs and boundary whitespace
literal, and translate the surrounding prose using the one prepared model pair.
Do not tokenize High fidelity input. Documents without URLs stay one request.
Apply the existing output budget to every appended piece and ignore cancelled
requests between asynchronous translations.
