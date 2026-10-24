# Whole-document translation and reader links

The user wants simpler/faster translation with Source choices Auto detect/Japanese/English/Korean, and clickable URLs in Readme and Legal. They explicitly require scrolling never to activate URLs and requested whole-text detection/translation.

Pass the complete text to the selected platform detector once in Auto mode, bypass detection for manual selection, then translate the complete text once with a single model pair. Remove the per-segment detection, paragraph sampling and custom chunking. Keep cancellation/request identity, model-download consent, output size bounds and Apple high-fidelity preference. Source and target dropdowns share a row; file and encoding share another.

Reuse one virtualized document reader for both surfaces. Parse HTTP(S)/www URLs once per text change, map full URL ranges across wrapped lines, underline hit regions, and open only on a matched press/release with no intervening drag, scrolling or cancellation. Link rows remain passive so the recycler owns scrolling; taps stopping momentum do not activate links. Use the platform URL opener.

Validate native gesture tests, source UI/cancellation integration, platform builds and real-device whole-text translation and URL scrolling.
