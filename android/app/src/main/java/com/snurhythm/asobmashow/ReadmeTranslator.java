package com.snurhythm.asobmashow;

import android.app.Activity;
import android.app.AlertDialog;
import android.util.Log;
import com.google.mlkit.common.model.DownloadConditions;
import com.google.mlkit.common.model.RemoteModelManager;
import com.google.mlkit.nl.languageid.LanguageIdentification;
import com.google.mlkit.nl.languageid.LanguageIdentifier;
import com.google.mlkit.nl.translate.TranslateLanguage;
import com.google.mlkit.nl.translate.TranslateRemoteModel;
import com.google.mlkit.nl.translate.Translation;
import com.google.mlkit.nl.translate.Translator;
import com.google.mlkit.nl.translate.TranslatorOptions;
import com.google.android.gms.tasks.Task;
import com.google.android.gms.tasks.Tasks;
import org.json.JSONArray;
import org.json.JSONObject;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

// All state belongs to the Activity's UI thread; callbacks never retain a native view.
final class ReadmeTranslator {
    private final Activity activity;
    private long request;
    private Translator translator;
    private String translatorSource, destination;
    private LanguageIdentifier identifier;
    private AlertDialog prompt;
    private JSONArray parts, samples;
    private JSONObject labels;
    private final ReadmeTranslationOutput output = new ReadmeTranslationOutput(4 * 1024 * 1024);
    private static native void deliver(long id, int state, byte[] text);

    ReadmeTranslator(Activity activity) { this.activity = activity; }
    private boolean active(long id) { return request == id && !activity.isDestroyed(); }
    private void publish(long id, int state, String text) {
        if (active(id)) deliver(id, state, text.getBytes(StandardCharsets.UTF_8));
    }
    void cancel(long id) {
        if (id != 0 && request != id) return;
        request = 0;
        if (prompt != null) { prompt.dismiss(); prompt = null; }
        if (identifier != null) { identifier.close(); identifier = null; }
        if (translator != null) { translator.close(); translator = null; }
        translatorSource = null;
        parts = null;
        output.clear();
    }
    private void finish(long id, int state, String text) {
        if (!active(id)) return;
        if (state == 2 && text.getBytes(StandardCharsets.UTF_8).length > 4 * 1024 * 1024) {
            state = 3; text = "";
        }
        publish(id, state, text);
        cancel(id);
    }
    private boolean append(long id, String text) {
        if (output.append(text)) return true;
        finish(id, 3, "");
        return false;
    }
    private void fail(long id, Exception error) {
        if (!active(id)) return;
        Log.w("ReadmeTranslation", "On-device translation failed", error);
        finish(id, 3, "");
    }
    void start(long id, byte[] data, String target) {
        cancel(0);
        request = id;
        try {
            labels = new JSONObject(new String(data, StandardCharsets.UTF_8));
            parts = labels.getJSONArray("parts");
            samples = labels.getJSONArray("samples");
            destination = TranslateLanguage.fromLanguageTag(target);
            if (destination == null) { finish(id, 3, ""); return; }
            identifier = LanguageIdentification.getClient();
            identifyNext(id, 0);
        } catch (Exception error) { fail(id, error); }
    }
    private Task<Boolean> modelPresent(String language) {
        // ML Kit's per-language models translate to/from English; English has no separate pack.
        if (TranslateLanguage.ENGLISH.equals(language)) return Tasks.forResult(true);
        return RemoteModelManager.getInstance().isModelDownloaded(new TranslateRemoteModel.Builder(language).build());
    }
    private void prepare(long id, String source, int index) {
        if (source.equals(translatorSource)) { translate(id, index); return; }
        if (translator != null) translator.close();
        translator = Translation.getClient(new TranslatorOptions.Builder()
            .setSourceLanguage(source).setTargetLanguage(destination).build());
        translatorSource = source;
        Tasks.whenAllSuccess(modelPresent(source), modelPresent(destination))
            .addOnSuccessListener(results -> {
                if (!active(id)) return;
                if (Boolean.TRUE.equals(results.get(0)) && Boolean.TRUE.equals(results.get(1))) {
                    translate(id, index); return;
                }
                prompt = new AlertDialog.Builder(activity)
                    .setTitle(labels.optString("translate"))
                    .setMessage(labels.optString("download_message") + "\n\n" +
                        Locale.forLanguageTag(source).getDisplayLanguage() + " → " +
                        Locale.forLanguageTag(destination).getDisplayLanguage())
                    .setPositiveButton(labels.optString("download"), (dialog, which) -> download(id, index))
                    .setNegativeButton(labels.optString("cancel"), (dialog, which) -> finish(id, 4, ""))
                    .setOnCancelListener(dialog -> finish(id, 4, ""))
                    .show();
            }).addOnFailureListener(error -> fail(id, error));
    }
    private void download(long id, int index) {
        if (!active(id)) return;
        publish(id, 0, "");
        translator.downloadModelIfNeeded(new DownloadConditions.Builder().build())
            .addOnSuccessListener(unused -> { if (active(id)) translate(id, index); })
            .addOnFailureListener(error -> fail(id, error));
    }
    private void identifyNext(long id, int index) {
        if (!active(id)) return;
        // Keep blank lines, decorative separators and numbers exactly as written.
        while (index < parts.length() && !parts.optString(index).codePoints().anyMatch(Character::isLetter)) {
            if (!append(id, parts.optString(index++))) return;
        }
        if (index == parts.length()) { finish(id, 2, output.toString()); return; }
        publish(id, 1, "");
        final int current = index;
        identifier.identifyLanguage(parts.optString(samples.optInt(index, index))).addOnSuccessListener(language -> {
            if (!active(id)) return;
            String source = TranslateLanguage.fromLanguageTag(language);
            // Short author names can be guessed as an unsupported language.
            // Preserve those segments rather than discarding the translated notes.
            if (source == null || destination.equals(source)) {
                if (append(id, parts.optString(current))) identifyNext(id, current + 1);
            } else prepare(id, source, current);
        }).addOnFailureListener(error -> fail(id, error));
    }
    private void translate(long id, int index) {
        if (!active(id)) return;
        publish(id, 1, "");
        translator.translate(parts.optString(index)).addOnSuccessListener(text -> {
            if (!active(id)) return;
            if (append(id, text)) identifyNext(id, index + 1);
        }).addOnFailureListener(error -> fail(id, error));
    }
}
