# Selector navigation and compact rows

Implement the requested PLAY, COURSE, MUSIC and IR tabs in the built-in selector.
PLAY omits courses; COURSE shows the former Courses children directly. Preserve
folder, selection and scroll state separately for the two selector tabs. Lazy,
shell-owned MusicPlayerScene and IrUploadsScene instances retain their existing
features and state. Forward lifecycle, language, input and deferred work without
registering those embedded instances as SceneManager destinations. IR Settings
returns to the shell; profile changes retire preparation before replacing data.

Remove the list column heading and numeric index. Stack title and artist, restore
a slim left clear lamp, and reserve jacket space with a placeholder when absent.
Extend the bottom surface through the safe area and inset actionable controls.
Keep all four tabs reachable in narrow/portrait layouts.

Implementation: compact row/details views; utility scene embedding; shell/tab and
folder integration; serialized desktop build, relevant native/lifecycle/layout
tests, and visual smoke checks. Commit and push task files after verification.

Completed: desktop build, 21 affected checks and all 483 CTest tests passed.
Final four-tab screenshots and validation limits are recorded in design-qa.md.
