# Clipper1 DSP

Everything is in `Source/dsp/`; `ClipCurve.h` has no dependency, `Clipper.*` links only JUCE's
`juce_dsp` (for `dsp::Oversampling`). Measurements below come from `tests/clipper1_test.cpp`.

## Signal flow (per channel, dual mono)

```
in -> sanitise (NaN / inf -> 0) -> x InputGain (L or R)            [peak meter "input"]
   -> x 1 / Threshold          (u = 1 at the Threshold)
   -> oversample up (0, 2, 4, 8x)  ->  y = shape(u, w)  ->  oversample down
   -> x Ceiling -> clamp to +/- Ceiling (base rate)  -> x fade gain -> out   [peak meter "output"]
```

Below the knee `y = u`, so the gain is InputGain x Ceiling / Threshold: **Ceiling - Threshold** in dB.
Channels are clipped independently (a sample-wise curve is the clipper; linking the channels
would change what the curve does). Mono buses use Input L.

## Curve

`shape(u, w)` with knee half-width `w = Softness / 100`:

| range of \|u\| | y |
| --- | --- |
| up to 1 - w | \|u\| |
| 1 - w .. 1 + w | \|u\| - (\|u\| - (1 - w))^2 / (4 w) |
| from 1 + w | 1 |

Odd symmetric. `y(1) = 1 - w / 4`; the ceiling is reached at `(1 + w) x Threshold`. At `w = 0` it is exactly
`min(|u|, 1)`. Slope falls linearly from 1 to 0 across the knee, so the curve is C1 for `w > 0`. In dB:
the knee starts `20 log10(1 - w)` below the Threshold (Softness 50 %: 6 dB; 100 %: from zero level).
Chosen for this plug-in (Custom provenance; the construction is the compressor soft knee of
Giannoulis, Massberg and Reiss 2012 applied to level). The editor's curve display calls the same
function. Checked: exact line below the knee, exact hard clip at 0 %, never above 1, odd, monotonic,
continuous, C1.

## Ceiling guarantee

At the plug-in's own rate (oversampling Off) `|out| <= Ceiling` exactly (the curve is clamped at 1
in floating point and the output is clamped again); tested on sines, a Nyquist-ish tone, squares,
impulses, steps, noise and noise plus bass, at 44.1, 48, 96 and 192 kHz, 4 thresholds x 4 ceilings x
3 softnesses (384 cases per rate): worst output = 1.0000000 x ceiling.

With oversampling the downsampling filter overshoots a flat-topped signal. Measured on a hard-clipped
full-scale square (Threshold -12 dB, Ceiling -1 dB): 2x up to 1.29 dB, 4x up to 2.57 dB, 8x up to
1.55 dB over the ceiling before the final clip (the same at every sample rate, since the filters are
fixed half-bands). A final clip at the base rate removes it, so `|out| <= Ceiling` holds in every
mode; it only acts on those overshoot peaks, and being a hard clip it can fold a little of itself
back. Programme material overshoots far less than this worst case. Parameter changes are smoothed,
so for about 50 ms after lowering the Ceiling the output may sit above the new value (it follows the
smoothed ceiling).

## Oversampling

`juce::dsp::Oversampling<float>`, 2 channels, `filterHalfBandFIREquiripple`, maximum quality, integer
latency. One object per factor is built in `prepare()` (nothing is allocated afterwards). Latency, from
the oversampler, identical at every sample rate: Off 0, 2x 49, 4x 61, 8x 65 samples. Reported to the
host at prepare and whenever the factor changes (the change is detected on the audio thread and
`setLatencySamples` runs from an `AsyncUpdater`).

A factor change fades the output out over 256 samples, swaps (and resets) the oversampler at silence,
waits out the new filter's delay, and fades in over 256 samples: no click, about 10 ms of dip.

Aliasing, measured on a 1.18 kHz tone hard-clipped with the Threshold 30 dB below it (a near-square):
energy outside the true harmonics, relative to total: Off -20.6 dB, 4x -30.0 dB, 8x -31.2 dB. The floor is the
final half-band filter's transition band (about 21.6 to 26.4 kHz at 48 kHz): harmonics inside it fold back
partially whatever the factor.

## Smoothing

Input gains, 1/Threshold, knee and Ceiling are one-pole smoothed (time constant 10 ms, per base-rate
sample, snapping exactly onto the target). The knee is held across the oversampled sub-samples.
The tests derive the largest legal sample step from the coefficient and check it (input gain,
threshold, ceiling, softness, factor change); an unsmoothed step would be about 50 to 500 times larger.

## Clip meter

Largest `|u| / |y|` seen at the clip's own rate in the block, in dB (the peak reduction the curve
applied; the final base-rate trim is not included). Raw block maxima, ballistics in the editor.
