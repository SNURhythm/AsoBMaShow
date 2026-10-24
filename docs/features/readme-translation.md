# Readme copy and translation

The chart readme reader can copy the full displayed text, including lines outside
the viewport. Translate defaults to the app language; choose another target with
**Translate to**. Original and Translated select which text is displayed and copied.
Changing file, encoding, source or target clears the translation. Nothing is written back
to the chart folder or archive.

Android translation is powered by [Google Translate](https://cloud.google.com/translate)
through ML Kit's on-device models. A prompt precedes missing-model downloads,
which can use mobile data. Once downloaded, models work offline; readme text is
not uploaded for translation. Android provides one Chinese target, as supported
by ML Kit. **Source** offers Auto detect, Japanese, English and Korean. Auto detect
calls the platform detector once with the whole README; manual selection skips
language detection. The whole text is translated in one call using one source/target
model pair. No per-line guessing, sampling or custom chunking is performed. The
existing reader input and translated-output byte limits remain in effect.

ML Kit internally uses only the first 200 characters for detection ([reference](https://developers.google.com/android/reference/com/google/mlkit/nl/languageid/LanguageIdentifier)).
Choose a source manually if headings or mixed-language text give a wrong result.
The translation engine controls the translated text's line breaks; the original
text is always retained. Changing source cancels any pending translation and clears
its result, just like changing the target.

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

HTTP/HTTPS URLs and bare `www.` addresses are underlined and open in the system
browser on a completed click or tap. Wrapped fragments open the full URL. Movement,
wheel scrolling, cancelled input and a tap that stops scroll momentum do not open
a link. This same reader is used for Legal notices.
