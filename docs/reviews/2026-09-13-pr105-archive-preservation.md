# PR #105: full-extraction preservation follow-up

## Findings addressed

- [Windows devices, streams and aliases](https://github.com/SNURhythm/AsoBMaShow/pull/105#discussion_r3996738780):
  full-extraction manifest preflight rejects unsafe Win32 components before
  creating output or recording recovery work. The policy applies on Windows;
  valid names on other platforms are not unnecessarily restricted. Rules cover
  device names and their extensions, COM/LPT superscript digits, alternate data
  streams, invalid/control characters, and ASCII whitespace/dot aliases.
- [Marker-only output reuse](https://github.com/SNURhythm/AsoBMaShow/pull/105#discussion_r3996833406):
  reused folders remain available for indexing, but their results explicitly
  prohibit source deletion. Both the UI eligibility check and deletion worker
  enforce this, including unchanged reused folders: a marker cannot certify
  current contents. The result explains why the original is retained. Batch
  **Delete Originals** continues to extract into fresh folders.
- [Colliding output paths](https://github.com/SNURhythm/AsoBMaShow/pull/105#discussion_r3996833411):
  every fresh full extraction checks its destination namespace, even with one
  worker. Duplicate files, file/directory conflicts, and filesystem-specific
  aliases fail before payload writes and completion-marker publication. Shared
  exact parent directories remain valid.

## Stable output reservations

File outputs are reserved with exclusive creation (`CREATE_NEW` on Windows,
`O_CREAT | O_EXCL` on POSIX). Successful reservations remain in place while
the backends write into those files. This also preserves creation-time Windows
8.3 aliases: deleting probes and recreating outputs in a different worker order
could otherwise produce collisions that were absent during preflight. Failed
preflight removes its file reservations; the existing incomplete-output journal
continues to own recovery cleanup.

## Regression coverage

- Portable Windows-policy tests cover 2,076 accepted/rejected component and
  path cases. Rules follow Microsoft's [naming reference](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file),
  [whitespace normalization documentation](https://learn.microsoft.com/en-us/troubleshoot/windows-client/shell-experience/file-folder-name-whitespace-characters),
  and [console-device/stream documentation](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew).
- ZIP, TAR, 7z and RAR fixtures exercise aliases, duplicates, conflicting
  prefixes and valid shared parents with one and four workers. Rejected
  archives retain their original bytes and write no payload or completion marker.
- Hard-link snapshots verify that empty reserved outputs retain their identities
  and receive the correct payload across all four formats. ZIP fixtures include
  both long-name/short-name creation orders.
- Missing and same-sized corrupted keysounds/BGA files do not regain deletion
  eligibility through an intact marker. Fresh extraction still permits deletion.
- The collision, reservation-retention and reused-deletion regressions were
  observed failing against their respective earlier implementations.

Native Windows execution is not available on this machine. The portable policy
suite runs on macOS; native alias fixtures adapt to the actual filesystem's
case/normalization/short-name behavior. No deployment is part of this follow-up.

## Final verification

- All-target desktop build: `cmake --build cmake-build-debug -j 6` passed.
- Focused policy, extraction and operation suites: 3/3 passed.
- Full parallel CTest: 361/361 passed in 87.52 seconds.
- The preceding full run had one 40-second `visual_catch_up` timeout. That
  unrelated suite passed unchanged in 0.18 seconds when rerun, followed by the
  clean full run. No video code or test timeouts were changed.
- Independent final code review found no remaining actionable findings in this
  follow-up. `git diff --check` passed.
