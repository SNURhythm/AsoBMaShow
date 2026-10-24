# Readme copy and translation

The chart readme reader can copy the full displayed text, including lines outside
the viewport. **Translate** opens a dedicated **Translate readme** modal with
Source and Translate to selectors, plus Translation quality on supported Apple
systems. The target defaults to the app language. Cancel/Back closes this modal
without discarding the displayed translation; Translate confirms the selected
options and starts a new request. Options remain selected when reopening the modal.

Original and Translated select which text is displayed and copied. Each view
remembers its scroll offset; the first translated view inherits the original's
current offset, clamped to the translated document's bounds. Changing file or
encoding clears the translation and reading positions. Nothing is written back
to the chart folder or archive.

Android translation is powered by [Google Translate](https://cloud.google.com/translate)
through ML Kit's on-device models. A prompt precedes missing-model downloads,
which can use mobile data. Once downloaded, models work offline; readme text is
not uploaded for translation. Android provides one Chinese target, as supported
by ML Kit. **Source** offers Auto detect, Japanese, English and Korean. Auto detect
calls the platform detector once with the whole README; manual selection skips
language detection. One source/target model pair is prepared per request.
Standard translation (including Android) splits only around HTTP/HTTPS and `www.`
URLs. Surrounding prose is translated, while the exact original URL bytes and
boundary whitespace are inserted unchanged. With no URLs, text remains one call.
High fidelity always receives the whole document without URL tokenization, retaining
its full context. No per-line language guessing or sampling is performed. Existing
reader input and translated-output byte limits apply to all pieces, including URLs.

ML Kit internally uses only the first 200 characters for detection ([reference](https://developers.google.com/android/reference/com/google/mlkit/nl/languageid/LanguageIdentifier)).
Choose a source manually if headings or mixed-language text give a wrong result.
The translation engine controls the translated text's line breaks; the original
text is always retained. New source, target and quality selections take effect
when Translate is confirmed; editing options alone does not clear the result.

Apple uses Translation on iOS 18+ and macOS 15+. On iOS/macOS 26.4+, **Standard (faster)** and
**High fidelity** buttons select the translation strategy. Standard is the default
and explicitly uses Apple’s low-latency model. The choice is retained while this
reader instance is alive. Older Apple systems and Android hide these controls.
High fidelity requests Apple Intelligence translation; Apple's framework falls back to its standard on-device
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
