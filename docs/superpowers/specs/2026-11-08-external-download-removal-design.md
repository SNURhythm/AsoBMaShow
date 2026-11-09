# External BMS download removal and public apology

The developer has committed to removing BMS downloads and the embedded browser
while service permissions are reviewed. The user requested implementation on a
new branch from `develop`, with the public apology available inside the app in
every supported language and the Korean text preserved verbatim.

## Approach

Remove the provider clients, browser and download runtime, rather than hide their
buttons or retain a feature flag. Remove their controls from both selectors,
Tasks, Settings and the newcomer tutorial. Retire scheduled native transfers
without resuming them. Keep already imported charts and private staging files;
local folder/archive import, difficulty tables, storage tools and IR remain.

Add a persistent, clearly labeled apology action on Intro, including keyboard
and configured-controller navigation. Open a scrollable, offline document using
the existing reader. Bundle the exact final Korean draft and faithful English,
Japanese, Simplified Chinese and Traditional Chinese translations. Select the
app language; include access to the Korean original from translated documents.

## Constraints

- No requests to HorieYuuka services, including during validation.
- The user authorized iOS Firebase deployment after verification and later
  requested commit, push, merge into `develop`, and fast-forward of `main` on
  November 9. No separate public post or manual deployment is requested.
- Work in this checkout; do not create a worktree.
- Preserve existing user charts, scores, profiles and local imports.
- Do not alter the user's Korean apology to claim planned actions are complete.
- Preserve ordinary source style; no whole-file formatting.

## Verification

Cover removal of packaged native download/browser components, unreachable
download actions in both selectors/tutorial, legacy native transfer retirement,
Intro navigation to the notice, multilingual asset selection, verbatim Korean
content and existing local library workflows. Build `main` and affected groups
in the existing build folder, serializing build invocations. Validate mobile
source/manifest integration before the separately authorized iOS Firebase
deployment.
