# Clipper1 release notes

## v0.1.1

- Meters: the Clip meter and the input/output peak meters now show every clip and peak between two
  screen refreshes. Before, a short clip (a snare hit among fast kicks) could be missed by the
  display. Audio is unchanged.
- Link between Threshold and Out Ceiling is now on by default for new instances; saved sessions keep
  their setting.
- Checked before release: CTest 1/1 (clipper1_test, ALL PASSED, 0 failures); editor snapshot rendered
  and identical to the development build's docs/editor.png; AU and VST3 slices arm64 + x86_64.
- Windows x64: result pending.

## v0.1.0

Clipper1 is a stereo clipper with a Threshold, an Out Ceiling, an adjustable soft knee and 2x, 4x or
8x oversampling. It is an independent plug-in identity (`com.c0dypeng.clipper1`, `Cdpg` / `Clp1`).

- macOS Universal: AU and VST3, arm64 + x86_64.
- Windows x64: VST3, built natively with MSVC on windows-2022; CTest 1/1 (clipper1_test, 0 failures)
  passed there. Requires the Microsoft Visual C++ x64 Redistributable.
- macOS bundles are ad-hoc signed, not notarized. Windows builds are unsigned.
- Checked before release: CTest 1/1 (clipper1_test: 77 checks, 0 failures); editor snapshot rendered; AU and VST3 slices arm64 + x86_64; ad-hoc signatures, ZIP integrity and SHA-256 verified by the packaging script.

Known limits: the ceiling is a sample-value ceiling at the plug-in's own rate (a final clip enforces
it in every mode); between-sample (true-peak) overs are not controlled. Aliasing is reduced, not
removed. Latency is 0 / 49 / 61 / 65 samples for Off / 2x / 4x / 8x and is reported to the host. After
a change the ceiling follows its smoothed value for about 50 ms, and an oversampling change fades the
sound for about 10 ms.

Each ZIP includes installation instructions, dependency notices and build provenance, with a
SHA-256 checksum companion. No DAW-host test has been run.
