# Android Documents access

## Accepted scope
Expose the complete Android Documents directory (including BMS, databases,
Skins and other user files) through a DocumentsProvider, like iOS Files. Preserve
copy-on-import and native filesystem reads during chart loading. Use a new
Documents subfolder beneath app files; keep private caches and import staging
outside that subtree. The user explicitly does not need backward compatibility
or migration for Android.
Rename `play`/`firebase` flavors to `restricted_file_access`/`all_file_access`;
select restricted access for local APK and Firebase testing by default.

## Implementation
1. Rename flavor configuration, scripts, workflow, documentation and related tests.
2. Add a permission-gated provider backed by external-files/Documents
   (internal-files/Documents fallback), matching GetDocumentsPath. Keep rendered
   music and archive indexes in Android private cache. Support browsing, streaming,
   creating, renaming and deleting. Guard document IDs, names, root mutation and
   symlink traversal. Keep cache and preferences outside the published root.
3. Offer an Android Open in Files action. Batch BMS edits until writers close and
   the app is resumed, then enqueue a normal library refresh. Persist pending
   changes across process restarts. Other document edits do not trigger scanning.
4. Test path containment and change coalescing with a Java harness; test provider
   CRUD and direct-file equivalence through the real Android ContentResolver.
   Run existing storage/build tests, compile desktop and signed restricted APK,
   install on the connected device, and verify the Files UI.
5. Review, commit and push task changes to the existing upstream. Build-only;
   publishing a Firebase release is outside this request.

## Validation focus
Root/parent/sibling escapes and symlinks; recursive self-import; incomplete copies; replacement writes;
revision acknowledgement races; full-root visibility; manifest permission and
variant boundaries; preservation of existing files and copy-based import behavior.
