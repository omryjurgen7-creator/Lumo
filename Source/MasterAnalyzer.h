#pragma once
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <memory>

namespace lumo
{
inline float  linToDb (double lin) noexcept { return (float) (20.0 * std::log10 (std::max (lin, 1.0e-6))); }
inline float  powToDb (double p)   noexcept { return (float) (10.0 * std::log10 (std::max (p,   1.0e-12))); }
inline double lufsOf  (double e)   noexcept { return -0.691 + 10.0 * std::log10 (std::max (e, 1.0e-12)); }

struct MeterResults
{
    std::atomic<float> peakL { -120.f }, peakR { -120.f };           // sample peak, held with 20 dB/s decay
    std::atomic<float> truePeakL { -120.f }, truePeakR { -120.f };   // 4x oversampled, held with decay
    std::atomic<float> maxTruePeak { -120.f };                       // since reset
    std::atomic<float> rmsL { -120.f }, rmsR { -120.f };             // 300 ms, unweighted
    std::atomic<float> momentary { -120.f }, shortTerm { -120.f }, integrated { -120.f };
    std::atomic<float> lra { 0.f }, crest { 0.f }, plr { 0.f };
    std::atomic<float> correlation { 0.f };
    std::atomic<float> balance { 0.f };                              // L - R in dB (RMS)
};

struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    inline double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = 0; }
};

// 0.1 LU resolution histogram from -70 to +40 LUFS, used for BS.1770 gating and EBU R128 LRA.
struct LoudnessHistogram
{
    static constexpr int N = 1100;
    std::array<double, N>   energy {};
    std::array<uint32_t, N> count  {};

    void clear() { energy.fill (0.0); count.fill (0); }

    void add (double e)
    {
        const double l = lufsOf (e);
        if (l < -70.0) return;
        const int b = juce::jlimit (0, N - 1, (int) std::floor ((l + 70.0) * 10.0));
        energy[(size_t) b] += e;
        count[(size_t) b]++;
    }

    double integrated() const
    {
        double sum = 0; uint64_t cnt = 0;
        for (int i = 0; i < N; ++i) { sum += energy[(size_t) i]; cnt += count[(size_t) i]; }
        if (cnt == 0) return -120.0;

        const double gate = lufsOf (sum / (double) cnt) - 10.0;
        const int start = std::max (0, (int) std::ceil ((gate + 70.0) * 10.0));

        double s2 = 0; uint64_t c2 = 0;
        for (int i = start; i < N; ++i) { s2 += energy[(size_t) i]; c2 += count[(size_t) i]; }
        return c2 == 0 ? -120.0 : lufsOf (s2 / (double) c2);
    }

    double lra() const
    {
        double sum = 0; uint64_t cnt = 0;
        for (int i = 0; i < N; ++i) { sum += energy[(size_t) i]; cnt += count[(size_t) i]; }
        if (cnt == 0) return 0.0;

        const double gate = lufsOf (sum / (double) cnt) - 20.0;
        const int start = std::max (0, (int) std::ceil ((gate + 70.0) * 10.0));

        uint64_t total = 0;
        for (int i = start; i < N; ++i) total += count[(size_t) i];
        if (total == 0) return 0.0;

        auto percentile = [&] (double p)
        {
            const uint64_t target = std::max<uint64_t> (1, (uint64_t) std::ceil (p * (double) total));
            uint64_t cum = 0;
            for (int i = start; i < N; ++i)
            {
                cum += count[(size_t) i];
                if (cum >= target) return (double) i / 10.0 - 70.0;
            }
            return (double) (N - 1) / 10.0 - 70.0;
        };
        return std::max (0.0, percentile (0.95) - percentile (0.10));
    }
};

class MasterAnalyzer
{
public:
    MeterResults results;

    void requestReset() { resetFlag.store (true); }

    void prepare (double sampleRate, int maxBlock)
    {
        fs = sampleRate;
        maxBlockSize = std::max (1, maxBlock);

        oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
            2, 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, false);
        oversampler->initProcessing ((size_t) maxBlockSize);

        subBlockLen   = std::max (1, (int) std::round (fs * 0.1));
        rmsAlpha      = 1.0 - std::exp (-1.0 / (fs * 0.3));
        decayPerSample = std::pow (10.0, -20.0 / (20.0 * fs));   // 20 dB/s peak fall-back

        makeKWeighting();
        fullReset();
    }

    void process (const float* l, const float* r, int n)
    {
        if (! oversampler) return;
        if (resetFlag.exchange (false)) fullReset();

        while (n > 0)
        {
            const int c = std::min (n, maxBlockSize);
            processChunk (l, r, c);
            l += c; r += c; n -= c;
        }
    }

private:
    void makeKWeighting()
    {
        // ITU-R BS.1770-4 K-weighting, computed for any sample rate
        const double pi = juce::MathConstants<double>::pi;
        {   // stage 1: high shelf (+4 dB @ ~1.7 kHz)
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (pi * f0 / fs);
            const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            for (auto& f : shelf)
            {
                f.b0 = (Vh + Vb * K / Q + K * K) / a0;
                f.b1 = 2.0 * (K * K - Vh) / a0;
                f.b2 = (Vh - Vb * K / Q + K * K) / a0;
                f.a1 = 2.0 * (K * K - 1.0) / a0;
                f.a2 = (1.0 - K / Q + K * K) / a0;
            }
        }
        {   // stage 2: RLB high-pass (~38 Hz)
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (pi * f0 / fs);
            const double a0 = 1.0 + K / Q + K * K;
            for (auto& f : hp)
            {
                f.b0 = 1.0; f.b1 = -2.0; f.b2 = 1.0;
                f.a1 = 2.0 * (K * K - 1.0) / a0;
                f.a2 = (1.0 - K / Q + K * K) / a0;
            }
        }
    }

    void fullReset()
    {
        for (auto& f : shelf) f.reset();
        for (auto& f : hp)    f.reset();
        if (oversampler) oversampler->reset();

        envL = envR = cLL = cRR = cLR = 0;
        sumKL = sumKR = 0; subCount = 0; subPeak = 0;
        ring.fill (0.0); peakRing.fill (0.0); ringIdx = 0; filled = 0;
        holdL = holdR = tpHoldL = tpHoldR = 0; maxTP = 0;
        intHist.clear(); lraHist.clear();

        results.peakL = results.peakR = results.truePeakL = results.truePeakR = -120.f;
        results.maxTruePeak = -120.f;
        results.momentary = results.shortTerm = results.integrated = -120.f;
        results.lra = 0.f; results.crest = 0.f; results.plr = 0.f;
    }

    void processChunk (const float* l, const float* r, int n)
    {
        // sample peak + true peak (4x)
        float bpL = 0, bpR = 0;
        for (int i = 0; i < n; ++i)
        {
            bpL = std::max (bpL, std::abs (l[i]));
            bpR = std::max (bpR, std::abs (r[i]));
        }

        const float* ptrs[2] = { l, r };
        juce::dsp::AudioBlock<const float> in (ptrs, 2, (size_t) n);
        auto up = oversampler->processSamplesUp (in);
        float tL = bpL, tR = bpR;
        const float* u0 = up.getChannelPointer (0);
        const float* u1 = up.getChannelPointer (1);
        for (size_t i = 0; i < up.getNumSamples(); ++i)
        {
            tL = std::max (tL, std::abs (u0[i]));
            tR = std::max (tR, std::abs (u1[i]));
        }

        const double dec = std::pow (decayPerSample, (double) n);
        holdL   = std::max ((double) bpL, holdL * dec);
        holdR   = std::max ((double) bpR, holdR * dec);
        tpHoldL = std::max ((double) tL,  tpHoldL * dec);
        tpHoldR = std::max ((double) tR,  tpHoldR * dec);
        maxTP   = std::max (maxTP, (double) std::max (tL, tR));
        subPeak = std::max (subPeak, (double) std::max (tL, tR));

        // per-sample: RMS, correlation, K-weighted energy
        for (int i = 0; i < n; ++i)
        {
            const double xl = l[i], xr = r[i];
            envL += rmsAlpha * (xl * xl - envL);
            envR += rmsAlpha * (xr * xr - envR);
            cLL  += rmsAlpha * (xl * xl - cLL);
            cRR  += rmsAlpha * (xr * xr - cRR);
            cLR  += rmsAlpha * (xl * xr - cLR);

            const double kl = hp[0].process (shelf[0].process (xl));
            const double kr = hp[1].process (shelf[1].process (xr));
            sumKL += kl * kl;
            sumKR += kr * kr;

            if (++subCount >= subBlockLen) { endSubBlock(); subCount = 0; }
        }

        results.peakL = linToDb (holdL);   results.peakR = linToDb (holdR);
        results.truePeakL = linToDb (tpHoldL); results.truePeakR = linToDb (tpHoldR);
        results.maxTruePeak = linToDb (maxTP);
        results.rmsL = powToDb (envL);     results.rmsR = powToDb (envR);
        results.balance = results.rmsL.load() - results.rmsR.load();
        results.correlation = (float) (cLR / std::sqrt (cLL * cRR + 1.0e-20));
    }

    void endSubBlock()
    {
        const double ms = (sumKL + sumKR) / (double) subBlockLen;
        sumKL = sumKR = 0;

        ring[(size_t) ringIdx] = ms;
        peakRing[(size_t) ringIdx] = subPeak;
        subPeak = 0;
        ringIdx = (ringIdx + 1) % ringSize;
        filled = std::min (filled + 1, ringSize);

        if (filled >= 4)   // momentary: 400 ms, updated every 100 ms (75% overlap)
        {
            double e = 0;
            for (int k = 0; k < 4; ++k) e += ring[(size_t) ((ringIdx - 1 - k + ringSize) % ringSize)];
            e /= 4.0;
            results.momentary = (float) lufsOf (e);
            intHist.add (e);
        }
        if (filled >= ringSize)   // short-term: 3 s
        {
            double e = 0;
            for (double v : ring) e += v;
            e /= (double) ringSize;
            results.shortTerm = (float) lufsOf (e);
            lraHist.add (e);
        }

        const double integ = intHist.integrated();
        results.integrated = (float) integ;
        results.lra = (float) lraHist.lra();

        double pk = 0;
        for (int i = 0; i < filled; ++i) pk = std::max (pk, peakRing[(size_t) i]);
        results.crest = linToDb (pk) - powToDb (0.5 * (envL + envR));
        results.plr = integ > -70.0 ? (float) ((double) linToDb (maxTP) - integ) : 0.f;
    }

    static constexpr int ringSize = 30;

    double fs = 44100.0;
    int maxBlockSize = 512, subBlockLen = 4410, subCount = 0, ringIdx = 0, filled = 0;
    double rmsAlpha = 0.0, decayPerSample = 1.0;
    double envL = 0, envR = 0, cLL = 0, cRR = 0, cLR = 0;
    double sumKL = 0, sumKR = 0, subPeak = 0;
    double holdL = 0, holdR = 0, tpHoldL = 0, tpHoldR = 0, maxTP = 0;
    std::array<double, ringSize> ring {}, peakRing {};
    Biquad shelf[2], hp[2];
    LoudnessHistogram intHist, lraHist;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    std::atomic<bool> resetFlag { false };
};
} // namespace lumo
