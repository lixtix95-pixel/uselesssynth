// ROBIN — питч-шифтер (эффект 15/16 из Terrain).
//
// Реализация: фазовый вокодер со взвешенным перекрытием (STFT + перенос
// спектра по индексам бинов с накоплением фазы), не FFT-free гранулы.
//
// Почему не две «головы» линии задержки и не наивный грануляр — оба варианта
// замерены и отброшены:
//   * две головы (окно привязано к задержке) — головы читают историю со
//     сдвигом D/2 входного времени => постоянный фазовый сдвиг ω·D/2 на тоне,
//     то есть гребёнка с провалами каждые sr/D Гц. Замер: тон 440 Гц при окне
//     70 мс падал с 0.35 до 0.08 (−13 дБ), уровень «плавал»;
//   * две независимые гранулы (окно по возрасту зерна) — зёрна неизбежно
//     оказываются в разных точках входного времени (Δa·(1−r)), при +12
//     это 1680 отсчётов и провалы каждые 28 Гц. Замер: амплитуда 880 Гц 0.083
//     вместо 0.35, RMS 0.18 — звук «худой».
// Фазовый вокодер не складывает копии сигнала, а пересобирает его из спектра,
// поэтому гребёнки нет вовсе. Цена — латентность N отсчётов (21.3 мс при 48 кГц)
// и «флангерность» на широкополосном шуме (свойство метода, не дефект).
//
// Латентность постоянна (N) и не зависит от mix: хост компенсирует её всегда,
// поэтому включение эффекта не дёргает транспорт.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "../core/dsp.h"
#include "../core/fft.h"

namespace orogen {

class RobinFx
{
public:
    // N = 1024 (21.3 мс при 48 кГц), перекрытие 75 %.
    static constexpr int kFft = 1024;
    static constexpr int kHop = kFft / 4;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        const int n = kFft;
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& S = st[(size_t) ch];
            S.inFifo.assign ((size_t) n, 0.0f);
            S.outFifo.assign ((size_t) n, 0.0f);
            S.acc.assign ((size_t) n, 0.0f);
            S.nrm.assign ((size_t) n, 0.0f);
            S.spec.assign ((size_t) n, {});
            S.lastPhase.assign ((size_t) (n / 2 + 1), 0.0f);
            S.sumPhase.assign ((size_t) (n / 2 + 1), 0.0f);
            S.trueFreq.assign ((size_t) (n / 2 + 1), 0.0f);
            S.magn.assign ((size_t) (n / 2 + 1), 0.0f);
            S.phase.assign ((size_t) (n / 2 + 1), 0.0f);
            S.fresh = true;
            S.inPos = n - kHop;
            S.outPos = 0;
        }
        dryL.assign ((size_t) kFft, 0.0f);
        dryR.assign ((size_t) kFft, 0.0f);
        dryIdx = 0;
        window.assign ((size_t) n, 0.0f);
        winLen = 0;
        setSize (sizeN);          // строит окно нужной длины
        updateRate();
    }

    void setSemitones (float s) { semisN = clamp (s, -24.0f, 24.0f); updateRate(); }
    // size: длина окна анализа 0 → 512 (10.7 мс), 1 → 1024 (21.3 мс). Кадр FFT
    // всегда 1024 (латентность постоянна), окно просто короче — как «мелкое
    // зерно»: частотное разрешение хуже, звук шире/«песочнее».
    void setSize (float s)
    {
        sizeN = clamp (s, 0.0f, 1.0f);
        const int len = std::max (kHop * 2, std::min (kFft, (int) (512.0f + sizeN * 512.0f)));
        if (len != winLen) { winLen = len; rebuildWindow(); }
    }
    void setSpread (float s)    { spreadN = clamp (s, 0.0f, 1.0f); updateRate(); }
    void setMix (float m)
    {
        const float prev = mixN;
        mixN = clamp (m, 0.0f, 1.0f);
        if (mixN > 1.0e-4f && prev <= 1.0e-4f) dirty = true;   // включили — буферы чистые
    }

    float semitones() const { return semisN; }
    int   windowSamples() const { return winLen; }
    int   latency() const { return kFft; }

    inline void process (float& l, float& r) noexcept
    {
        if (dryL.empty()) return;

        // Сухой путь задержан на ту же величину, что и мокрый: иначе mix
        // складывает сигнал с самим собой со сдвигом 21 мс — гребёнка.
        const float dryLv = dryL[(size_t) dryIdx];
        const float dryRv = dryR[(size_t) dryIdx];
        dryL[(size_t) dryIdx] = l;
        dryR[(size_t) dryIdx] = r;
        if (++dryIdx >= kFft) dryIdx = 0;

        if (mixN <= 1.0e-4f)
        {
            dirty = true;                  // FFT не считаем, но задержку держим
            l = dryLv; r = dryRv;
            return;
        }
        if (dirty) { clearBuffers(); dirty = false; }

        const float wetL = runChannel (st[0], l, ratioL);
        const float wetR = runChannel (st[1], r, ratioR);

        l = dryLv * (1.0f - mixN) + wetL * mixN;
        r = dryRv * (1.0f - mixN) + wetR * mixN;
    }

private:
    struct Chan
    {
        std::vector<float> inFifo, outFifo, acc, nrm, lastPhase, sumPhase, trueFreq, magn, phase;
        std::vector<std::complex<float>> spec;
        int inPos = 0, outPos = 0;
        bool fresh = true;
    };

    static constexpr float kBinScale = (float) kFft * 0.5f;   // 2|X|/N → амплитуда; обратно ×N/2

    inline float runChannel (Chan& S, float x, float ratio) noexcept
    {
        S.inFifo[(size_t) S.inPos++] = x;
        const float y = S.outFifo[(size_t) S.outPos++];

        if (S.inPos >= kFft)
        {
            S.inPos = kFft - kHop;
            processFrame (S, ratio);
            S.outPos = 0;
        }
        return y;
    }

    void processFrame (Chan& S, float ratio) noexcept
    {
        // 1. окно
        for (int i = 0; i < kFft; ++i)
            S.spec[(size_t) i] = std::complex<float> (S.inFifo[(size_t) i] * window[(size_t) i], 0.0f);

        // 2. прямой FFT
        Fft::forward (S.spec);

        // 3. магнитуда и мгновенная частота
        constexpr float kExpct = (float) (kTwoPi * kHop / kFft);
        const int half = kFft / 2;
        for (int k = 0; k <= half; ++k)
        {
            const auto z = S.spec[(size_t) k];
            const float re = z.real(), im = z.imag();
            const float mag = 2.0f * std::sqrt (re * re + im * im) / (float) kFft;
            float ph = std::atan2 (im, re);
            S.phase[(size_t) k] = ph;
            float dp = ph - S.lastPhase[(size_t) k];
            S.lastPhase[(size_t) k] = ph;
            dp -= (float) k * kExpct;
            dp -= kTwoPi * std::floor (dp / kTwoPi + 0.5f);        // princarg
            S.trueFreq[(size_t) k] = ((float) k + dp / kExpct) * kExpct;
            S.magn[(size_t) k] = mag;
        }

        // 4. перенос спектра: синтез-бин k берёт магнитуду из анализа-бина k/ratio.
        // Фаза накапливается строго с шагом бина (2π·k·hop/N): только так
        // перекрывающиеся кадры складываются когерентно. Если вместо этого
        // копить «истинную» частоту источника (как в наивном варианте),
        // кадры расходятся по фазе: замер дал 815 Гц вместо 880 и амплитуду
        // 0.26 вместо 0.50.
        for (int k = 0; k <= half; ++k)
        {
            const float src = (float) k / ratio;
            int i0 = (int) src;
            if (i0 > half) i0 = half;
            const int i1 = std::min (half, i0 + 1);
            const float fr = std::min (1.0f, src - (float) i0);

            float mag = S.magn[(size_t) i0] + fr * (S.magn[(size_t) i1] - S.magn[(size_t) i0]);
            float frq = S.trueFreq[(size_t) i0] + fr * (S.trueFreq[(size_t) i1] - S.trueFreq[(size_t) i0]);

            if (ratio < 1.0f)
            {
                // Сжатие спектра: мощность источника надо разложить по выходным
                // бинам без потерь и без двойного счёта. Каждый исходный бин
                // попадает ровно в один выходной — берём коробку шириной в один
                // бин вокруг k/ratio и суммируем мощность.
                const float lo = src - 0.5f, hi = src + 0.5f;
                float e = 0.0f;
                for (int i = std::max (0, (int) std::ceil (lo)); i <= std::min (half, (int) std::floor (hi)); ++i)
                    e += S.magn[(size_t) i] * S.magn[(size_t) i];
                mag = std::sqrt (e);
            }

            // Фаза: истинная частота источника × коэффициент переноса. Для бина,
            // попавшего точно в партиал, это ровно k·expct, то есть кадры
            // складываются когерентно; остаётся ошибка округления до бина — она
            // и даёт лёгкую «фазовость», свойственную методу.
            // (Вариант с фазовым связыванием бинов по пикам пробовался и дал
            // хуже: +12 терял уровень до 0.15 вместо 0.57 — откатан.)
            float& acc = S.sumPhase[(size_t) k];
            acc += ratio * frq;
            acc -= kTwoPi * std::floor (acc / kTwoPi);

            const float a = mag * kBinScale;
            S.spec[(size_t) k] = std::complex<float> (a * std::cos (acc), a * std::sin (acc));
            if (k > 0 && k < half)
                S.spec[(size_t) (kFft - k)] = std::conj (S.spec[(size_t) k]);
        }
        S.spec[0] = { S.spec[0].real(), 0.0f };
        S.spec[(size_t) half] = { S.spec[(size_t) half].real(), 0.0f };

        // 5. обратный FFT
        Fft::inverse (S.spec);

        // 6. OLA с окном и точной нормировкой на сумму квадратов окон
        for (int i = 0; i < kFft; ++i)
        {
            const float w = window[(size_t) i];
            const float v = S.spec[(size_t) i].real();
            S.acc[(size_t) i] += w * v;
            S.nrm[(size_t) i] += w * w;
        }
        for (int i = 0; i < kFft; ++i)
        {
            // Нормировка на сумму квадратов окон. Делить можно только там, где
            // окна реально перекрываются: при коротком окне и на старте nrm
            // бывает 0.02, и деление давало выбросы до 4.75 при входе 0.35.
            const float d = S.nrm[(size_t) i];
            const float y = d > 0.25f ? S.acc[(size_t) i] / d : 0.0f;
            S.outFifo[(size_t) i] = clamp (y, -4.0f, 4.0f);
        }

        // сдвиг аккумуляторов на шаг
        for (int i = 0; i + kHop < kFft; ++i)
        {
            S.acc[(size_t) i] = S.acc[(size_t) (i + kHop)];
            S.nrm[(size_t) i] = S.nrm[(size_t) (i + kHop)];
        }
        for (int i = kFft - kHop; i < kFft; ++i) { S.acc[(size_t) i] = 0.0f; S.nrm[(size_t) i] = 0.0f; }

        // вход тоже сдвигаем: иначе в кадр попадают старые отсчёты и вместо
        // сигнала получается его «огрызок» (замер до правки: амплитуда 0.018
        // вместо 0.5, корреляция с входом 0.04)
        for (int i = 0; i + kHop < kFft; ++i) S.inFifo[(size_t) i] = S.inFifo[(size_t) (i + kHop)];
    }

    void rebuildWindow()
    {
        if (window.size() < (size_t) kFft) return;   // setFx() может прийти до prepare()
        std::fill (window.begin(), window.end(), 0.0f);
        const int len = winLen;
        // окно занимает последние len отсчётов кадра: новейший сэмпл — на краю
        for (int i = 0; i < len; ++i)
            window[(size_t) (kFft - len + i)] = 0.5f - 0.5f * std::cos (kTwoPi * (double) i / (double) len);
    }

    void clearBuffers()
    {
        for (auto& S : st)
        {
            std::fill (S.inFifo.begin(), S.inFifo.end(), 0.0f);
            std::fill (S.outFifo.begin(), S.outFifo.end(), 0.0f);
            std::fill (S.acc.begin(), S.acc.end(), 0.0f);
            std::fill (S.nrm.begin(), S.nrm.end(), 0.0f);
            std::fill (S.lastPhase.begin(), S.lastPhase.end(), 0.0f);
            std::fill (S.sumPhase.begin(), S.sumPhase.end(), 0.0f);
            S.inPos = kFft - kHop;
            S.outPos = 0;
        }
    }

    void updateRate()
    {
        ratioL = std::pow (2.0f, semisN / 12.0f);
        ratioR = ratioL * std::pow (2.0f, (spreadN * 0.15f) / 12.0f);
    }

    std::vector<float> window, dryL, dryR;
    int dryIdx = 0, winLen = 0;
    Chan st[2];
    float ratioL = 1.0f, ratioR = 1.0f;
    float semisN = 0.0f, sizeN = 0.4f, spreadN = 0.0f, mixN = 0.0f;
    bool dirty = true;
    double sr = 48000.0;
};

} // namespace orogen
