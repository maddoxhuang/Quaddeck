<!--
Required format for every public QuadDeck GitHub Release.

Copy this file to an ignored working file, such as build/release-notes.md.
Use the release title "QuadDeck {{VERSION}}"; VERSION and PREVIOUS_VERSION
contain only the version number, without the leading v.

Replace every {{PLACEHOLDER}}, remove these author comments, and keep the
five section headings in this order. Write in English. Use short change
bullets with bold topic labels; repeat or remove the example bullet as needed.
Confirm the package requirements and compatibility against the release source.

Record actual results for this version and package. Do not copy verification results,
CI links, checksums or pass claims from an earlier release. Distinguish passed,
failed, skipped, not run and still-running checks. Keep automated results
separate from real-media, service, input-method, audio and hardware acceptance.
If a check was not run, say so; if no CI run exists, omit its link.

Use version-tagged document and asset links. Replace CHANGELOG_ANCHOR with the
section slug without #, or remove the # and placeholder to link the whole file.
For the first public release, remove the previous-version comparison link.
Keep private paths, credentials and preparation diaries out of the final notes.
This template defines the format; it does not authorize publication.
-->

QuadDeck {{VERSION}} {{ONE_SENTENCE_RELEASE_SUMMARY}}.

## What's new

- **{{TOPIC}}:** {{USER_VISIBLE_CHANGE}}.

## Download and run

1. Download [**QuadDeck-win-x64.zip**](https://github.com/maddoxhuang/Quaddeck/releases/download/v{{VERSION}}/QuadDeck-win-x64.zip) and [**SHA256SUMS.txt**](https://github.com/maddoxhuang/Quaddeck/releases/download/v{{VERSION}}/SHA256SUMS.txt).
2. Verify the ZIP against the checksum, then close any running copy of QuadDeck.
3. Extract the complete archive into a new folder and run `QuadDeck\QuadDeck.exe`. Keep the EXE, DLLs, shaders and licenses together.

Requires **Windows 10/11 x64** and the [Microsoft Visual C++ v14 x64 runtime](https://aka.ms/vc14/vc_redist.x64.exe). This is a portable package; no QuadDeck installer is required.

## Compatibility

Back up `%LOCALAPPDATA%\QuadDeck\settings.qconfig`, saved sessions and visual presets before switching builds. Copies running under the same Windows account share the settings file.

- {{COMPATIBILITY_WITH_PREVIOUS_VERSIONS}}. Formats: `QCONFIG {{QCONFIG_VERSION}}`, `QDECK {{QDECK_VERSION}}` and `QSTYLE {{QSTYLE_VERSION}}`.
- {{VERSION_SPECIFIC_REQUIREMENTS_OR_LIMITATIONS}}.
<!-- Remove the second bullet if this version has no additional requirements or limitations. -->

## Validation

- **Local build:** {{BUILD_RESULT_FOR_THIS_SOURCE_REVISION}}.
- **Packaged dependencies:** {{FINAL_PACKAGE_LAUNCH_AND_DEPENDENCY_CHECKS}}.
- **Release checks:** {{PRIVACY_AND_ARCHIVE_VALIDATION_RESULTS}}.
- **Windows CI:** {{CI_BUILD_RESULT_WITH_RUN_LINK}}.

{{RUNTIME_CHECKS_COMPLETED_AND_REMAINING_ACCEPTANCE_LIMITS}}

## Links

- [Full changelog](https://github.com/maddoxhuang/Quaddeck/blob/v{{VERSION}}/CHANGELOG.md#{{CHANGELOG_ANCHOR}})
- [User guide](https://github.com/maddoxhuang/Quaddeck/blob/v{{VERSION}}/README.md)
- [Changes since v{{PREVIOUS_VERSION}}](https://github.com/maddoxhuang/Quaddeck/compare/v{{PREVIOUS_VERSION}}...v{{VERSION}})
