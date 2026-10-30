// Контент для Sample / Granular / Resynth / Organics: банк one-shot'ов.
//
// В Terrain это 2 177 записей с реального железа. Здесь источники
// СИНТЕЗИРУЮТСЯ процедурно (нет ни одного внешнего файла), но с той же
// структурой, которая нужна движкам: питч-стабильный основной тон 110 Гц,
// атака, тело, хвост, и — главное — MIP-уровни для питч-шифта вверх.
//
// Ключевая деталь: у каждого сэмпла строятся MIP-уровни по октавам
// (полуполосная децимация), с компенсацией задержки фильтра. Без них при
// повышении ноты высокие частоты заворачиваются внутрь слышимой полосы.
// Количество уровней ограничивается длиной записи, чтобы даже самый короткий
// уровень безопасно читался 32-отводным sinc-интерполятором.
#pragma once

#include "../core/dsp.h"
#include "../core/fft.h"
#include "../engines/halfband.h"

#include <string>
#include <vector>
#include <cstdio>
#include <cstring>

namespace orogen {

inline constexpr int kSampleLevels = 11;     // максимум: 1x … 1024x down
inline constexpr int kMinMipLength = 64;     // не строить уровни короче sinc-окна
inline constexpr double kSampleF0 = 110.0;   // основной тон всех источников
inline constexpr double kSampleRate = 48000.0;

enum class OneShotKind
{
    FmTine,      // FM-колокольчик (Rhodes-подобный)
    Karplus,     // щипок струны (Karplus-Strong)
    Choir,       // аддитивный «хор» с вибрато
    GlassHit,    // модальный удар по стеклу
    TapePad,     // пила через «ленту» с wow
    WoodKnock,   // шумовой удар + резонаторы
    VoxAh,       // формантный «а»
    SubBass      // синус с падением высоты
};

inline const char* oneShotName (OneShotKind k)
{
    switch (k)
    {
        case OneShotKind::FmTine:    return "Fm Tine";
        case OneShotKind::Karplus:   return "Karplus Pluck";
        case OneShotKind::Choir:     return "Choir Ah";
        case OneShotKind::GlassHit:  return "Glass Hit";
        case OneShotKind::TapePad:   return "Tape Pad";
        case OneShotKind::WoodKnock: return "Wood Knock";
        case OneShotKind::VoxAh:     return "Vox Ah";
        case OneShotKind::SubBass:   return "Sub Bass";
    }
    return "?";
}

struct OneShot
{
    std::string name;
    // levels[l]: сэмплы с полосой SR/2^(l+1) и длиной len >> l
    std::vector<std::vector<float>> levels;
    std::vector<int> lens;
    int rootNote = 45;                        // A2 = 110 Гц

    // Точки петли в отсчётах уровня 0 (ищутся один раз при сборке банка).
    // Органикам нужна петля, длина которой кратна периоду основного тона:
    // случайная точка даёт склейку в противофазе и «жужжание».
    int loopStart = 0, loopEnd = 0;

    // Огибающая (RMS по окну 10 мс, шаг 10 мс) уровня 0.
    // Нужна грануляру: зерно, взятое из хвоста затухающей записи, звучит на
    // 25 дБ тише зерна из тела — и уровень голоса начинает зависеть от позиции
    // и питч-рейта (измерено: 27 дБ разброса между нотами 45 и 69).
    // По огибающей амплитуда зерна нормируется к «телу» записи.
    std::vector<float> env;
    int envStep = 480;
    // Префикс суммы квадратов 10-мс ячеек. Строится с огибающей при загрузке
    // банка: средняя энергия произвольного отрезка затем считается за O(1),
    // без аллокаций и без пропуска коротких всплесков на длинных источниках.
    std::vector<double> envEnergyPrefix;

    // Медиана огибающей по первым 60 % (атака и хвост не в счёт), считается
    // ОДИН РАЗ при сборке банка. Раньше она считалась в bodyLevel() на каждый
    // prepare() грануляра, а там копия огибающей в std::vector — то есть
    // аллокация в аудио-потоке (найдено счётчиком new/delete в test_instrument).
    float bodyRms = 1.0f;

    inline float envAt (float pos) const noexcept
    {
        if (env.empty()) return 1.0f;
        int i = (int) (pos / (float) envStep);
        i = clamp (i, 0, (int) env.size() - 1);
        return env[(size_t) i];
    }

    inline double meanEnvEnergy (double from, double to) const noexcept
    {
        if (env.empty()) return 1.0;
        if (envStep <= 0) return (double) env.front() * env.front();
        // Внешний код, вручную сформировавший env без computeEnvelope, не
        // должен читать неинициализированный префикс: остаётся прежняя проба.
        if (envEnergyPrefix.size() != env.size() + 1 || to <= from)
        {
            const double v = envAt ((float) ((from + to) * 0.5));
            return v * v;
        }
        const auto integral = [this] (double x) noexcept {
            const double first = env.front(), last = env.back();
            if (x <= 0.0) return x * first * first;
            const size_t lastIndex = env.size() - 1;
            if (x >= (double) lastIndex)
                return envEnergyPrefix[lastIndex] + (x - (double) lastIndex) * last * last;
            const size_t i = (size_t) x;
            const double v = env[i];
            return envEnergyPrefix[i] + (x - (double) i) * v * v;
        };
        const double a = from / (double) envStep, b = to / (double) envStep;
        return std::max (0.0, (integral (b) - integral (a)) / (b - a));
    }
};

// тело записи = медиана огибающей по первым 60 % (атака и хвост не в счёт).
// Значение предвычислено в теле OneShot — в аудио-потоке здесь только чтение.
inline float bodyLevel (const OneShot& s) noexcept { return s.bodyRms; }

inline void computeEnvelope (OneShot& s, double sr = kSampleRate)
{
    s.envStep = (int) (sr * 0.01);
    const auto& l0 = s.levels[0];
    s.env.clear();
    s.envEnergyPrefix.clear();
    if (l0.empty() || s.envStep <= 0) return;
    s.env.reserve (l0.size() / (size_t) s.envStep + 1);
    for (size_t p = 0; p + (size_t) s.envStep <= l0.size(); p += (size_t) s.envStep)
    {
        double acc = 0.0;
        for (int i = 0; i < s.envStep; ++i) acc += (double) l0[p + (size_t) i] * l0[p + (size_t) i];
        s.env.push_back ((float) std::sqrt (acc / s.envStep));
    }

    // Если источник короче окна огибающей, не берём begin()+1 у пустого
    // вектора. Медиану заменить RMS всей короткой записи можно при сборке;
    // короткий источник не превращаем в фиктивную 10-мс огибающую.
    if (s.env.empty())
    {
        double acc = 0.0;
        for (float v : l0) acc += (double) v * v;
        s.bodyRms = std::max (1.0e-4f, (float) std::sqrt (acc / std::max<size_t> (1, l0.size())));
        return;
    }

    s.envEnergyPrefix.resize (s.env.size() + 1, 0.0);
    for (size_t i = 0; i < s.env.size(); ++i)
        s.envEnergyPrefix[i + 1] = s.envEnergyPrefix[i] + (double) s.env[i] * s.env[i];

    // медиана по первым 60 % — один раз, на сборке банка (не в аудио-потоке)
    const size_t n = std::max<size_t> (1, s.env.size() * 3 / 5);
    std::vector<float> tmp (s.env.begin(), s.env.begin() + (long) n);
    std::nth_element (tmp.begin(), tmp.begin() + (long) (tmp.size() / 2), tmp.end());
    s.bodyRms = std::max (1.0e-4f, tmp[tmp.size() / 2]);
}

// ---------------------------------------------------------------------------
// Генерация одного источника (все — моно, 110 Гц, 2 с)
// ---------------------------------------------------------------------------
inline void renderOneShot (OneShotKind kind, std::vector<float>& out, double sr, double dur)
{
    const int n = (int) (sr * dur);
    out.assign ((size_t) n, 0.0f);
    Rng rng (0x5EEDu ^ (uint32_t) kind);
    const double f0 = kSampleF0;
    const double TAUd = kTwoPi;

    switch (kind)
    {
        case OneShotKind::FmTine:
        {
            double p1 = 0, p2 = 0;
            for (int i = 0; i < n; ++i)
            {
                const double t = i / sr;
                const double idx = 3.2 * std::exp (-t * 5.0);
                const double env = std::exp (-t * 2.6) * (1.0 - std::exp (-t * 900.0));
                p2 += f0 * 3.0 / sr;  if (p2 >= 1) p2 -= 1;
                p1 += f0 / sr;        if (p1 >= 1) p1 -= 1;
                out[(size_t) i] = (float) (std::sin (TAUd * p1 + idx * std::sin (TAUd * p2)) * env);
            }
            break;
        }
        case OneShotKind::Karplus:
        {
            const int L = (int) std::lround (sr / f0);
            std::vector<float> buf ((size_t) L);
            for (auto& v : buf) v = rng.nextBipolar();
            // сглаживаем начальный шум (иначе «металлический» щелчок)
            for (int k = 1; k < L; ++k) buf[(size_t) k] = 0.5f * (buf[(size_t) k] + buf[(size_t) k - 1]);
            int idx = 0;
            const float decay = 0.9975f;              // T60 ~ 1.1 c
            for (int i = 0; i < n; ++i)
            {
                const float a = buf[(size_t) idx];
                const float b = buf[(size_t) ((idx + 1) % L)];
                out[(size_t) i] = a;
                buf[(size_t) idx] = 0.5f * (a + b) * decay;
                idx = (idx + 1) % L;
            }
            break;
        }
        case OneShotKind::Choir:
        {
            const float F[3] = { 700.0f, 1220.0f, 2600.0f };
            const float G[3] = { 1.0f, 0.55f, 0.28f };
            for (int i = 0; i < n; ++i)
            {
                const double t = i / sr;
                const double vib = 1.0 + 0.004 * std::sin (TAUd * 5.2 * t + 0.7);
                const double env = std::min (1.0, t * 6.0) * std::exp (-t * 1.1);
                float s = 0;
                for (int k = 1; k <= 24; ++k)
                {
                    const float hz = (float) (k * f0 * vib);
                    float w = 0;
                    for (int fi = 0; fi < 3; ++fi)
                    {
                        const float d = (hz - F[fi]) / (F[fi] * 0.3f);
                        w += std::exp (-d * d) * G[fi];
                    }
                    s += w * (float) std::sin (TAUd * k * f0 * vib * t) / std::pow ((float) k, 0.25f);
                }
                out[(size_t) i] = (float) (s * env * 0.35);
            }
            break;
        }
        case OneShotKind::GlassHit:
        {
            const double R[4] = { 1.0, 2.76, 5.40, 8.93 };
            const double D[4] = { 2.2, 4.0, 6.5, 9.0 };
            for (int i = 0; i < n; ++i)
            {
                const double t = i / sr;
                double s = 0;
                for (int m = 0; m < 4; ++m)
                    s += std::sin (TAUd * f0 * R[m] * t) * std::exp (-t * D[m]) / (1.0 + m);
                // шумовой удар
                s += rng.nextBipolar() * std::exp (-t * 120.0) * 0.5;
                out[(size_t) i] = (float) (s * 0.6);
            }
            break;
        }
        case OneShotKind::TapePad:
        {
            double ph = 0;
            float lp = 0.0f, wowPh = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const double t = i / sr;
                wowPh += 0.35f / (float) sr;
                const double wow = 1.0 + 0.006 * std::sin (TAUd * wowPh);
                const double env = std::min (1.0, t * 3.0) * std::exp (-t * 0.5);
                ph += f0 * wow / sr; if (ph >= 1) ph -= 1;
                float saw = (float) (2.0 * ph - 1.0);
                const float cut = 0.06f;
                lp += (saw - lp) * cut;
                out[(size_t) i] = (float) (lp * env * 1.6);
            }
            break;
        }
        case OneShotKind::WoodKnock:
        {
            // Две группы мод: быстрые (удар) и медленные (корпус). Раньше были
            // только быстрые, и после 0.5 с запись уходила в полную тишину:
            // медиана огибающей = 0, а грануляр и organics, читающие хвост,
            // выдавали почти тишину с редкими щелчками. Медленные моды дают
            // тело, которое слышно до конца двух секунд.
            const double R[3] = { 1.0, 2.9, 5.2 };
            const double D[3] = { 14.0, 25.0, 40.0 };
            const double RS[3] = { 0.5, 1.4, 2.3 };
            const double DS[3] = { 2.2, 3.1, 4.4 };
            double lp = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double t = i / sr;
                double s = rng.nextBipolar() * std::exp (-t * 260.0) * 0.8;
                for (int m = 0; m < 3; ++m)
                    s += std::sin (TAUd * f0 * 4.0 * R[m] * t) * std::exp (-t * D[m]) * 0.35;
                // медленное тело: те же моды ниже + отфильтрованный шумовой «стук»
                for (int m = 0; m < 3; ++m)
                    s += std::sin (TAUd * f0 * 4.0 * RS[m] * t + m) * std::exp (-t * DS[m]) * 0.22;
                lp += (rng.nextBipolar() - lp) * 0.08;
                s += lp * std::exp (-t * 3.0) * 1.6;
                out[(size_t) i] = (float) s;
            }
            break;
        }
        case OneShotKind::VoxAh:
        {
            const float F[3] = { 730.0f, 1090.0f, 2440.0f };
            const float G[3] = { 1.0f, 0.6f, 0.3f };
            for (int i = 0; i < n; ++i)
            {
                const double t = i / sr;
                const double drift = 1.0 + 0.002 * std::sin (TAUd * 0.7 * t);
                const double env = std::min (1.0, t * 8.0) * std::exp (-t * 1.4);
                float s = 0;
                for (int k = 1; k <= 30; ++k)
                {
                    const float hz = (float) (k * f0);
                    float w = 0;
                    for (int fi = 0; fi < 3; ++fi)
                    {
                        const float d = (hz - F[fi]) / (F[fi] * 0.26f);
                        w += std::exp (-d * d) * G[fi];
                    }
                    s += w * (float) std::sin (TAUd * k * f0 * drift * t) / std::pow ((float) k, 0.2f);
                }
                out[(size_t) i] = (float) (s * env * 0.3);
            }
            break;
        }
        case OneShotKind::SubBass:
        {
            double ph = 0;
            for (int i = 0; i < n; ++i)
            {
                const double t = i / sr;
                const double f = f0 * (1.0 + 0.5 * std::exp (-t * 30.0));   // питч-дроп
                ph += f / sr; if (ph >= 1) ph -= 1;
                const double env = std::min (1.0, t * 200.0) * std::exp (-t * 1.2);
                out[(size_t) i] = (float) (std::sin (TAUd * ph) * env);
            }
            break;
        }
    }

    // Калибровка: целимся в RMS ~0.25, но не даём пику выйти за 0.95.
    // Перкуссивные источники (Wood Knock) при простой нормализации по пику
    // оказывались на 11 дБ тише тональных — это слышно как «провал» при
    // переключении источника.
    float peak = 1.0e-9f; double acc = 0.0;
    for (auto v : out) { peak = std::max (peak, std::fabs (v)); acc += (double) v * v; }
    const double rms = std::sqrt (acc / (double) out.size());
    const float g = (float) std::min (0.95 / (double) peak, 0.25 / std::max (1e-9, rms));
    for (auto& v : out) v *= g;
}

// ---------------------------------------------------------------------------
// Построение MIP-уровней: каскад 2×-децимаций с полуполосным КИХ.
// level 0 — исходник, level 1 — полоса SR/4, level 2 — SR/8, level 3 — SR/16.
// ---------------------------------------------------------------------------
// Поиск точки петли: перебираем старты и длины, кратные периоду основного
// тона, и берём вариант с лучшей корреляцией формы волны на стыке. Это то,
// что делают сэмплеры («auto-loop»); без этого у записей с вибрато склейка
// давала боковые полосы, а у затухающих — пульсацию уровня.
inline void findLoopPoints (OneShot& s, double sr)
{
    if (s.levels.empty() || s.env.empty()) { s.loopStart = 0; s.loopEnd = (int) s.lens[0]; return; }
    const auto& d = s.levels[0];
    const int n = (int) d.size();
    const int Pr = std::max (32, (int) std::lround (sr / std::max (20.0, (double) midiToHz (s.rootNote))));
    float peak = 1.0e-4f;
    for (float v : s.env) peak = std::max (peak, v);

    const int kList[] = { 2, 4, 8, 16, 32, 64, 96 };
    float bestScore = -1.0e9f;
    int bestS = 0, bestL = Pr * 8;

    for (int st = Pr; st < n / 2; st += Pr)
    {
        if (s.envAt ((float) st) < peak * 0.2f) continue;
        for (int k : kList)
        {
            const int L = k * Pr;
            if (st + L + Pr >= n - 64) break;
            double num = 0.0, aa = 0.0, bb = 0.0;
            for (int i = 0; i < Pr; ++i)
            {
                const double a = d[(size_t) (st + i)], b = d[(size_t) (st + L + i)];
                num += a * b; aa += a * a; bb += b * b;
            }
            const float corr = (float) (num / std::sqrt (std::max (1.0e-12, aa * bb)));
            const float decay = s.envAt ((float) (st + L)) / std::max (1.0e-4f, s.envAt ((float) st));
            const float score = corr - 0.2f * std::fabs (std::log (std::max (0.02f, decay)))
                              + 0.05f * std::log2 ((float) k);
            if (score > bestScore) { bestScore = score; bestS = st; bestL = L; }
        }
    }
    s.loopStart = bestS;
    s.loopEnd = bestS + bestL;
}

inline void buildMipLevels (OneShot& os, double sr)
{
    os.levels.resize (1);
    os.lens.assign (1, (int) os.levels[0].size());

    for (int l = 1; l < kSampleLevels; ++l)
    {
        const std::vector<float>& in = os.levels.back();
        const size_t outCount = in.size() / 2;
        if (outCount < (size_t) kMinMipLength) break;
        std::vector<float> outv;
        outv.reserve (outCount);

        using Decimator = HalfbandDecimator2x<255, 220>;
        Decimator dec;                        // строится вне аудио-потока
        dec.reset();
        // decimate() центрирует 255 отводов в кольце на x[n-128].
        // Снимаем выходы на чётных n и пропускаем 64 выходных значения:
        // уровень L[n] тогда соответствует входу L-1[2n], без фазового сдвига.
        // Для последних значений фильтр дополняется нулями, длина остаётся
        // ровно floor(in.size()/2), а сопоставление времени уровней сохраняется.
        constexpr int delay = Decimator::kSize - 1 - Decimator::kHalf;
        static_assert ((delay & 1) == 0, "decimator alignment requires even delay");
        const size_t firstOutput = (size_t) delay;
        const size_t inputCount = 2 * outCount + firstOutput;
        for (size_t i = 0; i < inputCount; ++i)
        {
            dec.push (i < in.size() ? in[i] : 0.0f);
            if ((i & 1u) == 0 && i >= firstOutput)
                outv.push_back (dec.decimate());
        }
        os.lens.push_back ((int) outv.size());
        os.levels.push_back (std::move (outv));
    }
    computeEnvelope (os, sr);
    findLoopPoints (os, sr);
}

inline OneShot buildOneShot (OneShotKind kind, double sr = kSampleRate, double dur = 2.0)
{
    OneShot os;
    os.name = oneShotName (kind);
    os.rootNote = 45;                                  // A2 = 110 Гц
    os.levels.resize (1);
    renderOneShot (kind, os.levels[0], sr, dur);
    buildMipLevels (os, sr);
    return os;
}

// ---------------------------------------------------------------------------
// ПРОБНЫЙ источник — для измерения качества движков, а не для музыки.
//
// Все «музыкальные» источники имеют вибрато, биения и нестационарную
// огибающую. Метрика алиасинга на них не работает: сайдбенды вибрато
// (при питче +3 октавы вибрато 5 Гц превращается в 40 Гц) попадают
// в промежутки между гармониками и считаются алиасингом — измерялось
// «-17 дБ» у идеально чистого спектра.
//
// Пробник — строго стационарная полоса: 40 гармоник 110 Гц, амплитуда 1/k,
// без вибрато и без огибающей. Всё, что появится выше верхней гармоники
// (4.4 кГц, умноженное на питч-рейт), — ошибка движка.
// ---------------------------------------------------------------------------
inline OneShot buildProbeShot (double sr = kSampleRate, double dur = 4.0, double bandHz = 4400.0)
{
    OneShot os;
    os.name = "Probe";
    os.rootNote = 45;
    const int n = (int) (sr * dur);
    os.levels.resize (1);
    std::vector<float>& src = os.levels[0];
    src.resize ((size_t) n);
    const int kMax = (int) (bandHz / kSampleF0);
    for (int i = 0; i < n; ++i)
    {
        double acc = 0.0;
        for (int k = 1; k <= kMax; ++k)
            acc += std::sin (kTwoPi * k * kSampleF0 * i / sr) / k;
        src[(size_t) i] = (float) (0.45 * acc);
    }
    buildMipLevels (os, sr);
    return os;
}

// ---------------------------------------------------------------------------
class SampleBank
{
public:
    std::vector<OneShot> shots;

    void buildFactory (double sr = kSampleRate)
    {
        shots.clear();
        for (auto k : { OneShotKind::FmTine, OneShotKind::Karplus, OneShotKind::Choir,
                        OneShotKind::GlassHit, OneShotKind::TapePad, OneShotKind::WoodKnock,
                        OneShotKind::VoxAh, OneShotKind::SubBass })
            shots.push_back (buildOneShot (k, sr));
    }

    bool save (const std::string& path) const
    {
        FILE* f = std::fopen (path.c_str(), "wb");
        if (! f) return false;
        std::fwrite ("OROSMP1", 1, 7, f);
        const uint32_t ns = (uint32_t) shots.size();
        std::fwrite (&ns, 4, 1, f);
        for (const auto& s : shots)
        {
            const uint32_t lv = (uint32_t) s.lens.size(), root = (uint32_t) s.rootNote;
            std::fwrite (&lv, 4, 1, f);
            std::fwrite (&root, 4, 1, f);
            for (size_t l = 0; l < s.lens.size(); ++l)
            {
                const uint32_t len = (uint32_t) s.lens[l];
                std::fwrite (&len, 4, 1, f);
                std::fwrite (s.levels[l].data(), sizeof (float), len, f);
            }
            const uint32_t nl = (uint32_t) s.name.size();
            std::fwrite (&nl, 4, 1, f);
            std::fwrite (s.name.data(), 1, nl, f);
        }
        std::fclose (f);
        return true;
    }

    bool load (const std::string& path)
    {
        FILE* f = std::fopen (path.c_str(), "rb");
        if (! f) return false;
        char magic[7] = {};
        if (std::fread (magic, 1, 7, f) != 7 || std::memcmp (magic, "OROSMP1", 7) != 0)
        { std::fclose (f); return false; }
        uint32_t ns = 0;
        std::fread (&ns, 4, 1, f);
        shots.assign (ns, {});
        for (auto& s : shots)
        {
            uint32_t lv = 0, root = 0;
            std::fread (&lv, 4, 1, f);
            std::fread (&root, 4, 1, f);
            s.rootNote = (int) root;
            s.levels.resize (lv); s.lens.resize (lv);
            for (uint32_t l = 0; l < lv; ++l)
            {
                uint32_t len = 0;
                std::fread (&len, 4, 1, f);
                s.lens[l] = (int) len;
                s.levels[l].resize (len);
                std::fread (s.levels[l].data(), sizeof (float), len, f);
            }
            uint32_t nl = 0;
            std::fread (&nl, 4, 1, f);
            s.name.resize (nl);
            std::fread (s.name.data(), 1, nl, f);
            computeEnvelope (s);              // огибающая в файл не пишется: считается заново
        }
        std::fclose (f);
        return true;
    }
};

} // namespace orogen
