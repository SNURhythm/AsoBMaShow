# Windows Parallel Build and Skin Portability Design

## Goal

Make the Visual Studio Debug build safe under CMake parallel builds and make
the application plus directly related skin targets compile and pass on
Windows/MSVC. Preserve the existing archive security checks, coordinator
exception-safety behavior, and non-Windows behavior.

## Scope

The implementation covers:

- the shared compiler-PDB race reported by MSVC as C1041;
- MSVC portability failures in `SkinCommitCoordinator.cpp`;
- Windows type and const-correctness failures in `SkinArchiveImporter.cpp`;
- any directly consequent compile or link failures in the selected application
  and skin test targets.

Unrelated warnings and unrelated test suites are outside scope unless they
prevent the selected targets from building or running.

## Design

### Parallel compiler PDB access

Add `/FS` to the existing global MSVC compile options. Visual Studio targets
normally compile many translation units into one target-level compiler PDB;
`/FS` asks `CL.EXE` to coordinate those writes through the MSVC PDB server.
This retains `/Zi` debugging information and parallel compilation. Switching
to `/Z7` is rejected because it changes debug-information storage and increases
object-file size solely to avoid the coordination problem.

### Coordinator portability and exception safety

Do not disable the failing type-trait assertions conditionally. Determine which
member types make MSVC's aggregate move/swap traits differ, then change the
coordinator's bounded-delivery and revalidation replacement operations so their
exception behavior is explicit and portable. A failed allocation or move must
leave the existing terminal result or revalidation request available for retry;
accepted tickets and leases must not be silently abandoned.

Use focused coordinator tests to preserve queue bounds, completion delivery,
retry, acknowledgement, and revalidation semantics. Compiler-only assertions
may remain where the asserted property is guaranteed by the implementation,
but the design must not rely on implementation-specific `std` aggregate traits.

### Archive importer portability

Use libarchive's exposed file-type expression type rather than POSIX `mode_t`,
which is absent in native MSVC builds. Keep comparisons against libarchive's
portable `AE_IF*` constants.

Make the Windows security-attributes accessor callable from logically const
path-opening operations. The Windows creation APIs accept a non-const pointer
but consume the descriptor as input; the implementation will express that API
boundary narrowly without removing constness from unrelated importer methods.
All private-owner/DACL validation and no-follow handle checks remain intact.

## Verification

Use the existing Visual Studio build tree and parallel builds throughout:

1. Configure/generate the tree.
2. Build `main` with `cmake --build ... --target main --parallel`.
3. Build these directly related test executables in parallel:
   - `skin_commit_coordinator_tests`
   - `skin_archive_importer_tests`
   - `skin_package_store_tests`
   - `skin_package_operation_service_tests`
   - `gameplay_skin_settings_tests`
4. Run the corresponding CTest cases in Debug configuration.
5. Run `tests/skin_archive_importer_windows_contract_tests.py`.

The work is complete only when the parallel builds exit successfully and all
selected tests pass. Any newly exposed failure will be diagnosed independently
before another source change is made.
