# Readme copy and translation

The chart readme reader can copy the full displayed text, including lines outside
the viewport. Translate defaults to the app language; choose another target with
**Translate to**. Original and Translated select which text is displayed and copied.
Changing file, encoding or target clears the translation. Nothing is written back
to the chart folder or archive.

Android translation is powered by [Google Translate](https://cloud.google.com/translate)
through ML Kit's on-device models. A prompt precedes missing-model downloads,
which can use mobile data. Once downloaded, models work offline; readme text is
not uploaded for translation. Android provides one Chinese target, as supported
by ML Kit. Source language is detected per text segment so English headings do not prevent
Japanese or Korean notes from being translated. Short names or ambiguous text may
remain unchanged or translate less accurately.

Apple uses Translation on iOS 18+ and macOS 15+. On iOS/macOS 26.4+, the app requests
high-fidelity translation; Apple's framework falls back to its standard on-device
model when high fidelity is unavailable. The system handles model download consent.
Copy remains available on older Apple systems, Windows and Linux.

Translations are automatic and may be inaccurate. Keep the original for author
instructions, credits and usage terms. Google provides the following disclaimer:

> THIS SERVICE MAY CONTAIN TRANSLATIONS POWERED BY GOOGLE. GOOGLE DISCLAIMS ALL
> WARRANTIES RELATED TO THE TRANSLATIONS, EXPRESS OR IMPLIED, INCLUDING ANY
> WARRANTIES OF ACCURACY, RELIABILITY, AND ANY IMPLIED WARRANTIES OF
> MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.

See [Google's attribution requirements](https://docs.cloud.google.com/translate/attribution).
