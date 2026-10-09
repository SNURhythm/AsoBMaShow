# SDL fork maintenance

The SDL3 migration uses separate upstream release and application patch branches:

| Submodule | Official upstream | Clean release branch | Application patch branch |
| --- | --- | --- | --- |
| SDL | https://github.com/libsdl-org/SDL.git | `release-3.4.x` | `asobmashow/release-3.4.x` |
| SDL_ttf | https://github.com/libsdl-org/SDL_ttf.git | `release-3.2.x` | `asobmashow/release-3.2.x` |

In each checkout, `origin` is the SNURhythm fork and `upstream` is the official
repository. Remotes are local Git configuration; add `upstream` from the table
when preparing another checkout. `.gitmodules` tracks the application patch
branches, while the parent repository pins exact commits.

Keep application changes on the patch branches. Keep the fork's clean release
branches aligned with upstream using fast-forward updates. To incorporate an
upstream update, run the following inside the appropriate submodule, replacing
`<release>` with its release branch from the table:

```sh
git fetch upstream
git switch <release>
git merge --ff-only upstream/<release>
git push origin <release>
git switch asobmashow/<release>
git merge upstream/<release>
git submodule update --init --recursive
```

Resolve conflicts and validate the affected platforms before pushing the patch
branch and committing the new submodule pointer in AsoBMaShow. Preserve published
patch history by merging upstream changes rather than rebasing and force-pushing.
If a shallow checkout cannot find the merge base, deepen its upstream history
before merging; do not use `--allow-unrelated-histories`.

The existing SDL2 branches retain the earlier application patches for reference.
Creating the SDL3 patch branches does not port those patches or complete the
application's SDL3 API/build migration.
