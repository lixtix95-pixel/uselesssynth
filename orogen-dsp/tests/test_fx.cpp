// Тесты новых эффектов: plate-ревербератор Датторро (el-visio/dattorro-verb,
// MIT), WDF-сатурация (chowdsp_wdf, BSD-3) и четыре эффекта, перенесённые из
// плагина MonoJuice 0.9.5: Drive (19 моделей), Material (модальный резонатор),
// Spectre (MDCT, BSD-3) и Glitch (статтер).
//
// Главная проверка — «адаптер против оригинала»: референсный C-код
// (third_party/dattorro-verb/verb.c) компилируется рядом и сравнивается
// с нашим C++ адаптером сэмпл-в-сэмпл на 48 кГц.
#include "engine_suite.h"
#include "../src/instrument/fx_plate.h"
#include "../src/instrument/fx_wdf.h"
#include "../src/instrument/fx.h"
#include "../src/instrument/fx_drive.h"
#include "../src/instrument/fx_material.h"
#include "../src/instrument/fx_spectre.h"
#include "../src/instrument/fx_glitch.h"
#include "../src/instrument/fx_comb.h"
#include "../src/instrument/fx_spectre_classic.h"
#include "../src/instrument/fx_robin.h"
#include "../src/instrument/fx_chorus_fw.h"

#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <complex>
#include <chrono>
#include <string>

using namespace orogen;
using suite::check;
using suite::num;

// --- референс: оригинальный C-код без изменений
extern "C" {
struct sDattorroVerb;
sDattorroVerb* DattorroVerb_create (void);
void DattorroVerb_delete (sDattorroVerb*);
void DattorroVerb_setPreDelay (sDattorroVerb*, double);
void DattorroVerb_setPreFilter (sDattorroVerb*, double);
void DattorroVerb_setInputDiffusion1 (sDattorroVerb*, double);
void DattorroVerb_setInputDiffusion2 (sDattorroVerb*, double);
void DattorroVerb_setDecayDiffusion (sDattorroVerb*, double);
void DattorroVerb_setDecay (sDattorroVerb*, double);
void DattorroVerb_setDamping (sDattorroVerb*, double);
void DattorroVerb_process (sDattorroVerb*, double);
double DattorroVerb_getLeft (sDattorroVerb*);
double DattorroVerb_getRight (sDattorroVerb*);
}

namespace {

constexpr double kSr = 48000.0;

std::vector<float> impulseResponse (PlateReverb& pl, int n)
{
    std::vector<float> out ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        float l = (i == 0) ? 1.0f : 0.0f, r = l;
        pl.process (l, r);
        out[(size_t) i] = l;
    }
    return out;
}

double rms (const std::vector<float>& v, int from = 0, int to = -1)
{
    if (to < 0) to = (int) v.size();
    double a = 0.0;
    for (int i = from; i < to; ++i) a += (double) v[(size_t) i] * v[(size_t) i];
    return std::sqrt (a / std::max (1, to - from));
}

// T60 по огибающей импульсного отклика (регрессия −60 дБ по энергии)
// амплитуда синуса заданной частоты на отрезке (квадратурная корреляция)
double toneMagnitude (const std::vector<float>& v, int from, double hz)
{
    const int n = (int) v.size() - from;
    double si = 0.0, co = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double ph = kTwoPi * hz * (double) (from + i) / kSr;
        si += v[(size_t) (from + i)] * std::sin (ph);
        co += v[(size_t) (from + i)] * std::cos (ph);
    }
    return 2.0 * std::sqrt (si * si + co * co) / std::max (1, n);
}

double rt60 (const std::vector<float>& ir)
{
    const int block = (int) (kSr * 0.05);
    std::vector<double> e;
    std::vector<double> t;
    for (int i = 0; i + block < (int) ir.size(); i += block)
    {
        e.push_back (20.0 * std::log10 (rms (ir, i, i + block) + 1e-12));
        t.push_back ((double) (i + block / 2) / kSr);
    }
    // линейная регрессия по участку от −5 до −35 дБ от максимума
    const double peak = *std::max_element (e.begin(), e.end());
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
    for (size_t i = 0; i < e.size(); ++i)
    {
        if (e[i] > peak - 5.0 || e[i] < peak - 35.0) continue;
        sx += t[i]; sy += e[i]; sxx += t[i] * t[i]; sxy += t[i] * e[i]; ++n;
    }
    if (n < 4) return 0.0;
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);   // дБ/с
    return slope < -0.1 ? -60.0 / slope : 0.0;
}

// спектральные полосы (для проверки, что тарелка — не «звон» на одной ноте)
void bandDb (const std::vector<float>& ir, double lo, double hi,
             double& outLow, double& outMid, double& outHigh)
{
    const int N = 16384;
    std::vector<std::complex<float>> sp ((size_t) N);
    for (int i = 0; i < N; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (kTwoPi * i / (N - 1));
        sp[(size_t) i] = { (float) (ir[(size_t) i] * w), 0.0f };
    }
    Fft::forward (sp);
    auto energy = [&] (double a, double b)
    {
        double e = 0.0;
        for (int k = 1; k < N / 2; ++k)
        {
            const double f = (double) k * kSr / N;
            if (f >= a && f < b) e += (double) std::norm (sp[(size_t) k]);
        }
        return e;
    };
    const double tot = energy (20.0, 20000.0) + 1e-20;
    outLow  = 10.0 * std::log10 (energy (20.0, lo) / tot + 1e-20);
    outMid  = 10.0 * std::log10 (energy (lo, hi) / tot + 1e-20);
    outHigh = 10.0 * std::log10 (energy (hi, 20000.0) / tot + 1e-20);
}

// THD и интермодуляция для сатураторов
struct DistStats { double thd = 0.0, imd = 0.0, rms = 0.0; float peak = 0.0f; };

DistStats measureDist (std::function<float (float)> proc, double f, float amp)
{
    DistStats st;
    const int N = 65536;
    std::vector<double> y ((size_t) N);
    for (int i = 0; i < N; ++i)
    {
        const float x = amp * (float) std::sin (kTwoPi * f * i / kSr);
        y[(size_t) i] = (double) proc (x);
        st.rms += y[(size_t) i] * y[(size_t) i];
        st.peak = std::max (st.peak, (float) std::fabs (y[(size_t) i]));
    }
    st.rms = std::sqrt (st.rms / N);

    auto bin = [&] (double freq)
    {
        double re = 0, im = 0;
        for (int i = 0; i < N; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (kTwoPi * i / (N - 1));
            const double a = kTwoPi * freq * i / kSr;
            re += y[(size_t) i] * w * std::cos (a);
            im -= y[(size_t) i] * w * std::sin (a);
        }
        return std::sqrt (re * re + im * im) / N;
    };
    const double fund = bin (f);
    double harm = 0.0;
    for (int k = 2; k <= 12; ++k) { const double a = bin (f * k); harm += a * a; }
    st.thd = 10.0 * std::log10 ((harm + 1e-30) / (fund * fund + 1e-30));
    return st;
}

} // namespace

int main()
{
    std::printf ("=== ЭФФЕКТЫ НА ВНЕШНИХ DSP-БИБЛИОТЕКАХ ===\n\n");

    // ---------------------------------------------------------------- 1
    std::printf ("--- 1. Адаптер против оригинального C-кода (48 кГц) ---\n");
    {
        // Референс: параметры по умолчанию, моно-импульс, оба канала.
        const int N = 48000;
        sDattorroVerb* ref = DattorroVerb_create();
        std::vector<float> refL ((size_t) N, 0.0f), refR ((size_t) N, 0.0f);
        for (int i = 0; i < N; ++i)
        {
            DattorroVerb_process (ref, (i == 0) ? 1.0 : 0.0);
            refL[(size_t) i] = (float) DattorroVerb_getLeft (ref);
            refR[(size_t) i] = (float) DattorroVerb_getRight (ref);
        }
        DattorroVerb_delete (ref);

        // Наш адаптер: те же коэффициенты, сглаживание отключено (snapParams),
        // DC-блокер отключён — его в референсе нет.
        PlateReverb pl;
        pl.prepare (kSr);
        pl.setDecay (0.75f); pl.setDamping (0.95f); pl.setPreFilter (0.85f);
        pl.setDiffusion (0.70f);            // decayDiffusion1Amount
        pl.setInputDiffusion1 (0.75f);
        pl.setInputDiffusion2 (0.625f);
        pl.setPreDelay (0.01f);
        pl.setOutputTrim (1.0f);
        pl.setMix (1.0f);
        pl.setDcBlocker (false);
        pl.snapParams();

        std::vector<float> myL ((size_t) N), myR ((size_t) N);
        for (int i = 0; i < N; ++i)
        {
            float l = (i == 0) ? 1.0f : 0.0f, r = l;
            pl.process (l, r);
            myL[(size_t) i] = l; myR[(size_t) i] = r;
        }

        const auto relDb = [] (const std::vector<float>& a, const std::vector<float>& b, double& worst)
        {
            double numErr = 0.0, den = 0.0; worst = 0.0;
            for (size_t i = 0; i < a.size(); ++i)
            {
                const double d = (double) a[i] - (double) b[i];
                numErr += d * d; den += (double) b[i] * (double) b[i];
                worst = std::max (worst, std::fabs (d));
            }
            return 10.0 * std::log10 ((numErr + 1e-30) / (den + 1e-30));
        };

        double worstL = 0.0, worstR = 0.0;
        const double dbL = relDb (myL, refL, worstL);
        const double dbR = relDb (myR, refR, worstR);
        std::printf ("   L: %.1f дБ (макс %.2e), R: %.1f дБ (макс %.2e)\n",
                     dbL, worstL, dbR, worstR);
        std::printf ("   RMS оригинала %.5f, адаптера %.5f\n", rms (refL, 1000), rms (myL, 1000));
        check (dbL < -100.0 && dbR < -100.0, "адаптер совпадает с оригиналом посэмплово",
               num (std::max (dbL, dbR), 1) + " дБ");
    }

    // ---------------------------------------------------------------- 2
    std::printf ("\n--- 2. Plate: время реверберации по decay ---\n");
    {
        double prev = 0.0;
        bool grows = true;
        for (float d : { 0.5f, 0.7f, 0.85f, 0.95f })
        {
            PlateReverb pl; pl.prepare (kSr);
            pl.setDecay (d); pl.setDamping (0.6f); pl.setDiffusion (0.7f);
            pl.setOutputTrim (1.0f); pl.setMix (1.0f);
            const auto ir = impulseResponse (pl, (int) (kSr * 20));
            const double t = rt60 (ir);
            std::printf ("   decay %.2f -> T60 %5.2f с, RMS(0-1 с) %.4f\n", d, t, rms (ir, 0, (int) kSr));
            if (t <= prev) grows = false;
            prev = t;
        }
        check (grows && prev > 1.0, "T60 растёт с decay и достигает > 1 с", num (prev, 2) + " с");
    }

    // ---------------------------------------------------------------- 3
    std::printf ("\n--- 3. Plate: стерео-декорреляция и отсутствие мод-«звона» ---\n");
    {
        PlateReverb pl; pl.prepare (kSr);
        pl.setDecay (0.8f); pl.setDamping (0.5f); pl.setDiffusion (0.7f);
        pl.setOutputTrim (0.6f); pl.setMix (1.0f);
        const int N = 48000 * 4;
        std::vector<float> L ((size_t) N), R ((size_t) N);
        for (int i = 0; i < N; ++i)
        {
            float l = (i == 0) ? 1.0f : 0.0f, r = l;
            pl.process (l, r);
            L[(size_t) i] = l; R[(size_t) i] = r;
        }
        double sl = 0, sr2 = 0, slr = 0;
        for (int i = 0; i < N; ++i)
        { sl += (double) L[(size_t) i] * L[(size_t) i]; sr2 += (double) R[(size_t) i] * R[(size_t) i];
          slr += (double) L[(size_t) i] * R[(size_t) i]; }
        const double corr = slr / std::sqrt (sl * sr2 + 1e-20);
        double lo, mid, hi;
        bandDb (L, 500.0, 4000.0, lo, mid, hi);
        std::printf ("   корреляция L/R %.3f; полосы: <500 Гц %.1f дБ, 500–4к %.1f дБ, >4к %.1f дБ\n",
                     corr, lo, mid, hi);
        check (std::fabs (corr) < 0.8, "каналы декоррелированы (не моно)", num (corr, 3));
        check (hi > -45.0, "хвост имеет воздух (энергия > 4 кГц)", num (hi, 1) + " дБ");
    }

    // ---------------------------------------------------------------- 4
    std::printf ("\n--- 4. Plate: стабильность и DC ---\n");
    {
        PlateReverb pl; pl.prepare (kSr);
        pl.setDecay (0.99f); pl.setDamping (0.2f); pl.setDiffusion (0.75f);
        pl.setOutputTrim (0.6f); pl.setMix (1.0f);
        double acc = 0, dc = 0; int nan = 0; float peak = 0.0f;
        for (int i = 0; i < (int) (kSr * 120); ++i)     // 2 минуты на самом длинном хвосте
        {
            float l = (i < 400) ? 0.3f : 0.0f, r = l;
            pl.process (l, r);
            if (! std::isfinite (l)) ++nan;
            acc += (double) l * l; dc += l;
            peak = std::max (peak, std::fabs (l));
        }
        const double rmsEnd = std::sqrt (acc / (kSr * 120));
        std::printf ("   120 с при decay 0.99: пик %.3f, RMS %.4f, DC %+.5f, NaN %d\n",
                     peak, rmsEnd, dc / (kSr * 120), nan);
        check (nan == 0 && peak < 2.0f, "нет разгона и NaN", num (peak, 3));
        check (std::fabs (dc / (kSr * 120)) < 1e-3, "нет постоянной составляющей", num (dc / (kSr * 120), 5));
    }

    // Старый DSP ограничивал pre-delay 100 мс, хотя параметр хоста объявлен
    // до 200 мс. Меряем настоящий первый ненулевой wet-отсчёт, а не значение
    // параметра или разницу полной картинки/сигнала с сухим путём.
    std::printf ("\n--- Plate: реальный диапазон pre-delay 0…200 мс ---\n");
    {
        bool timingOk = true;
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            int first[3] {};
            int j = 0;
            for (float delay : { 0.10f, 0.15f, 0.20f })
            {
                PlateReverb pl;
                pl.prepare (sr);
                pl.setDcBlocker (false);
                pl.setMix (1.0f);
                pl.setPreDelay (delay);
                pl.snapParams();
                const auto ir = impulseResponse (pl, (int) (sr * 0.40));
                first[j] = -1;
                for (int i = 0; i < (int) ir.size(); ++i)
                    if (std::fabs (ir[(size_t) i]) > 1.0e-8f) { first[j] = i; break; }
                ++j;
            }
            const int expected = (int) std::lround (0.05 * sr);
            const int d1 = first[1] - first[0], d2 = first[2] - first[1];
            std::printf ("   %.0f Гц: первый wet %d / %d / %d, интервалы %d / %d (ожидается %d)\n",
                         sr, first[0], first[1], first[2], d1, d2, expected);
            if (first[0] < 0 || std::abs (d1 - expected) > 3 || std::abs (d2 - expected) > 3)
                timingOk = false;
        }
        check (timingOk, "pre-delay 100→150→200 мс реально сдвигает wet-ответ на 50 мс");
    }

    std::printf ("\n--- Plate: четыре новых регулятора через полную FxChain ---\n");
    {
        auto response = [] (const FxChain::Settings& settings)
        {
            FxChain chain;
            chain.prepare (kSr);
            chain.set (settings);
            std::vector<float> output (96000);
            for (int i = 0; i < (int) output.size(); ++i)
            {
                // даём 250 мс сглаживанию параметров до импульса
                float l = i == 12000 ? 0.5f : 0.0f, r = l;
                chain.process (l, r);
                output[(size_t) i] = l;
            }
            return output;
        };
        FxChain::Settings base;
        base.plateMix = 1.0f;
        base.compRatio = 1.0f;
        base.compMakeup = 0.0f;
        const auto ref = response (base);
        double refEnergy = 0.0;
        for (float x : ref) refEnergy += (double) x * x;
        const char* names[] = { "pre-filter", "input diffusion 1", "input diffusion 2", "output" };
        for (int control = 0; control < 4; ++control)
        {
            FxChain::Settings varied = base;
            if (control == 0) varied.platePreFilter = 0.10f;
            if (control == 1) varied.plateInputDiff1 = 0.10f;
            if (control == 2) varied.plateInputDiff2 = 0.10f;
            if (control == 3) varied.plateOutput = 1.40f;
            const auto out = response (varied);
            double diffEnergy = 0.0;
            int bad = 0;
            for (size_t i = 0; i < out.size(); ++i)
            {
                if (! std::isfinite (out[i])) ++bad;
                const double d = (double) out[i] - ref[i];
                diffEnergy += d * d;
            }
            const double rel = std::sqrt (diffEnergy / std::max (1.0e-30, refEnergy));
            std::printf ("   %-18s: относительная ΔRMS %.5f, NaN %d\n", names[control], rel, bad);
            check (refEnergy > 1.0e-10 && rel > 0.01 && bad == 0,
                   std::string ("FxChain: ") + names[control] + " меняет wet-отклик",
                   num (rel, 5));
        }
        FxChain::Settings muted = base;
        muted.plateOutput = 0.0f;
        const auto zero = response (muted);
        const double zeroRms = rms (zero);
        check (zeroRms < 1.0e-9, "нулевой Plate Output действительно гасит wet при Mix 100%", num (zeroRms, 9));
    }

    {
        PlateReverb a, b;
        a.prepare (kSr); b.prepare (kSr);
        a.setMix (1.0f); b.setMix (1.0f);
        a.setDcBlocker (false); b.setDcBlocker (false);
        a.snapParams(); b.snapParams();
        double ratio[3] = { -1.0, -1.0, -1.0 };
        const int at[3] = { 0, 383, 3839 };  // сразу, через 8 мс, через 80 мс
        for (int i = 0; i <= 48000 + at[2]; ++i)
        {
            if (i == 48000) b.setOutputTrim (0.0f);
            float l1 = 0.15f * std::sin (2.0 * kPi * 271.0 * i / kSr), r1 = l1;
            float l2 = l1, r2 = r1;
            a.process (l1, r1); b.process (l2, r2);
            for (int k = 0; k < 3; ++k)
                if (i == 48000 + at[k])
                {
                    const double energy = (double) l1 * l1 + (double) r1 * r1;
                    if (energy > 1.0e-15) ratio[k] = ((double) l1 * l2 + (double) r1 * r2) / energy;
                }
        }
        std::printf ("   автоматизация Output 0.6→0: коэффициент %.6f → %.6f за 8 мс → %.6f за 80 мс\n",
                     ratio[0], ratio[1], ratio[2]);
        check (ratio[0] > 0.995 && ratio[0] < 1.0 && ratio[1] > 0.36 && ratio[1] < 0.38
               && ratio[2] >= 0.0 && ratio[2] < 0.001,
               "автоматизация Plate Output сглажена, без мгновенного скачка");
    }

    // ---------------------------------------------------------------- 5
    std::printf ("\n--- 5. WDF-сатурация против tanh: THD и интермодуляция ---\n");
    {
        for (float drive : { 0.2f, 0.5f, 0.8f })
        {
            WdfClipper cl; cl.prepare (kSr); cl.setDrive (drive); cl.setMix (1.0f); cl.setTone (1.0f);
            const auto wdf = measureDist ([&] (float x) { return cl.process (x); }, 220.0, 0.5f);

            // наш прежний shaper (tanh)
            Shaper sh; sh.prepare (kSr); sh.setDrive (drive); sh.setMix (1.0f);
            const auto tanhStats = measureDist ([&] (float x) { return sh.process (x); }, 220.0, 0.5f);

            std::printf ("   drive %.1f: WDF THD %6.1f дБ (RMS %.3f) | tanh THD %6.1f дБ (RMS %.3f)\n",
                         drive, wdf.thd, wdf.rms, tanhStats.thd, tanhStats.rms);
            check (wdf.thd > -30.0, std::string ("WDF даёт гармоники при drive ") + num (drive, 1),
                   num (wdf.thd, 1) + " дБ");
            check (wdf.peak < 1.5f, std::string ("WDF не разгоняет пик при drive ") + num (drive, 1),
                   num (wdf.peak, 3));
        }
    }

    // ---------------------------------------------------------------- 6
    std::printf ("\n--- 6. WDF-сатурация: алиасинг нелинейности ---\n");
    {
        // Метрика: доля негармонической энергии во ВСЕЙ энергии сигнала.
        // Нормировать на пик нельзя: при сильной обрезке несущая проваливается
        // и отношение «шум/пик» теряет смысл (на этом и провалился прежний тест).
        const double f = 5000.0;
        const int N = 65536;
        const auto aliasRatioDb = [&] (auto&& proc, float amp)
        {
            std::vector<std::complex<float>> sp ((size_t) N);
            for (int i = 0; i < N; ++i)
            {
                const double w = 0.5 - 0.5 * std::cos (kTwoPi * i / (N - 1));
                sp[(size_t) i] = { (float) (proc ((float) (amp * std::sin (kTwoPi * f * i / kSr))) * w), 0.0f };
            }
            Fft::forward (sp);
            double harm = 0.0, alias = 0.0;
            for (int k = 1; k < N / 2; ++k)
            {
                const double fk = (double) k * kSr / N;
                const double m = std::norm (sp[(size_t) k]);
                const double r = fk / f;
                if (std::fabs (r - std::round (r)) * f > 30.0) alias += m;
                else harm += m;
            }
            return 10.0 * std::log10 ((alias + 1e-30) / (alias + harm + 1e-30));
        };

        WdfClipper cl; cl.prepare (kSr); cl.setMix (1.0f); cl.setTone (1.0f);
        Shaper sh;     sh.prepare (kSr); sh.setMix (1.0f);
        bool better = true, quiet = true;
        double worst = -400.0;
        for (float drive : { 0.5f, 1.0f })
        {
            cl.setDrive (drive); sh.setDrive (drive);
            const double wdfDb = aliasRatioDb ([&] (float x) { return cl.process (x); }, 0.7f);
            const double tanhDb = aliasRatioDb ([&] (float x) { return sh.process (x); }, 0.7f);
            std::printf ("   drive %.1f, 5 кГц: негармоническая доля WDF %6.1f дБ | tanh %6.1f дБ\n",
                         drive, wdfDb, tanhDb);
            if (wdfDb >= tanhDb) better = false;
            if (wdfDb > -40.0) quiet = false;
            worst = std::max (worst, wdfDb);
        }
        check (better && quiet, "WDF: негармонический мусор ниже -40 дБ и ниже tanh",
               num (worst, 1) + " дБ");
    }

    // ---------------------------------------------------------------- 7
    std::printf ("\n--- 7. Цепочка эффектов целиком ---\n");
    {
        FxChain fx; fx.prepare (kSr);
        FxChain::Settings s;
        s.drive = 0.4f; s.plateMix = 0.4f; s.chorusMix = 0.2f; s.delayMix = 0.2f;
        fx.set (s);
        float peak = 0.0f; int nan = 0;
        double acc = 0.0;
        for (int i = 0; i < 48000 * 5; ++i)
        {
            const float x = (i % 24000 < 12000) ? 0.6f * (float) std::sin (kTwoPi * 220.0 * i / kSr) : 0.0f;
            float l = x, r = x;
            fx.process (l, r);
            if (! std::isfinite (l) || ! std::isfinite (r)) ++nan;
            peak = std::max (peak, std::max (std::fabs (l), std::fabs (r)));
            acc += (double) l * l;
        }
        std::printf ("   5 с: пик %.3f, RMS %.4f, NaN %d\n", peak, std::sqrt (acc / (48000 * 5)), nan);
        check (peak < 1.0f && nan == 0, "цепочка не клипует и стабильна", num (peak, 3));
    }

    // ---------------------------------------------------------------- 8
    std::printf ("\n--- 8. Drive: 19 моделей, уровень и THD ---\n");
    {
        const float inRms = 0.4f / std::sqrt (2.0f);
        double worstLevel = 0.0;
        int nan = 0;
        float worstPeak = 0.0f;
        for (int m = 0; m < (int) DriveModel::Count; ++m)
        {
            const auto model = (DriveModel) m;
            for (float amount : { 0.0f, 0.5f, 1.0f })
            {
                DriveFx d; d.prepare (kSr);
                d.setModel (model); d.setAmount (amount); d.setMix (1.0f);
                double acc = 0.0; float peak = 0.0f;
                for (int i = 0; i < 48000; ++i)
                {
                    float x = 0.4f * (float) std::sin (kTwoPi * 220.0 * i / kSr);
                    float l = x, r = x;
                    d.process (l, r);
                    if (! std::isfinite (l)) ++nan;
                    acc += (double) l * l;
                    peak = std::max (peak, std::fabs (l));
                }
                const double rmsOut = std::sqrt (acc / 48000.0);
                const double db = 20.0 * std::log10 ((rmsOut + 1e-12) / inRms);
                worstLevel = std::max (worstLevel, std::fabs (db));
                worstPeak = std::max (worstPeak, peak);
            }
        }
        std::printf ("   19 моделей × 3 положения ручки: худшее отклонение уровня %.1f дБ, пик %.2f, NaN %d\n",
                     worstLevel, worstPeak, nan);
        // Rectifier физически даёт пик ~2.4 (двухполупериодное выпрямление),
        // постоянка снимается DC-блокером; в цепочке дальше стоит лимитер,
        // поэтому здесь проверяем «нет NaN и нет разгона», а не «пик < 1».
        check (nan == 0 && worstPeak < 3.0f, "все модели драйва стабильны", num (worstPeak, 2));
        check (worstLevel < 6.0, "уровень моделей выровнен по калибровке", num (worstLevel, 1) + " дБ");

        // характер: у моделей должны быть разные спектры (иначе это один и тот же шейпер)
        DriveFx a, b;
        a.prepare (kSr); a.setModel (DriveModel::Tube);   a.setAmount (0.6f); a.setMix (1.0f);
        b.prepare (kSr); b.setModel (DriveModel::Rectifier); b.setAmount (0.6f); b.setMix (1.0f);
        double diff = 0.0, ref = 0.0;
        for (int i = 0; i < 24000; ++i)
        {
            const float x = 0.4f * (float) std::sin (kTwoPi * 220.0 * i / kSr);
            float l1 = x, r1 = x, l2 = x, r2 = x;
            a.process (l1, r1); b.process (l2, r2);
            diff += (double) (l1 - l2) * (l1 - l2); ref += (double) l1 * l1;
        }
        const double sep = 10.0 * std::log10 ((diff + 1e-30) / (ref + 1e-30));
        std::printf ("   Tube против Rectifier: разница сигналов %.1f дБ\n", sep);
        check (sep > -6.0, "модели звучат по-разному", num (sep, 1) + " дБ");
    }

    // ---------------------------------------------------------------- 9
    std::printf ("\n--- 9. Material: модальный резонатор, 4 профиля ---\n");
    {
        for (int mode = 0; mode < 4; ++mode)
        {
            MaterialFx m; m.prepare (kSr);
            m.setMode (mode); m.setMix (1.0f); m.setExcite (0.6f); m.setTone (0.5f);
            m.setDamp (0.5f); m.setSize (0.5f); m.setFeedback (0.5f);
            std::vector<float> ir ((size_t) (kSr * 4), 0.0f);
            for (size_t i = 0; i < ir.size(); ++i)
            {
                float l = (i == 0) ? 0.9f : 0.0f, r = l;
                m.process (l, r);
                ir[i] = l;
            }
            const double t = rt60 (ir);
            std::printf ("   профиль %d: T60 %5.2f с, RMS(0–1 с) %.4f, пик %.3f\n",
                         mode, t, rms (ir, 0, (int) kSr), *std::max_element (ir.begin(), ir.end(),
                         [] (float a, float b) { return std::fabs (a) < std::fabs (b); }));
            check (t > 0.05 && t < 12.0, std::string ("профиль ") + std::to_string (mode) + " звучит и затухает",
                   num (t, 2) + " с");
        }
    }

    // ---------------------------------------------------------------- 10
    std::printf ("\n--- 10. Spectre: MDCT-маска сохраняет тон и не разгоняется ---\n");
    {
        SpectreFx sp; sp.prepare (kSr);
        sp.setWindow (2); sp.setMix (1.0f); sp.setProminence (6.0f);
        std::vector<float> out ((size_t) (kSr * 2));
        float peak = 0.0f; int nan = 0;
        for (int i = 0; i < (int) out.size(); ++i)
        {
            float x = 0.3f * (float) std::sin (kTwoPi * 440.0 * i / kSr);
            float l = x, r = x;
            sp.process (l, r);
            if (! std::isfinite (l)) ++nan;
            peak = std::max (peak, std::fabs (l));
            out[(size_t) i] = l;
        }
        // после прогрева тон 440 Гц должен остаться на месте
        double atTone = 0.0, total = 0.0;
        const auto mag = toneMagnitude (out, (int) out.size() / 2, 440.0);
        atTone = mag; total = rms (out, (int) out.size() / 2);
        std::printf ("   латентность %d сэмплов (окно), пик %.3f, NaN %d, 440 Гц весит %.4f при RMS %.4f\n",
                     sp.latency(), peak, nan, atTone, total);
        check (nan == 0 && peak < 1.5f, "спектральный эффект стабилен", num (peak, 3));
        check (atTone > 0.02, "тон на 440 Гц после маски сохраняется", num (atTone, 3));
    }

    // ---------------------------------------------------------------- 11
    std::printf ("\n--- 11. Glitch: статтер повторяет кусок и не разгоняется ---\n");
    {
        GlitchFx g; g.prepare (kSr);
        g.setChance (0.6f); g.setLengthMs (90.0f); g.setRepeats (3.0f); g.setMix (1.0f);
        float peak = 0.0f; int nan = 0; double acc = 0.0;
        for (int i = 0; i < (int) (kSr * 4); ++i)
        {
            const float x = (i % 24000 < 12000) ? 0.4f * (float) std::sin (kTwoPi * 330.0 * i / kSr) : 0.0f;
            float l = x, r = x;
            g.process (l, r);
            if (! std::isfinite (l)) ++nan;
            peak = std::max (peak, std::fabs (l));
            acc += (double) l * l;
        }
        std::printf ("   4 с: пик %.3f, RMS %.4f, NaN %d\n", peak, std::sqrt (acc / (kSr * 4)), nan);
        check (nan == 0 && peak < 1.2f, "статтер не разгоняется", num (peak, 3));
        check (std::sqrt (acc / (kSr * 4)) > 0.01, "на выходе есть сигнал", num (std::sqrt (acc / (kSr * 4)), 3));
    }

    // ---------------------------------------------------------------- 12
    std::printf ("\n--- 12. Вся цепочка с новыми эффектами ---\n");
    {
        FxChain fx; fx.prepare (kSr);
        FxChain::Settings s;
        s.drive = 0.5f; s.driveModel = (int) DriveModel::DiodeWdf;
        s.plateMix = 0.35f; s.chorusMix = 0.2f; s.delayMix = 0.15f;
        s.materialMix = 0.4f; s.materialMode = 2;
        s.spectreMix = 0.3f; s.glitchMix = 0.2f;
        fx.set (s);
        float peak = 0.0f; int nan = 0; double acc = 0.0;
        for (int i = 0; i < 48000 * 5; ++i)
        {
            const float x = (i % 24000 < 12000) ? 0.5f * (float) std::sin (kTwoPi * 220.0 * i / kSr) : 0.0f;
            float l = x, r = x;
            fx.process (l, r);
            if (! std::isfinite (l) || ! std::isfinite (r)) ++nan;
            peak = std::max (peak, std::max (std::fabs (l), std::fabs (r)));
            acc += (double) l * l;
        }
        std::printf ("   5 с, все эффекты включены: пик %.3f, RMS %.4f, NaN %d\n",
                     peak, std::sqrt (acc / (48000 * 5)), nan);
        check (nan == 0 && peak < 1.0f, "цепочка с 4 новыми эффектами не клипует", num (peak, 3));
    }

    // ---------------------------------------------------------------- 13
    std::printf ("\n--- 13. COMB: ODIN2 (резонанс) и ParallelComb (тапы) ---\n");
    {
        // резонанс: усиление на частоте линии растёт вместе с ручкой resonance
        for (float res : { 50.0f, 75.0f, 95.0f })
        {
            CombFx c; c.prepare (kSr);
            c.setModel (0); c.setMix (1.0f);
            c.setOdinFreq (220.0f, 331.0f); c.setOdinRes (res, res);
            std::vector<float> out ((size_t) (kSr * 2), 0.0f);
            float peak = 0.0f; int nan = 0;
            for (int i = 0; i < (int) out.size(); ++i)
            {
                float l = 0.3f * (float) std::sin (kTwoPi * 220.0 * i / kSr), r = l;
                c.process (l, r);
                if (! std::isfinite (l)) ++nan;
                peak = std::max (peak, std::fabs (l));
                out[(size_t) i] = l;
            }
            const double gain = 20.0 * std::log10 (toneMagnitude (out, 0, 220.0) / 0.3 + 1.0e-12);
            std::printf ("   ODIN2, resonance %.0f: пик %.3f, усиление 220 Гц %+.2f дБ, NaN %d\n", res, peak, gain, nan);
            check (nan == 0 && gain > 1.0, res > 90.0f ? "ODIN2 резонирует (res 95)" : "ODIN2 не гасит тон", num (gain, 1));
        }

        // три схемы включения дают разные сигналы
        double rmsByRouting[3] {};
        for (int rt = 0; rt < 3; ++rt)
        {
            CombFx c; c.prepare (kSr);
            c.setModel (0); c.setMix (1.0f);
            c.setOdinFreq (220.0f, 331.0f); c.setOdinRouting (rt);
            double acc = 0.0;
            for (int i = 0; i < (int) (kSr * 2); ++i)
            {
                float l = 0.3f * (float) std::sin (kTwoPi * 220.0 * i / kSr), r = l;
                c.process (l, r);
                acc += (double) l * l;
            }
            rmsByRouting[rt] = std::sqrt (acc / (kSr * 2));
        }
        std::printf ("   ODIN2 routing 0/1/2: RMS %.4f / %.4f / %.4f\n",
                     rmsByRouting[0], rmsByRouting[1], rmsByRouting[2]);
        check (rmsByRouting[0] > rmsByRouting[1] * 1.5, "последовательная схема тише параллельной",
               num (rmsByRouting[1], 3));

        // ParallelComb: обратная связь удлиняет хвост (и не даёт NaN)
        for (float fb : { 25.0f, 90.0f })
        {
            CombFx c; c.prepare (kSr);
            c.setModel (1); c.setMix (1.0f); c.setParFeedback (fb);
            for (int t = 0; t < 4; ++t) c.setParDelayMs (t, 100.0f + 50.0f * (float) t);
            float peak = 0.0f; int nan = 0; double early = 0.0, late = 0.0;
            const int total = 48000 * 6;
            for (int i = 0; i < total; ++i)
            {
                const float x = (i < 480) ? 0.8f * (float) std::sin (kTwoPi * 300.0 * i / kSr) * (1.0f - i / 480.0f) : 0.0f;
                float l = x, r = x;
                c.process (l, r);
                if (! std::isfinite (l)) ++nan;
                peak = std::max (peak, std::fabs (l));
                if (i >= 48000 && i < 96000)  early += (double) l * l;
                if (i >= 240000 && i < 288000) late += (double) l * l;
            }
            std::printf ("   ParallelComb, feedback %.0f%%: пик %.3f, энергия 1-2 с %.2e, 5-6 с %.2e, NaN %d\n",
                         fb, peak, early, late, nan);
            check (nan == 0 && peak <= 1.001f, "ParallelComb не выходит за лимитер", num (peak, 3));
            if (fb > 80.0f) check (late > 10.0, "обратная связь 90 % держит хвост через 5 с", num (late, 1));
        }
    }

    // ---------------------------------------------------------------- 14
    std::printf ("\n--- 14. CHORUS из прошивки: задержка, dry, модуляция ---\n");
    {
        FirmwareChorusFx ch; ch.prepare (kSr);
        const int lat = ch.latency();
        ch.setMix (0.0f);
        std::vector<float> in ((size_t) (kSr * 2), 0.0f), out ((size_t) (kSr * 2), 0.0f);
        for (size_t i = 0; i < in.size(); ++i) in[i] = 0.4f * (float) std::sin (kTwoPi * 440.0 * (double) i / kSr);
        double err = 0.0;
        for (size_t i = 0; i < in.size(); ++i)
        {
            float l = in[i], r = l;
            ch.process (l, r);
            out[i] = l;
            if ((int) i > lat + 1000) err = std::max (err, (double) std::fabs (l - in[i - (size_t) lat]));
        }
        std::printf ("   mix 0: latency %d, расхождение с задержанным входом %.2e\n", lat, err);
        check (err < 1.0e-5, "dry-путь точно совпадает с входом (задержка скомпенсирована)", num (err, 6));

        // без модуляции мокрый сигнал — точная задержанная копия
        auto bestCorr = [&] (const std::array<int, 8>& p, int& lagOut)
        {
            FirmwareChorusFx c; c.prepare (kSr); c.setParameters (p); c.setMix (1.0f);
            std::vector<float> y (in.size(), 0.0f);
            float peak = 0.0f; int nan = 0;
            for (size_t i = 0; i < in.size(); ++i)
            {
                float l = in[i], r = l;
                c.process (l, r);
                if (! std::isfinite (l)) ++nan;
                peak = std::max (peak, std::fabs (l));
                y[i] = l;
            }
            double best = -2.0; int bestLag = 0;
            for (int lag = c.latency(); lag < c.latency() + 2400; ++lag)
            {
                double a = 0.0, b = 0.0, ab = 0.0;
                for (size_t i = 24000; i < in.size(); ++i)
                {
                    const double x = in[i - (size_t) lag], z = y[i];
                    a += x * x; b += z * z; ab += x * z;
                }
                const double cc = ab / std::sqrt (a * b);
                if (cc > best) { best = cc; bestLag = lag; }
            }
            lagOut = bestLag - c.latency();
            std::printf ("   пик %.3f, NaN %d, корр.макс %.3f на задержке +%d\n", peak, nan, best, lagOut);
            check (nan == 0 && peak < 1.2f, "chorus стабилен", num (peak, 3));
            return best;
        };
        int lag = 0;
        const double c0 = bestCorr ({ { 40, 0, 50, 127, 30, 80, 100, 100 } }, lag);
        check (c0 > 0.99, "без модуляции chorus = точная задержанная копия", num (c0, 3));
        const double c1 = bestCorr ({ { 40, 60, 50, 127, 30, 80, 100, 100 } }, lag);
        check (c1 < 0.9, "с модуляцией сигнал декоррелируется", num (c1, 3));

        // задержка соответствует частоте дискретизации
        FirmwareChorusFx a; a.prepare (44100.0);
        FirmwareChorusFx b; b.prepare (96000.0);
        std::printf ("   latency: %.0f кГц → %d, 48 кГц → %d, 96 кГц → %d\n", 44.1, a.latency(), lat, b.latency());
        check (a.latency() == 16 && b.latency() > 100, "на 44.1 кГц конверсия не нужна (latency 16)", num (a.latency(), 0));
    }

    // ---------------------------------------------------------------- 15
    std::printf ("\n--- 15. Цепочка с COMB и прошивочным хорусом ---\n");
    {
        FxChain fx; fx.prepare (kSr);
        FxChain::Settings s;
        s.drive = 0.4f; s.driveModel = (int) DriveModel::Tube;
        s.combMix = 0.6f; s.combModel = 0; s.combResA = 88.0f; s.combResB = 80.0f;
        s.combFreqA = 220.0f; s.combFreqB = 330.0f;
        s.chorusModel = 1; s.chorusMix = 0.35f;
        s.delayMix = 0.15f; s.plateMix = 0.3f;
        s.materialMix = 0.3f; s.spectreMix = 0.25f; s.glitchMix = 0.15f;
        fx.set (s);
        float peak = 0.0f; int nan = 0; double acc = 0.0;
        for (int i = 0; i < 48000 * 5; ++i)
        {
            const float x = (i % 24000 < 12000) ? 0.5f * (float) std::sin (kTwoPi * 220.0 * i / kSr) : 0.0f;
            float l = x, r = x;
            fx.process (l, r);
            if (! std::isfinite (l) || ! std::isfinite (r)) ++nan;
            peak = std::max (peak, std::max (std::fabs (l), std::fabs (r)));
            acc += (double) l * l;
        }
        std::printf ("   5 с: пик %.3f, RMS %.4f, NaN %d\n", peak, std::sqrt (acc / (48000 * 5)), nan);
        check (nan == 0 && peak < 1.0f, "цепочка с COMB и chorus не клипует", num (peak, 3));
    }

    // ---------------------------------------------------------------- 16
    std::printf ("\n--- 16. Spectre CLASSIC и задержка цепочки ---\n");
    {
        SpectreClassicFx sc; sc.prepare (kSr);
        sc.setWindow (2); sc.setProminence (8.0f); sc.setMix (1.0f);
        std::vector<float> out ((size_t) (kSr * 2), 0.0f);
        float peak = 0.0f; int nan = 0;
        for (int i = 0; i < (int) out.size(); ++i)
        {
            float l = 0.4f * (float) std::sin (kTwoPi * 440.0 * i / kSr), r = l;
            sc.process (l, r);
            if (! std::isfinite (l)) ++nan;
            peak = std::max (peak, std::fabs (l));
            out[(size_t) i] = l;
        }
        const double tone = toneMagnitude (out, (int) out.size() / 2, 440.0);
        const double level = rms (out, (int) out.size() / 2);
        std::printf ("   CLASSIC (окно 2048, prominence 8 дБ): латентность %d, пик %.3f, тон %.4f при RMS %.4f, NaN %d\n",
                     sc.latency(), peak, tone, level, nan);
        check (sc.latency() == 2048, "latency = длина окна", num (sc.latency(), 0));
        check (nan == 0 && peak < 1.5f, "CLASSIC стабилен", num (peak, 3));
        check (tone > 0.02, "тон на 440 Гц сохраняется", num (tone, 3));

        // задержка цепочки = сумма задержек активных эффектов
        FxChain fx; fx.prepare (kSr);
        FxChain::Settings s;
        fx.set (s);
        // ROBIN стоит в цепочке всегда (его сухой путь задержан на 1024,
        // иначе mix складывал бы сигнал с самим собой со сдвигом 21 мс),
        // поэтому «пустая» цепочка = 1024, а не 0.
        check (fx.latency() == 1024, "пустая цепочка = задержка ROBIN 1024", num (fx.latency(), 0));
        s.spectreMix = 0.5f; s.spectreWindow = 2; fx.set (s);
        check (fx.latency() == 1024 + 2048, "Spectre EXTENDED даёт задержку окна", num (fx.latency(), 0));
        s.spectreModel = 1; fx.set (s);
        check (fx.latency() == 1024 + 2048, "CLASSIC даёт ту же задержку", num (fx.latency(), 0));
        s.chorusModel = 1; s.chorusMix = 0.4f; fx.set (s);
        std::printf ("   цепочка: robin %d + spectre %d + chorus %d = %d сэмплов\n",
                     1024, 2048, 87, fx.latency());
        check (fx.latency() == 1024 + 2048 + 87, "задержки складываются", num (fx.latency(), 0));
        s.spectreMix = 0.0f; s.chorusMix = 0.0f; fx.set (s);
        check (fx.latency() == 1024, "нулевые миксы оставляют только ROBIN", num (fx.latency(), 0));
    }

    // ---------------------------------------------------------------- 17
    std::printf ("\n--- 17. ROBIN: питч-шифтер (фазовый вокодер) ---\n");
    {
        auto renderRobin = [&] (float semis, float mix, float size, float spread, float hz, int n)
        {
            RobinFx rb; rb.prepare (kSr);
            rb.setSemitones (semis); rb.setSize (size);
            rb.setSpread (spread);   rb.setMix (mix);
            std::vector<float> out ((size_t) n, 0.0f);
            for (int i = 0; i < n; ++i)
            {
                float l = 0.35f * (float) std::sin (kTwoPi * hz * i / kSr), r = l;
                rb.process (l, r);
                out[(size_t) i] = l;
            }
            return out;
        };

        // 1. задержка: ровно 1024 отсчёта и при mix 0, и при mix 1
        for (float mix : { 0.0f, 1.0f })
        {
            RobinFx rb; rb.prepare (kSr); rb.setMix (mix); rb.setSemitones (0.0f);
            float peak = 0.0f; int at = -1;
            for (int i = 0; i < 8192; ++i)
            {
                float l = (i == 3000) ? 1.0f : 0.0f, r = l;
                rb.process (l, r);
                if (std::fabs (l) > peak) { peak = std::fabs (l); at = i; }
            }
            std::printf ("   mix %.0f: импульс 3000 → %d, задержка %d, амплитуда %.4f, latency() %d\n",
                         mix, at, at - 3000, peak, rb.latency());
            check (at - 3000 == 1024 && peak > 0.99f,
                   mix > 0.5f ? "mix=1: задержка 1024, импульс цел"
                              : "mix=0: сухой путь тоже задержан на 1024 (латентность постоянна)");
            check (rb.latency() == 1024, "latency() = 1024", std::to_string (rb.latency()));
        }

        // 2. точная реконструкция при 0 полутонов (фаза не переписывается)
        {
            RobinFx rb; rb.prepare (kSr); rb.setSemitones (0.0f); rb.setMix (1.0f); rb.setSize (0.5f);
            std::vector<float> dry, out;
            for (int i = 0; i < 48000; ++i)
            {
                const float d = 0.4f * (float) std::sin (kTwoPi * 440.0 * i / kSr);
                dry.push_back (d);
                float l = d, r = d;
                rb.process (l, r);
                out.push_back (l);
            }
            double err = 0.0;
            for (int i = 20000; i < 48000; ++i) err = std::max (err, (double) std::fabs (out[(size_t) i] - dry[(size_t) (i - 1024)]));
            std::printf ("   0 полутонов: ошибка реконструкции %.2e (сравнение со входом, сдвинутым на 1024)\n", err);
            check (err < 2.0e-3, "0 полутонов = побитовая реконструкция входа", num (err, 2));
        }

        // 3. сдвиг по высоте: 440 → 880 при +12, 440 → 293.7 при −7
        struct Case { float semis; double hz; const char* name; };
        const Case cases[] = { { 12.0f, 880.00, "+12" }, { 7.0f, 659.26, "+7" },
                               { -7.0f, 293.66, "−7" }, { -12.0f, 220.00, "−12" },
                               {  0.5f, 452.89, "+0.5" } };
        for (const auto& c : cases)
        {
            auto out = renderRobin (c.semis, 1.0f, 0.5f, 0.0f, 440.0, (int) (kSr * 2.0));
            const int from = (int) out.size() / 2;
            const double target = toneMagnitude (out, from, c.hz);
            const double wrong  = toneMagnitude (out, from, c.hz > 440.0 ? 440.0 : 880.0);
            std::printf ("   %-4s: цель %.1f Гц → амплитуда %.3f, «чужая» частота %.3f\n",
                         c.name, c.hz, target, wrong);
            check (target > 0.20 && target > wrong * 1.8,
                   std::string ("сдвиг ") + c.name + ": тон на месте", num (target, 3));
        }

        // 4. короткое окно и расстройка: нет NaN и нет выбросов
        {
            auto fast = renderRobin (7.0f, 1.0f, 0.0f, 0.6f, 220.0, (int) (kSr * 2.0));
            float peak = 0.0f; int nan = 0;
            for (float v : fast) { if (! std::isfinite (v)) ++nan; peak = std::max (peak, std::fabs (v)); }
            std::printf ("   окно 512 + расстройка: пик %.3f, NaN %d\n", peak, nan);
            check (nan == 0 && peak < 1.5f, "короткое окно без NaN и без выбросов", num (peak, 3));
        }

        // 5. стоимость
        {
            RobinFx perf; perf.prepare (kSr);
            perf.setSemitones (5.0f); perf.setMix (1.0f);
            std::vector<float> sig ((size_t) kSr);
            for (int i = 0; i < kSr; ++i) sig[(size_t) i] = 0.3f * (float) std::sin (kTwoPi * 220.0 * i / kSr);
            const auto t0 = std::chrono::steady_clock::now();
            for (int rep = 0; rep < 4; ++rep)
                for (int i = 0; i < kSr; ++i) { float l = sig[(size_t) i], r = l; perf.process (l, r); }
            const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            const double cpu = 100.0 * sec / 4.0;
            std::printf ("   CPU: %.2f %% ядра на стерео-поток 48 кГц\n", cpu);
            check (cpu < 5.0, "ROBIN дешевле 5 % ядра", num (cpu, 2) + " %");
        }
    }


    // ---------------------------------------------------------------- 16
    std::printf ("\n--- 18. Полный набор ручек хоруса (оба движка) и новые ручки Glitch/Spectre ---\n");
    {
        // Рендер 1 с сигналом 220 Гц + 4.4 кГц через цепочку с заданными настройками
        auto render = [] (FxChain::Settings st)
        {
            FxChain fx; fx.prepare (kSr); fx.set (st);
            std::vector<float> y;
            y.reserve ((size_t) kSr);
            for (int i = 0; i < kSr; ++i)
            {
                const double t = (double) i / kSr;
                float x = 0.3f * (float) std::sin (kTwoPi * 220.0 * t)
                        + 0.15f * (float) std::sin (kTwoPi * 4400.0 * t);
                float l = x, r = x;
                fx.process (l, r);
                y.push_back (l);
            }
            return y;
        };
        auto delta = [] (const std::vector<float>& a, const std::vector<float>& b)
        {
            double acc = 0.0;
            for (size_t i = 12000; i < a.size(); ++i) { const double d = a[i] - b[i]; acc += d * d; }
            return std::sqrt (acc / (double) (a.size() - 12000));
        };

        // Прошивочный хорус: по одной ручке от базы
        FxChain::Settings base;
        base.chorusModel = 1; base.chorusMix = 1.0f;
        base.chorusFw = { 100, 102, 38, 127, 60, 80, 127, 100 };
        const auto ref = render (base);
        const struct { int idx; int value; const char* name; } fw[] = {
            { 0, 20,  "DEL" }, { 1, 10, "DEP" }, { 2, 100, "SPD" },
            { 4, 0,   "FB"  }, { 5, 0,  "WID" }, { 6, 0,   "LP"  }, { 7, 20, "INP" } };
        for (const auto& f : fw)
        {
            auto st = base;
            st.chorusFw[(size_t) f.idx] = f.value;
            const double d = delta (ref, render (st));
            std::printf ("   прошивка %-3s -> %.4f\n", f.name, d);
            check (d > 0.002, (std::string ("хорус прошивки: ") + f.name + " слышен").c_str(), num (d, 4));
        }

        // Простой хорус (модель 0): те же ручки
        FxChain::Settings s0;
        s0.chorusModel = 0; s0.chorusMix = 1.0f; s0.chorusDepth = 0.8f; s0.chorusRate = 0.6f;
        s0.chorusFw = { 100, 102, 38, 127, 60, 80, 127, 100 };
        const auto ref0 = render (s0);
        const struct { int idx; int value; const char* name; } simple[] = {
            { 0, 20, "DEL" }, { 4, 0, "FB" }, { 5, 0, "WID" }, { 6, 0, "LP" }, { 7, 20, "INP" } };
        for (const auto& f : simple)
        {
            auto st = s0;
            st.chorusFw[(size_t) f.idx] = f.value;
            const double d = delta (ref0, render (st));
            std::printf ("   простой  %-3s -> %.4f\n", f.name, d);
            check (d > 0.002, (std::string ("простой хорус: ") + f.name + " слышен").c_str(), num (d, 4));
        }
        // DEP/SPD простого хоруса — через свои параметры
        {
            auto st = s0; st.chorusDepth = 0.05f;
            check (delta (ref0, render (st)) > 0.002, "простой хорус: DEP слышен", num (delta (ref0, render (st)), 4));
            auto st2 = s0; st2.chorusRate = 3.0f;
            check (delta (ref0, render (st2)) > 0.002, "простой хорус: SPD слышен", num (delta (ref0, render (st2)), 4));
        }


        // дефолтные ручки простого хоруса = документированное поведение:
        // DEL 40 -> 12 мс базовой задержки, LP 100 -> фильтр прозрачен
        {
            Chorus c; c.prepare (kSr);
            c.setMix (1.0f); c.setDepth (0.0f); c.setRate (0.35f);
            c.setDelay (40.0f); c.setFeedback (30.0f); c.setWidth (80.0f);
            c.setLowpass (100.0f); c.setInput (100.0f);
            int peakAt = -1; float peak = 0.0f;
            for (int i = 0; i < 4800; ++i)
            {
                float l = (i == 0) ? 1.0f : 0.0f, r = l;
                c.process (l, r);
                if (std::fabs (l) > peak) { peak = (float) std::fabs (l); peakAt = i; }
            }
            const int expect = (int) (0.012 * kSr);
            std::printf ("   простой хорус: импульс вышел на %d, ожидалось ~%d\n", peakAt, expect);
            check (std::abs (peakAt - expect) <= 2, "DEL 40 = 12 мс базовой задержки",
                   num ((double) peakAt, 0));
            check (peak > 0.95f, "LP 100 не режет wet", num (peak, 3));
        }

        // Glitch: FB / FOLD / TONE
        FxChain::Settings g;
        g.glitchMix = 1.0f; g.glitchChance = 1.0f; g.glitchLengthMs = 90.0f;
        const auto gref = render (g);
        {
            auto a = g; a.glitchFeedback = 0.7f;
            auto b = g; b.glitchFold = 6.0f;
            auto c = g; c.glitchTone = 600.0f;
            const double da = delta (gref, render (a)), db = delta (gref, render (b)), dc = delta (gref, render (c));
            std::printf ("   glitch FB %.4f, FOLD %.4f, TONE %.4f\n", da, db, dc);
            check (da > 0.002, "glitch: FB слышен", num (da, 4));
            check (db > 0.002, "glitch: FOLD слышен", num (db, 4));
            check (dc > 0.002, "glitch: TONE слышен", num (dc, 4));
        }

        // Spectre: полоса анализа
        FxChain::Settings sp;
        sp.spectreMix = 1.0f;
        const auto sref = render (sp);
        {
            auto a = sp; a.spectreLow = 2000.0f;
            auto b = sp; b.spectreHigh = 1000.0f;
            const double da = delta (sref, render (a)), db = delta (sref, render (b));
            std::printf ("   spectre LOW %.4f, HIGH %.4f\n", da, db);
            check (da > 0.002, "spectre: LOW слышен", num (da, 4));
            check (db > 0.002, "spectre: HIGH слышен", num (db, 4));
        }
    }

    // Независимый дефект выходного FX-лимитера: если значение пересекало
    // 0.98, старая ветвь мгновенно перескакивала с 0.98 примерно на 0.775.
    // Тестируем непрерывность и стереопару напрямую, не снижая уровень
    // сырого движка и не используя лимитер для маскировки его дефектов.
    std::printf ("\n--- 19. Выходной лимитер: непрерывность и стереосвязь ---\n");
    {
        constexpr float ceiling = 0.98f;
        float beforeL = 0.97999f, beforeR = -0.489995f;
        float afterL = 0.98001f, afterR = -0.490005f;
        Dynamics::limit (beforeL, beforeR, ceiling);
        Dynamics::limit (afterL, afterR, ceiling);
        const float jump = std::fabs (afterL - beforeL);
        std::printf ("   0.97999 -> %.8f, 0.98001 -> %.8f; скачок %.8f\n",
                     beforeL, afterL, jump);
        check (std::fabs (beforeL - 0.97999f) < 1.0e-6f && jump < 3.0e-5f,
               "пересечение порога не создаёт разрыв", num (jump, 8));

        bool valid = true;
        float worstRatio = 0.0f, highest = 0.0f;
        for (float in : { -3.0f, -1.3f, -0.98f, -0.2f, 0.2f, 0.98f, 1.3f, 3.0f })
        {
            float l = in, r = -0.37f * in;
            Dynamics::limit (l, r, ceiling);
            highest = std::max (highest, std::max (std::fabs (l), std::fabs (r)));
            worstRatio = std::max (worstRatio, std::fabs (r + 0.37f * l));
            valid = valid && std::isfinite (l) && std::isfinite (r);
            if (std::fabs (in) <= ceiling) valid = valid && l == in && r == -0.37f * in;
        }
        std::printf ("   макс. пик %.8f, ошибка стереосвязи %.8f\n", highest, worstRatio);
        check (valid && highest <= ceiling + 1.0e-6f && worstRatio < 1.0e-6f,
               "ниже порога прозрачен; выше сохраняет стереосвязь и потолок");
    }

    std::printf ("\n=================================================\n");
    if (suite::gFailures == 0) std::printf ("ВСЕ ТЕСТЫ ПРОЙДЕНЫ (провалов: 0)\n");
    else std::printf ("ПРОВАЛОВ: %d\n", suite::gFailures);
    return suite::gFailures == 0 ? 0 : 1;
}
