// Фильтры слотов: 16 моделей вместо прежних пяти.
//
// В Terrain заявлено 123 фильтра. Это не 123 разных алгоритма, а набор
// топологий × параметры × режимы (у большинства «карточек» различаются только
// частота/резонанс/наклон). Здесь собраны сами топологии, из которых такой
// набор и складывается: 6/12/24/36 дБ на октаву в обе стороны, полосовые,
// режекторные, пиковый, фазовый, лестница (Moog-style), гребёнка и форманты.
//
// ВАЖНО про совместимость: идентификаторы 0…4 намеренно оставлены прежними
// (LP12, LP24, BP, HP24, Notch12) — старые патчи и пресеты звучат как раньше.
// Единственное осознанное изменение: HP24 раньше давал наклон 12 дБ/окт
// (формула x − k·v1 − v2 — это фильтр первой пары полюсов), теперь это
// настоящие 24 дБ/окт. В фабричных патчах этот тип не используется.
#pragma once

#include "dsp.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace orogen {

enum class FilterModel : int
{
    LP12 = 0, LP24 = 1, BP12 = 2, HP24 = 3, Notch12 = 4,   // исторические id
    LP6, HP6, HP12, BP24, Notch24, Peak, AllPass,          // 5…11
    LP36, Ladder, Comb, Vowel,                              // 12…15
    Count
};

inline const char* filterModelName (FilterModel m)
{
    switch (m)
    {
        case FilterModel::LP12:    return "LP12";
        case FilterModel::LP24:    return "LP24";
        case FilterModel::BP12:    return "BP12";
        case FilterModel::HP24:    return "HP24";
        case FilterModel::Notch12: return "Notch12";
        case FilterModel::LP6:     return "LP6";
        case FilterModel::HP6:     return "HP6";
        case FilterModel::HP12:    return "HP12";
        case FilterModel::BP24:    return "BP24";
        case FilterModel::Notch24: return "Notch24";
        case FilterModel::Peak:    return "Peak";
        case FilterModel::AllPass: return "AllPass";
        case FilterModel::LP36:    return "LP36";
        case FilterModel::Ladder:  return "Ladder";
        case FilterModel::Comb:    return "Comb";
        case FilterModel::Vowel:   return "Vowel";
        default:                   return "?";
    }
}

inline FilterModel filterModelFromInt (int v) noexcept
{
    const int n = (int) FilterModel::Count;
    return (FilterModel) (v < 0 ? 0 : (v >= n ? n - 1 : v));
}

// ---------------------------------------------------------------------------
// Один фильтр на слот голоса. Все состояния — внутри объекта, поэтому prepare()
// выделяет память один раз (в init() голоса), а process() не аллоцирует.
// ---------------------------------------------------------------------------
class SlotFilter
{
public:
    static constexpr int kCombMax = 2048;      // 42 мс при 48 кГц — минимум 23 Гц

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        combBuf.assign (kCombMax, 0.0f);
        reset();
    }

    void reset() noexcept
    {
        c1 = c2 = c3 = { 1.0e-20f, 1.0e-20f, 1.0e-20f, 1.0e-20f };
        op1 = op2 = op3 = op4 = 0.0f;
        ladderFb = 0.0f;
        f1 = f2 = f3 = { 1.0e-20f, 1.0e-20f, 1.0e-20f, 1.0e-20f };
        std::fill (combBuf.begin(), combBuf.end(), 0.0f);
        combIdx = 0;
    }

    // cutoff Hz, res 0…1 (0 — Q≈0.7, 1 — Q≈25 у SVF; у лестницы — самовозбуждение на грани)
    void setParams (float cutoffHz, float res) noexcept
    {
        fc = clamp (cutoffHz, 20.0f, (float) sr * 0.45f);
        q  = clamp (res, 0.0f, 0.98f);

        g  = std::tan ((float) kPi * fc / (float) sr);
        kc = 1.0f / (0.707f + q * q * 24.0f);
        // Каскад из N звеньев с одинаковыми коэффициентами перемножает
        // резонанс: при res 0.7 LP24 давал пик 155×, а LP36 — 1938×.
        // Поэтому добротность делится между звеньями:
        // Q_stage = 0.707 · (Q_total/0.707)^(1/N) — при нулевом резонансе звено
        // нейтрально (0.707), при высоком резонанс распределён.
        const float qTotal = 1.0f / kc;
        const float q2 = 0.707f * std::pow (qTotal / 0.707f, 0.5f);
        const float q3 = 0.707f * std::pow (qTotal / 0.707f, 1.0f / 3.0f);
        kc2 = 1.0f / q2;
        kc3 = 1.0f / q3;
        a1 = 1.0f / (1.0f + g * (g + kc));
        a2 = g * a1;
        a3 = g * a2;

        // однополюсный коэффициент (6 дБ/окт)
        gOne = 1.0f - std::exp (-(float) kTwoPi * fc / (float) sr);
        // Лестница — четыре однополюсника подряд. Частоту полюсов оставляем
        // равной fc (как у настоящей Moog-лестницы: −3 дБ всей цепи оказывается
        // ниже среза звена). Компенсация ×2.3 сдвигала резонанс на октаву — откатано.
        gLadder = gOne;

        // гребёнка: задержка = один период частоты среза
        combDelay = (int) clamp ((float) sr / fc, 4.0f, (float) (kCombMax - 2));
        // Обратная связь ограничена 0.9: при 0.97 резонансный пик достигал 28×
        // и гребёнка уходила в лимитер голоса.
        combFb = clamp (0.25f + 0.73f * q, 0.0f, 0.90f);

        // форманты: три полосовых со своими центрами и добротностями,
        // центры масштабируются частотой среза (как «vowel»-ручка в Terrain)
        formantCoef (clamp (fc * 1.00f,  60.0f, (float) sr * 0.45f), 5.0f + q * 9.0f,  fm[0]);
        formantCoef (clamp (fc * 2.20f, 100.0f, (float) sr * 0.45f), 7.0f + q * 13.0f, fm[1]);
        formantCoef (clamp (fc * 3.40f, 200.0f, (float) sr * 0.45f), 9.0f + q * 17.0f, fm[2]);
    }

    inline float process (float x, FilterModel m) noexcept
    {
        switch (m)
        {
            case FilterModel::LP12:    return svfOut (c1, x, Out::LP);
            case FilterModel::BP12:    return svfOut (c1, x, Out::BP);
            case FilterModel::Notch12: return svfOut (c1, x, Out::Notch);
            case FilterModel::Peak:    return svfOut (c1, x, Out::Peak);
            case FilterModel::AllPass: return svfOut (c1, x, Out::AllPass);

            case FilterModel::LP24:                     // два каскада
            {
                const float y = svfOut (c1, x, Out::LP, kc2);
                return svfOut (c2, y, Out::LP, kc2);
            }
            case FilterModel::HP12:    return svfOut (c1, x, Out::HP);
            case FilterModel::HP24:
            {
                const float y = svfOut (c1, x, Out::HP, kc2);
                return svfOut (c2, y, Out::HP, kc2);
            }
            case FilterModel::BP24:
            {
                const float y = svfOut (c1, x, Out::BP, kc2);
                return svfOut (c2, y, Out::BP, kc2);
            }
            case FilterModel::Notch24:
            {
                const float y = svfOut (c1, x, Out::Notch, kc2);
                return svfOut (c2, y, Out::Notch, kc2);
            }

            case FilterModel::LP6:
            {
                op1 += gOne * (x - op1);
                return op1;
            }
            case FilterModel::HP6:
            {
                op1 += gOne * (x - op1);
                return x - op1;
            }
            case FilterModel::LP36:                     // три каскада по 12 дБ
            {
                const float y1 = svfOut (c1, x,  Out::LP, kc3);
                const float y2 = svfOut (c2, y1, Out::LP, kc3);
                return svfOut (c3, y2, Out::LP, kc3);
            }
            case FilterModel::Ladder:  return ladder (x);

            case FilterModel::Comb:
            {
                const int n = (int) combBuf.size();
                const float d = combBuf[(size_t) ((combIdx - combDelay + n) % n)];
                combBuf[(size_t) combIdx] = x * (1.0f - combFb * 0.5f) + combFb * d;
                if (++combIdx >= n) combIdx = 0;
                return d;
            }

            case FilterModel::Vowel:
            {
                // три параллельных полосовых на формантах, веса как у голоса
                return 0.9f  * svfOutF (f1, fm[0], x)
                     + 0.55f * svfOutF (f2, fm[1], x)
                     + 0.30f * svfOutF (f3, fm[2], x);
            }

            default: return x;
        }
    }

private:
    enum class Out { LP, BP, HP, Notch, Peak, AllPass };

    struct Core { float ic1, ic2, ic1b, ic2b; };

    // один каскад TPT SVF (Zavalishin), коэффициенты общие на слот
    inline float svfOut (Core& c, float x, Out out, float kOverride = 0.0f) const noexcept
    {
        const float kx = kOverride > 0.0f ? kOverride : kc;
        const float a1x = 1.0f / (1.0f + g * (g + kx));
        const float a2x = g * a1x;
        const float a3x = g * a2x;
        const float v3 = x - c.ic2;
        const float v1 = a1x * c.ic1 + a2x * v3;
        const float v2 = c.ic2 + a2x * c.ic1 + a3x * v3;
        c.ic1 = 2.0f * v1 - c.ic1;
        c.ic2 = 2.0f * v2 - c.ic2;
        c.ic1 = antiDenormal (c.ic1);
        c.ic2 = antiDenormal (c.ic2);

        switch (out)
        {
            case Out::LP:      return v2;
            case Out::BP:      return v1;
            case Out::HP:      return x - kx * v1 - v2;
            case Out::Notch:   return x - kx * v1;
            case Out::Peak:    return 2.0f * v2 - x + kx * v1;
            case Out::AllPass: return x - 2.0f * kx * v1;
            default:           return v2;
        }
    }

    inline float ladder (float x) noexcept
    {
        // четыре однополюсных звена + нелинейная обратная связь: на высокой
        // резонансе сама себя ограничивает, выбросов не даёт
        const float fb = clamp (4.0f * q, 0.0f, 3.9f);
        // Компенсация потери низа: у лестницы с обратной связью НЧ падают как
        // 1/(1+k). Подмешиваем вход с коэффициентом (1 + k/2) — классический
        // приём, иначе на res 0.7 низ уходил в −12 дБ.
        const float u = fastTanh (x * (1.0f + 0.5f * fb) - fb * ladderFb);
        op1 += gLadder * (u  - op1);
        op2 += gLadder * (op1 - op2);
        op3 += gLadder * (op2 - op3);
        op4 += gLadder * (op3 - op4);
        ladderFb = op4;
        return op4;
    }

    // Коэффициенты отдельного SVF для произвольной частоты (форманты)
    struct Coefs { float a1 = 0, a2 = 0, a3 = 0, k = 0.7f; };

    void formantCoef (float hz, float resv, Coefs& c) const noexcept
    {
        const float gg = std::tan ((float) kPi * clamp (hz, 20.0f, (float) sr * 0.45f) / (float) sr);
        c.k  = 1.0f / std::max (0.05f, resv * 0.35f);
        c.a1 = 1.0f / (1.0f + gg * (gg + c.k));
        c.a2 = gg * c.a1;
        c.a3 = gg * c.a2;
    }

    inline float svfOutF (Core& c, const Coefs& k, float x) const noexcept
    {
        const float v3 = x - c.ic2;
        const float v1 = k.a1 * c.ic1 + k.a2 * v3;
        const float v2 = c.ic2 + k.a2 * c.ic1 + k.a3 * v3;
        c.ic1 = antiDenormal (2.0f * v1 - c.ic1);
        c.ic2 = antiDenormal (2.0f * v2 - c.ic2);
        return v1;
    }

    std::vector<float> combBuf;
    Core c1 { 0, 0, 0, 0 }, c2 { 0, 0, 0, 0 }, c3 { 0, 0, 0, 0 }, f1 { 0, 0, 0, 0 }, f2 { 0, 0, 0, 0 }, f3 { 0, 0, 0, 0 };
    float g = 0.1f, kc = 0.7f, kc2 = 0.84f, kc3 = 0.89f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, gOne = 0.1f, gLadder = 0.2f;
    float fc = 1000.0f, q = 0.0f;
    float op1 = 0.0f, op2 = 0.0f, op3 = 0.0f, op4 = 0.0f, ladderFb = 0.0f;
    float combFb = 0.5f;
    int combDelay = 100, combIdx = 0;
    Coefs fm[3];
    double sr = 48000.0;
};

} // namespace orogen
