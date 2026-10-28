// Офлайн-анализ one-shot'ов для движка Resynth.
//
// Идея ресинтеза: не играть запись, а «прочитать» её спектр и заново
// синтезировать синусами. Тогда время и высота становятся независимыми:
// растяжение в 4 раза не меняет тембр, а питч-шифт не меняет длительность.
//
// Анализ: короткие кадры по 10 мс, окно Ханна 2048, до 32 пиков на кадр
// с параболической интерполяцией частоты. Данные — это уже не 6 МБ сэмплов,
// а 8 источников × 200 кадров × 32 пика × 8 байт ≈ 400 КБ.
#pragma once

#include "../core/dsp.h"
#include "../core/fft.h"
#include "../content/sample_bank.h"

#include <string>
#include <vector>

namespace orogen {

inline constexpr int kRsPartials = 32;
inline constexpr double kRsHop = 0.010;      // 10 мс

struct PartialFrame
{
    float freq[kRsPartials] {};
    float amp [kRsPartials] {};
    int   count = 0;
};

struct ResynthSource
{
    std::string name;
    int rootNote = 45;
    std::vector<PartialFrame> frames;
};

class ResynthBank
{
public:
    std::vector<ResynthSource> sources;
    double sampleRate = 48000.0;

    // Анализ одного источника. Пики ищутся по магнитуде, частота уточняется
    // параболой по трём бинам — иначе партиал «дрожит» на ±bin/2 и звук
    // становится расстроенным при растяжении.
    static ResynthSource analyze (const OneShot& src, double sr)
    {
        ResynthSource out;
        out.name = src.name;
        out.rootNote = src.rootNote;

        const std::vector<float>& x = src.levels[0];
        const int N = 2048;
        const int hop = (int) (sr * kRsHop);
        const int nFrames = (int) (x.size() / (size_t) hop);

        // окно Ханна
        std::vector<float> win ((size_t) N);
        for (int i = 0; i < N; ++i)
            win[(size_t) i] = 0.5f - 0.5f * std::cos (float (kTwoPi) * (float) i / (float) (N - 1));

        std::vector<std::complex<float>> spec ((size_t) N);
        std::vector<float> mag ((size_t) N / 2);

        out.frames.reserve ((size_t) nFrames);
        for (int f = 0; f < nFrames; ++f)
        {
            const int start = f * hop - N / 2;          // кадр центрирован
            for (int i = 0; i < N; ++i)
            {
                const int idx = clamp (start + i, 0, (int) x.size() - 1);
                spec[(size_t) i] = { x[(size_t) idx] * win[(size_t) i], 0.0f };
            }
            Fft::forward (spec);
            for (int b = 0; b < N / 2; ++b) mag[(size_t) b] = std::abs (spec[(size_t) b]);

            // глобальный максимум кадра — порог от него
            float peak = 1.0e-9f;
            for (int b = 2; b < N / 2; ++b) peak = std::max (peak, mag[(size_t) b]);
            const float floorMag = peak * 0.0056f;      // -45 дБ

            PartialFrame pf;
            // локальные максимумы
            struct Cand { float amp; float freq; };
            std::vector<Cand> cands;
            const double binHz = sr / N;
            for (int b = 2; b < N / 2 - 1; ++b)
            {
                const float m = mag[(size_t) b];
                if (m < floorMag) continue;
                if (m <= mag[(size_t) b - 1] || m < mag[(size_t) b + 1]) continue;

                // параболическая интерполяция по логарифму магнитуды
                const double a = std::log (std::max (1e-12f, mag[(size_t) b - 1]));
                const double bb = std::log (std::max (1e-12f, m));
                const double c = std::log (std::max (1e-12f, mag[(size_t) b + 1]));
                const double denom = (a - 2.0 * bb + c);
                double delta = (std::fabs (denom) > 1.0e-12)
                                 ? 0.5 * (a - c) / denom : 0.0;
                delta = clamp (delta, -0.5, 0.5);

                const double freq = ((double) b + delta) * binHz;
                if (freq > 0.45 * sr) continue;

                // компенсация окна: сумма весов Ханна = N/2
                const float amp = (float) (m * 4.0 / (double) N);
                cands.push_back ({ amp, (float) freq });
            }

            // ---- Пик-трекинг, а не «сортировка по частоте».
            //
            // Если просто брать 32 самых громких пика и сортировать их по
            // частоте, состав партиалов в соседних кадрах различается, и
            // индекс j начинает соответствовать ДРУГОМУ партиалу: частота
            // прыгает на сотни герц, синтез «чирикает», энергия размазывается
            // по всему спектру (измерено -15 дБ «алиасинга» вместо -70).
            //
            // Здесь траектория каждого партиала продолжается: слот j занимает
            // пик, ближайший по частоте (±6 %) к предыдущему кадру. Умерший
            // партиал остаётся в слоте с amp = 0, и сглаживание в движке
            // гасит его плавно.
            std::sort (cands.begin(), cands.end(),
                       [] (const Cand& a1, const Cand& b1) { return a1.amp > b1.amp; });
            if ((int) cands.size() > kRsPartials) cands.resize ((size_t) kRsPartials);

            for (int j = 0; j < kRsPartials; ++j) { pf.freq[j] = 0.0f; pf.amp[j] = 0.0f; }
            std::vector<char> used (cands.size(), 0);

            if (out.frames.empty())
            {
                // первый кадр: слоты по возрастанию частоты
                std::sort (cands.begin(), cands.end(),
                           [] (const Cand& a1, const Cand& b1) { return a1.freq < b1.freq; });
                for (int j = 0; j < (int) cands.size(); ++j)
                {
                    pf.freq[j] = cands[(size_t) j].freq;
                    pf.amp[j]  = cands[(size_t) j].amp;
                }
            }
            else
            {
                const PartialFrame& prev = out.frames.back();
                // продолжаем существующие траектории
                for (int j = 0; j < kRsPartials; ++j)
                {
                    if (prev.amp[j] <= 0.0f) continue;
                    int best = -1; float bestRatio = 1.06f;
                    for (int c = 0; c < (int) cands.size(); ++c)
                    {
                        if (used[(size_t) c]) continue;
                        const float r = std::max (cands[(size_t) c].freq, prev.freq[j])
                                      / std::max (1.0f, std::min (cands[(size_t) c].freq, prev.freq[j]));
                        if (r < bestRatio) { bestRatio = r; best = c; }
                    }
                    if (best >= 0)
                    {
                        used[(size_t) best] = 1;
                        pf.freq[j] = cands[(size_t) best].freq;
                        pf.amp[j]  = cands[(size_t) best].amp;
                    }
                    else
                    {
                        pf.freq[j] = prev.freq[j];      // партиал ушёл: держим частоту, amp = 0
                        pf.amp[j]  = 0.0f;
                    }
                }
                // новые партиалы — в свободные слоты
                int slot = 0;
                for (int c = 0; c < (int) cands.size(); ++c)
                {
                    if (used[(size_t) c]) continue;
                    while (slot < kRsPartials && pf.amp[slot] > 0.0f) ++slot;
                    if (slot >= kRsPartials) break;
                    pf.freq[slot] = cands[(size_t) c].freq;
                    pf.amp[slot]  = cands[(size_t) c].amp;
                }
            }

            // count больше не нужен: слоты фиксированы, «мёртвые» имеют amp = 0
            pf.count = kRsPartials;
            out.frames.push_back (pf);
        }
        return out;
    }

    void build (const SampleBank& bank, double sr = kSampleRate)
    {
        sampleRate = sr;
        sources.clear();
        for (const auto& s : bank.shots)
            sources.push_back (analyze (s, sr));
    }

    size_t bytes() const
    {
        size_t b = 0;
        for (const auto& s : sources) b += s.frames.size() * sizeof (PartialFrame);
        return b;
    }
};

} // namespace orogen
