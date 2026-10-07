# Board

Questions and answers for contributors and the release integrator.

## Rules

- Add a dated, signed thread for an open question or a verified handoff.
- Distinguish code inspection, automated tests and real playback evidence.
- Keep unresolved runtime checks in [RUNTIME-VALIDATION.md](../RUNTIME-VALIDATION.md).
- Answer in place; do not mark an untested scenario passed when integrating code.

## Open

No open collaboration questions for the initial public release.
Runtime acceptance remains incomplete as recorded in the validation register.

## Initial public release verification - 2026-10-08

Agent: Codex
Scope: native Windows 1.0.0 release source, documentation and packaging.
Invariants: native C++20/MSVC/CMake/vcpkg, normal subtitle rendering and Emby
playback remain in the public build.
Tests: `powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1`
exited 0; 14/14 CTests passed in 35.46 seconds and the package was rebuilt.
`git diff --check` and independent source review passed.
Media: Not run; real playback, server and display checks remain open in the
runtime register. Automated tests do not establish that runtime acceptance.
Next: publish this source snapshot from its independent public Git history.

## Public documentation consolidation - 2026-10-08

Agent: Codex
Scope: initial release notes, English installation and compatibility guidance,
configuration-format documentation and contributor reading links.
Removed obsolete upgrade documents and consolidated current behavior under
the 1.0.0 release. Current application code and build configuration are unchanged.
Validation: documentation links and version references checked; package README
and CHANGELOG refreshed, with all other archive contents unchanged.
The earlier 14/14 CTest result still applies to the unchanged program; no native
rebuild or real-media test was run for this documentation-only change.
Next: publish the independent public repository after its remote is supplied.

## English documentation - 2026-10-08

Agent: Codex
Scope: translate the release notes, remove obsolete language notices, and require
English in the shared contributor workflow. All tracked documentation and the
package's document copies were checked; links in the changed documents remain valid.
Program sources and binaries are unchanged. Only README and CHANGELOG differ
inside the archive; no native rebuild or runtime test was needed for this edit.
Next: keep future documentation in English and publish only the independent
public repository after the user supplies its remote.

## README presentation - 2026-10-08

Agent: Codex
Scope: project icon, live build badge, version/platform/licence badges, feature
summary, navigation, expandable feature details and practical getting-started links.
Public baseline: `b18cc19` on `maddoxhuang/Quaddeck`.
The icon uses the public source URL so the packaged README can display it online
without requiring an extra assets directory. CI packages are identified as such;
no published Release or completed CI result is implied.
Validation: English text, document anchors and links, packaged README consistency
and unchanged executable/dependency hashes. No native rebuild was needed.
Next: review the rendered public README and retain the existing runtime checklist.
