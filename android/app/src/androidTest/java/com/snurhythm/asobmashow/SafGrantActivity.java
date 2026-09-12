package com.snurhythm.asobmashow;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.util.Log;

public final class SafGrantActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        picker.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
                | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        picker.putExtra(DocumentsContract.EXTRA_INITIAL_URI, Uri.parse(
                "content://com.android.externalstorage.documents/document/primary%3ADownload%2FAsoBMaShowTask5Saf"));
        startActivityForResult(picker, 1);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == 1 && resultCode == RESULT_OK && data != null && data.getData() != null) {
            Uri selected = data.getData();
            String expected = "content://com.android.externalstorage.documents/tree/primary%3ADownload%2FAsoBMaShowTask5Saf";
            if (!expected.equals(selected.toString())) {
                throw new AssertionError("Select only the isolated regression fixture folder");
            }
            grantUriPermission("com.snurhythm.asobmashow", selected,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION
                            | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
                            | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
            Log.i("AsoBMaShowPlatformTest", "Granted selected SAF fixture to target application");
        }
        finish();
    }
}
