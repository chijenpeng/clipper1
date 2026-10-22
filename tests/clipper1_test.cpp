// Offline checks of the clipper. Prints PASS/FAIL per check and returns non-zero when anything
// failed. Links only JUCE's DSP module (the oversampler); no host, no screen.
//
// Portability: the random generator is a hand-written xorshift (never std::*_distribution in
// an assertion), every draw is its own statement, and lambdas capture constants at namespace scope.

#include "dsp/ClipCurve.h"
#include "dsp/Clipper.h"
#include "dsp/LinkedControls.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

using namespace clipper;

//==============================================================================
// Allocation counter for the real-time check: counts operator new while armed.
namespace
{
std::atomic<bool> gCountAllocations { false };
std::atomic<long> gAllocations { 0 };

void* countedAlloc (std::size_t size)
{
    if (gCountAllocations.load())
        ++gAllocations;
    if (void* p = std::malloc (size > 0 ? size : 1))
        return p;
    throw std::bad_alloc();
}
} // namespace

void* operator new (std::size_t size) { return countedAlloc (size); }
void* operator new[] (std::size_t size) { return countedAlloc (size); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

//==============================================================================
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kBlock = 512;
const double kRates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };
int failures = 0;

std::string fmt (const char* f, ...)
{
    char buffer[1024];
    va_list args;
    va_start (args, f);
    std::vsnprintf (buffer, sizeof (buffer), f, args);
    va_end (args);
    return buffer;
}

void check (bool ok, const std::string& what)
{
    std::printf ("  %s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (! ok)
        ++failures;
}

struct Rng
{
    uint32_t state = 2463534242u;
    uint32_t next() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
    float bipolar()   // -1..1
    {
        const uint32_t v = next();
        return (float) ((double) v / 2147483648.0 - 1.0);
    }
};

float peakOf (const std::vector<float>& x, size_t from = 0)
{
    float p = 0.0f;
    for (size_t i = from; i < x.size(); ++i)
        p = std::max (p, std::abs (x[i]));
    return p;
}

std::vector<float> sine (double fs, double hz, double amp, size_t n)
{
    std::vector<float> x (n);
    for (size_t i = 0; i < n; ++i)
        x[i] = (float) (amp * std::sin (2.0 * kPi * hz * (double) i / fs));
    return x;
}

/** Runs a mono signal through the clipper, as both channels, in blocks; returns the left channel. */
std::vector<float> run (Clipper& c, const Params& p, const std::vector<float>& in, int block = kBlock)
{
    std::vector<float> l (in), r (in);
    for (size_t pos = 0; pos < in.size(); pos += (size_t) block)
    {
        const int n = (int) std::min ((size_t) block, in.size() - pos);
        float* ch[2] = { l.data() + pos, r.data() + pos };
        c.setParams (p);
        c.process (ch, 2, n);
    }
    return l;
}

Params params (float thr, float ceil, float soft, int os)
{
    Params p;
    p.thresholdDb = thr;
    p.ceilingDb = ceil;
    p.softnessPercent = soft;
    p.oversamplingLog2 = os;
    return p;
}

//==============================================================================
void testCurve()
{
    std::printf ("curve\n");
    bool unity = true, ceilingOk = true, odd = true, monotonic = true, continuous = true, hardExact = true, c1 = true;
    double worstStep = 0.0;

    for (int s = 0; s <= 100; s += 5)
    {
        const double w = softnessToKnee ((double) s);
        double prev = 0.0;
        double prevSlope = 1.0;
        const double du = 1.0e-4;
        for (double u = 0.0; u <= 4.0; u += du)
        {
            const double y = shape (u, w);
            if (u <= 1.0 - w && std::abs (y - u) > 1e-12) unity = false;
            if (y > 1.0) ceilingOk = false;
            if (std::abs (shape (-u, w) + y) > 0.0) odd = false;
            if (y + 1e-15 < prev) monotonic = false;
            worstStep = std::max (worstStep, std::abs (y - prev));
            if (u > 0.0 && std::abs (y - prev) > du * 1.0000001) continuous = false;   // Lipschitz 1: no jump
            if (u > 0.0)
            {
                const double slope = (y - prev) / du;
                if (w > 0.0 && std::abs (slope - prevSlope) > 2.0 * du / (4.0 * w) + 1e-6) c1 = false;   // slope changes at 1/(2w) per unit
                prevSlope = slope;
            }
            prev = y;
        }
        if (s == 0)
            for (double u = -4.0; u <= 4.0; u += 0.001)
                if (shape (u, 0.0) != std::max (-1.0, std::min (1.0, u))) hardExact = false;
    }
    check (unity, "exact straight line (gain 1) below the knee for every softness");
    check (hardExact, "0 % softness is the exact hard clip min(|u|, 1)");
    check (ceilingOk, "never above 1 (the ceiling) for softness 0..100 %, |u| up to 4");
    check (odd, "odd symmetry");
    check (monotonic, "monotonic non-decreasing");
    check (continuous, "continuous (no step larger than the input step, slope <= 1)");
    check (c1, "slope continuous for softness > 0 (no corner)");

    // The knee mapping, stated numerically: y(1) = 1 - w/4; reaches 1 at u = 1 + w.
    bool mapping = true;
    for (int s = 10; s <= 100; s += 10)
    {
        const double w = s / 100.0;
        if (std::abs (shape (1.0, w) - (1.0 - w / 4.0)) > 1e-12) mapping = false;
        if (std::abs (shape (1.0 + w, w) - 1.0) > 1e-12) mapping = false;
    }
    check (mapping, "knee mapping: y(Threshold) = 1 - w/4 and the ceiling is reached at (1 + w) x Threshold");
    check (std::abs (shape (0.5f, 0.0f) - 0.5f) == 0.0f && std::abs (shape (2.0f, 0.3f) - 1.0f) == 0.0f, "float instantiation agrees");
}

//==============================================================================
void testGain()
{
    std::printf ("gain\n");
    for (double fs : kRates)
    {
        Clipper c;
        c.prepare (fs, kBlock);
        // Threshold -12, ceiling -1: 11 dB of gain below the knee.
        const auto in = sine (fs, 997.0, 0.01, (size_t) fs / 2);   // -40 dBFS
        const auto out = run (c, params (-12.0f, -1.0f, 0.0f, 0), in);
        double worst = 0.0;
        for (size_t i = 0; i < in.size(); ++i)
            worst = std::max (worst, (double) std::abs (out[i] - in[i] * dbToGain (11.0f)));
        check (worst < 1e-6, fmt ("%.0f Hz: Ceiling - Threshold = +11 dB below the knee (worst error %.2g)", fs, worst));
    }

    // Softness 50 %: the knee starts at 0.5 x Threshold (-6 dB below it); a signal 12 dB below stays at gain 1.
    Clipper c;
    c.prepare (48000.0, kBlock);
    const auto in = sine (48000.0, 997.0, 0.1, 24000);
    const auto out = run (c, params (-6.0f, -6.0f, 50.0f, 0), in);
    double worst = 0.0;
    for (size_t i = 0; i < in.size(); ++i)
        worst = std::max (worst, (double) std::abs (out[i] - in[i]));
    check (worst < 1e-6, "unity below the knee with softness 50 % (Threshold = Ceiling)");

    // Exact hard clip at 0 %: out = ceiling x min(|x g / T|, 1), sample for sample.
    Clipper h;
    h.prepare (48000.0, kBlock);
    Rng rng;
    std::vector<float> noise (48000);
    for (auto& v : noise)
        v = 0.5f * rng.bipolar();
    const auto hard = run (h, params (-10.0f, -3.0f, 0.0f, 0), noise);
    const float c3 = dbToGain (-3.0f), tScale = dbToGain (10.0f);
    double err = 0.0;
    float top = 0.0f;
    for (size_t i = 0; i < noise.size(); ++i)
    {
        const float expect = c3 * shape (noise[i] * tScale, 0.0f);
        err = std::max (err, (double) std::abs (hard[i] - expect));
        top = std::max (top, std::abs (hard[i]));
    }
    check (err < 1e-6, fmt ("0 %% softness is an exact hard clip at the ceiling (worst error %.2g)", err));
    check (top == c3, "the clipped peaks are exactly the ceiling");

    // Dual mono: the right channel is processed on its own, with its own input gain.
    Clipper d;
    d.prepare (48000.0, kBlock);
    Params p = params (0.0f, 0.0f, 0.0f, 0);
    p.inputGainDbR = -6.0f;
    std::vector<float> l (1000, 0.5f), r (1000, 0.5f);
    float* ch[2] = { l.data(), r.data() };
    d.setParams (p);
    d.process (ch, 2, 1000);
    check (std::abs (l[999] - 0.5f) < 1e-6f && std::abs (r[999] - 0.5f * dbToGain (-6.0f)) < 1e-6f, "Input L and Input R apply to their own channel");

    // A mono bus works.
    Clipper m;
    m.prepare (48000.0, kBlock);
    std::vector<float> mono (1000, 2.0f);
    float* mch[1] = { mono.data() };
    m.setParams (params (0.0f, -6.0f, 0.0f, 2));
    m.process (mch, 1, 1000);
    check (std::isfinite (mono[999]), "mono processing with oversampling is finite");

    // Non-finite input never reaches the output.
    Clipper n;
    n.prepare (48000.0, kBlock);
    std::vector<float> bad (2000, 0.3f);
    bad[100] = NAN;
    bad[200] = INFINITY;
    bad[300] = -INFINITY;
    const auto cleaned = run (n, params (0.0f, 0.0f, 20.0f, 2), bad);
    bool finite = true;
    for (float v : cleaned)
        finite = finite && std::isfinite (v);
    check (finite && std::abs (cleaned[1999] - 0.3f) < 1e-3f, "NaN and infinity are replaced and do not poison the oversampler");
}

//==============================================================================
std::vector<std::vector<float>> signalsFor (double fs)
{
    std::vector<std::vector<float>> s;
    const size_t n = (size_t) (fs * 0.25);
    s.push_back (sine (fs, 1000.0, 1.0, n));
    s.push_back (sine (fs, 0.4 * fs, 1.0, n));
    s.push_back (sine (fs, 0.2 * fs, 3.0, n));
    std::vector<float> square (n), impulses (n, 0.0f), steps (n), noise (n), mixed (n);
    Rng rng;
    for (size_t i = 0; i < n; ++i)
    {
        square[i] = ((i / 37) % 2 == 0) ? 1.0f : -1.0f;
        steps[i] = ((i / 700) % 2 == 0) ? 0.9f : -0.9f;
        noise[i] = rng.bipolar();
        mixed[i] = 0.7f * (float) std::sin (2.0 * kPi * 80.0 * (double) i / fs) + 0.4f * noise[i];
    }
    for (size_t i = 100; i < n; i += 997)
        impulses[i] = (i / 997) % 2 == 0 ? 1.0f : -1.0f;
    s.push_back (square);
    s.push_back (impulses);
    s.push_back (steps);
    s.push_back (noise);
    s.push_back (mixed);
    return s;
}

void testCeiling()
{
    std::printf ("ceiling\n");
    const float thresholds[] = { 0.0f, -6.0f, -18.0f, -30.0f };
    const float ceilings[] = { 0.0f, -0.3f, -6.0f, -30.0f };
    const float softnesses[] = { 0.0f, 35.0f, 100.0f };

    for (double fs : kRates)
    {
        Clipper c;
        c.prepare (fs, kBlock);
        const auto signals = signalsFor (fs);
        long cases = 0;
        float worstExcess = 0.0f;   // output / ceiling, linear; must be <= 1
        for (const auto& sig : signals)
            for (float t : thresholds)
                for (float ce : ceilings)
                    for (float so : softnesses)
                    {
                        c.reset();
                        const auto out = run (c, params (t, ce, so, 0), sig);
                        worstExcess = std::max (worstExcess, peakOf (out) / dbToGain (ce));
                        ++cases;
                    }
        check (worstExcess <= 1.0f, fmt ("%.0f Hz, oversampling Off: never above the ceiling (%ld cases, worst %.7f x ceiling)", fs, cases, worstExcess));
    }

    // With oversampling the final base-rate clip keeps the same guarantee, and reports how much it trims.
    for (int os = 1; os <= 3; ++os)
        for (double fs : kRates)
        {
            Clipper c;
            c.prepare (fs, kBlock);
            const auto signals = signalsFor (fs);
            float worstExcess = 0.0f, worstTrim = 0.0f;
            for (const auto& sig : signals)
                for (float so : softnesses)
                {
                    c.reset();
                    const auto out = run (c, params (-12.0f, -1.0f, so, os), sig);
                    worstExcess = std::max (worstExcess, peakOf (out) / dbToGain (-1.0f));
                    // trims are reported per process() call; take the largest over the run
                }
            // Trim measurement on the harshest signal: hard-clipped full-scale square.
            c.reset();
            std::vector<float> sq (signals[3]);
            std::vector<float> l (sq), r (sq);
            for (size_t pos = 0; pos < sq.size(); pos += kBlock)
            {
                const int n = (int) std::min ((size_t) kBlock, sq.size() - pos);
                float* ch[2] = { l.data() + pos, r.data() + pos };
                c.setParams (params (-12.0f, -1.0f, 0.0f, os));
                c.process (ch, 2, n);
                worstTrim = std::max (worstTrim, c.meters().safetyClipDb);
            }
            check (worstExcess <= 1.0f, fmt ("%.0f Hz, %dx: never above the ceiling after the final clip; downsampling overshoot trimmed on a square: %.2f dB",
                                              fs, 1 << os, worstTrim));
            check (worstTrim < 3.0f, fmt ("%.0f Hz, %dx: overshoot before the final clip stays under 3 dB (%.2f)", fs, 1 << os, worstTrim));
        }
}

//==============================================================================
double aliasEnergyFraction (int oversamplingLog2)
{
    // A 1.2 kHz tone heavily clipped; coherent over N samples so the true harmonics sit on bins.
    constexpr double fs = 48000.0;
    constexpr int N = 4096, bin = 101;   // f0 = 101 / 4096 x fs = 1184 Hz; harmonics m x 101 < 2048 for m <= 20
    const double f0 = fs * bin / N;
    Clipper c;
    c.prepare (fs, kBlock);
    const auto in = sine (fs, f0, 0.9, (size_t) (N * 4));
    const auto out = run (c, params (-30.0f, 0.0f, 0.0f, oversamplingLog2), in);

    // The window is the last N samples; it contains an exact number of periods wherever it starts.
    double total = 0.0;
    for (int i = 0; i < N; ++i)
        total += (double) out[(size_t) (3 * N + i)] * (double) out[(size_t) (3 * N + i)];
    total *= 1.0;   // energy; the DFT sums |X|^2 / N

    double harmonics = 0.0;
    for (int m = 1; m * bin < N / 2; ++m)
    {
        double re = 0.0, im = 0.0;
        for (int i = 0; i < N; ++i)
        {
            const double ph = 2.0 * kPi * (double) ((long long) m * bin * i % N) / N;
            const double x = (double) out[(size_t) (3 * N + i)];
            re += x * std::cos (ph);
            im -= x * std::sin (ph);
        }
        harmonics += 2.0 * (re * re + im * im) / N;   // one-sided: bin m x 301 and its mirror
    }
    return std::max (total - harmonics, 1e-20) / total;
}

void testAliasing()
{
    std::printf ("aliasing\n");
    const double off = aliasEnergyFraction (0);
    const double x4 = aliasEnergyFraction (2);
    const double x8 = aliasEnergyFraction (3);
    std::printf ("       energy outside the true harmonics: Off %.1f dB, 4x %.1f dB, 8x %.1f dB (re total)\n",
                 10 * std::log10 (off), 10 * std::log10 (x4), 10 * std::log10 (x8));
    check (10 * std::log10 (off / x4) > 8.0, "4x oversampling cuts the folded-back energy by more than 8 dB on a hard-clipped tone (limited by the final half-band filter's transition band)");
    check (10 * std::log10 (x4 / x8) > 0.0, "8x is no worse than 4x");
}

//==============================================================================
void testLatency()
{
    std::printf ("latency\n");
    for (double fs : kRates)
    {
        Clipper c;
        c.prepare (fs, kBlock);
        for (int os = 0; os <= 3; ++os)
        {
            int expected = 0;
            if (os > 0)
            {
                auto ref = Clipper::makeOversampler (os);
                ref->initProcessing ((size_t) kBlock);
                expected = (int) std::lround (ref->getLatencyInSamples());
            }
            c.reset();
            std::vector<float> silence (kBlock * 4, 0.0f);
            silence[100] = 0.01f;   // an impulse far below the knee: the path is linear
            Params p = params (0.0f, 0.0f, 0.0f, os);
            const auto out = run (c, p, silence);
            size_t peak = 0;
            for (size_t i = 0; i < out.size(); ++i)
                if (std::abs (out[i]) > std::abs (out[peak]))
                    peak = i;
            const bool reported = c.latencySamples() == expected && c.latencyForFactor (os) == expected;
            const bool measured = (int) peak - 100 == expected;
            check (reported && measured, fmt ("%.0f Hz, %s: reported %d, oversampler %d, impulse peak delayed %d samples",
                                              fs, os == 0 ? "Off" : fmt ("%dx", 1 << os).c_str(), c.latencySamples(), expected, (int) peak - 100));
        }
    }

    // Changing the factor on a running clipper updates the reported latency.
    Clipper c;
    c.prepare (48000.0, kBlock);
    std::vector<float> x (kBlock * 6, 0.1f);
    run (c, params (0.0f, 0.0f, 0.0f, 0), x);
    const int before = c.latencySamples();
    run (c, params (0.0f, 0.0f, 0.0f, 3), x);
    const int after = c.latencySamples();
    run (c, params (0.0f, 0.0f, 0.0f, 1), x);
    check (before == 0 && after == c.latencyForFactor (3) && c.latencySamples() == c.latencyForFactor (1),
           fmt ("a factor change on the fly updates the latency (0 -> %d -> %d)", after, c.latencySamples()));
}

//==============================================================================
double maxStep (const std::vector<float>& y, size_t from)
{
    double m = 0.0;
    for (size_t i = from + 1; i < y.size(); ++i)
        m = std::max (m, (double) std::abs (y[i] - y[i - 1]));
    return m;
}

/** Runs `before` for settleBlocks blocks, then `after`; returns the output of the whole run. */
std::vector<float> stepRun (Clipper& c, const Params& before, const Params& after, const std::vector<float>& in, size_t switchAt)
{
    std::vector<float> out (in);
    for (size_t pos = 0; pos < in.size(); pos += kBlock)
    {
        const int n = (int) std::min ((size_t) kBlock, in.size() - pos);
        std::vector<float> r (out.begin() + (long) pos, out.begin() + (long) pos + n);
        float* ch[2] = { out.data() + pos, r.data() };
        c.setParams (pos < switchAt ? before : after);
        c.process (ch, 2, n);
    }
    return out;
}

void testSmoothing()
{
    std::printf ("smoothing\n");
    constexpr double fs = 48000.0;
    const float a = 1.0f - std::exp (-1.0f / (0.010f * (float) fs));   // the clipper's one-pole coefficient
    const size_t total = 48000, at = 24064;

    // Input gain, Threshold: gain steps on a sine below the knee.
    {
        const double amp = 0.01, hz = 100.0;
        Clipper c;
        c.prepare (fs, kBlock);
        const auto in = sine (fs, hz, amp, total);
        Params p0 = params (0.0f, 0.0f, 0.0f, 0), p1 = p0;
        p1.inputGainDbL = -20.0f;
        const auto out = stepRun (c, p0, p1, in, at);
        const double g0 = 1.0, g1 = dbToGain (-20.0), slope = 2.0 * kPi * hz / fs * amp;
        const double bound = 1.05 * (slope + amp * a * std::abs (g1 - g0));
        const double step = maxStep (out, 100);
        const double unsmoothed = amp * std::abs (g1 - g0);
        check (step <= bound, fmt ("input gain 0 -> -20 dB: largest sample step %.2e <= derived bound %.2e (an unsmoothed step would be %.2e)", step, bound, unsmoothed));
    }
    // Threshold: u = x / T scales; output gain (C - T) changes 0 -> 12 dB.
    {
        const double amp = 0.01, hz = 100.0;
        Clipper c;
        c.prepare (fs, kBlock);
        const auto in = sine (fs, hz, amp, total);
        Params p0 = params (0.0f, 0.0f, 0.0f, 0), p1 = params (-12.0f, 0.0f, 0.0f, 0);
        const auto out = stepRun (c, p0, p1, in, at);
        const double g0 = 1.0, g1 = dbToGain (12.0), slope = 2.0 * kPi * hz / fs * amp * g1;
        const double bound = 1.05 * (slope + amp * a * std::abs (g1 - g0));
        const double step = maxStep (out, 100);
        check (step <= bound, fmt ("threshold 0 -> -12 dB: largest sample step %.2e <= derived bound %.2e (unsmoothed %.2e)", step, bound, amp * std::abs (g1 - g0)));
    }
    // Ceiling on a signal that is held at the ceiling (DC 0.8, 6 dB past the threshold).
    {
        Clipper c;
        c.prepare (fs, kBlock);
        const std::vector<float> in (total, 0.8f);
        Params p0 = params (-6.0f, 0.0f, 0.0f, 0), p1 = params (-6.0f, -20.0f, 0.0f, 0);
        const auto out = stepRun (c, p0, p1, in, at);
        const double bound = 1.05 * a * std::abs (1.0 - (double) dbToGain (-20.0f)) * 1.0;
        const double step = maxStep (out, 100);
        check (step <= bound, fmt ("ceiling 0 -> -20 dB: largest sample step %.2e <= derived bound %.2e (unsmoothed %.2e)", step, bound, 1.0 - (double) dbToGain (-20.0f)));
    }
    // Softness on a signal at the threshold: dy/dw is at most 1/4.
    {
        Clipper c;
        c.prepare (fs, kBlock);
        const std::vector<float> in (total, 0.5f);
        Params p0 = params (-6.0206f, 0.0f, 0.0f, 0), p1 = p0;   // u = 0.5 x 2 = 1.0
        p1.softnessPercent = 100.0f;
        const auto out = stepRun (c, p0, p1, in, at);
        const double bound = 1.05 * a * 1.0 / 4.0 * 1.0;
        const double step = maxStep (out, 100);
        check (step <= bound, fmt ("softness 0 -> 100 %%: largest sample step %.2e <= derived bound %.2e (unsmoothed %.2e)", step, bound, 0.25));
    }
    // Changing the oversampling factor under signal: faded, no click.
    for (int target = 1; target <= 3; ++target)
    {
        const double amp = 0.5, hz = 440.0;
        Clipper c;
        c.prepare (fs, kBlock);
        const auto in = sine (fs, hz, amp, total);
        const auto out = stepRun (c, params (0.0f, 0.0f, 0.0f, 0), params (0.0f, 0.0f, 0.0f, target), in, at);
        const double bound = 1.1 * (2.0 * kPi * hz / fs * amp + amp / 256.0) + 1e-3;
        const double step = maxStep (out, 100);
        check (step <= bound, fmt ("Off -> %dx under signal: largest sample step %.2e <= bound %.2e", 1 << target, step, bound));
    }
}

//==============================================================================
void testRealtime()
{
    std::printf ("realtime\n");
    Clipper c;
    c.prepare (48000.0, kBlock);
    const auto sig = sine (48000.0, 1000.0, 1.0, kBlock);
    std::vector<float> l (sig), r (sig);
    float* ch[2] = { l.data(), r.data() };

    // Warm up every path once, then count allocations over a sweep of all factors and parameters.
    for (int os = 0; os <= 3; ++os)
    {
        c.setParams (params (-6.0f, -1.0f, 30.0f, os));
        for (int k = 0; k < 4; ++k)
            c.process (ch, 2, kBlock);
    }
    gAllocations.store (0);
    gCountAllocations.store (true);
    for (int round = 0; round < 40; ++round)
    {
        c.setParams (params (-(float) (round % 30), -(float) (round % 7), (float) (round * 3 % 100), round % 4));
        c.process (ch, round % 2 == 0 ? 2 : 1, kBlock);
        c.process (ch, 2, kBlock / 3);
    }
    gCountAllocations.store (false);
    check (gAllocations.load() == 0, fmt ("setParams and process (all factors, factor changes, mono and stereo) made %ld allocations through operator new", gAllocations.load()));
}

//==============================================================================
void testLink()
{
    std::printf ("link\n");
    LinkedControls link;
    link.setRange (-30.0f, 0.0f);
    link.reset (-6.0f, -1.0f, true);
    auto r = link.update (-9.0f, -1.0f, true);
    check (r.writeCeiling && std::abs (r.ceilingDb + 4.0f) < 1e-5f && std::abs (r.thresholdDb + 9.0f) < 1e-5f,
           "Link: Threshold -3 dB moves the Ceiling -3 dB");
    link.reset (-6.0f, -1.0f, false);
    r = link.update (-9.0f, -1.0f, false);
    check (! r.writeCeiling && ! r.writeThreshold && std::abs (r.ceilingDb + 1.0f) < 1e-5f, "Link off: the controls are independent");
}
} // namespace

int main (int argc, char** argv)
{
    const std::string only = argc > 1 ? argv[1] : "";
    const auto wanted = [&] (const char* name) { return only.empty() || only == name; };

    if (wanted ("curve")) testCurve();
    if (wanted ("gain")) testGain();
    if (wanted ("ceiling")) testCeiling();
    if (wanted ("aliasing")) testAliasing();
    if (wanted ("latency")) testLatency();
    if (wanted ("smoothing")) testSmoothing();
    if (wanted ("realtime")) testRealtime();
    if (wanted ("link")) testLink();

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
