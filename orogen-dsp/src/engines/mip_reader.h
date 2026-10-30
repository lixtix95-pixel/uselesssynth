// Чтение one-shot'а через MIP-уровни — общее для движков Sample и Granular.
//
// Задача одна и та же у обоих: питч-рейт задаётся пользователем в реальном
// времени, поэтому полосу приходится сужать на лету. Здесь собрано всё, что
// было выяснено замерами при доводке движка Sample:
//   • позиция живёт в шкале ИСХОДНИКА, уровень — только выбор таблицы;
//   • каждый выровненный уровень строится прореживанием 2:1: позиция в нём
//     равна posOrig / 2^L. Округление длины не меняет частоту дискретизации;
//   • чтение — полифазный windowed-sinc (см. sinc_interp.h);
//   • переход к следующему уровню завершается вскоре после порога 2^L,
//     чтобы при дробной скорости широкополосный уровень не давал алиасинг.
#pragma once

#include "sinc_interp.h"
#include "../content/sample_bank.h"

namespace orogen {

struct MipReader
{
    const OneShot* shot  = nullptr;
    const float*   dataA = nullptr;
    const float*   dataB = nullptr;
    int            lenOrig = 1, lenA = 1, lenB = 1;
    float          invScaleA = 1.0f, invScaleB = 1.0f;
    float          wA = 1.0f, wB = 0.0f;
    // -1 — обычное чтение; 0 — Granular с обычным sinc; 1..16 —
    // Granular с полосовым sinc для дробной скорости внутри октавы.
    int            grainBin = -1;

    void setup (const OneShot& s, float rate) noexcept
    {
        grainBin = -1;
        shot = &s;
        lenOrig = s.lens[0];

        float lvlF = (rate > 1.0f) ? std::log2 (rate) : 0.0f;
        lvlF = clamp (lvlF, 0.0f, (float) (s.lens.size() - 1));
        const int l0 = (int) lvlF;
        const int l1 = std::min ((int) s.lens.size() - 1, l0 + 1);
        // При прежнем переходе на всю октаву, например rate=24 содержал
        // ~42 % уровня для rate=16. Его полоса выше нового Найквиста и тон
        // заворачивался без заметного подавления. Переход завершается за 1/8
        // октавы: сохраняется непрерывность, но опасный уровень быстро уходит.
        const float t = l0 == l1 ? 0.0f : clamp ((lvlF - (float) l0) * 8.0f, 0.0f, 1.0f);

        wA = 1.0f - t;
        wB = t;
        lenA = s.lens[(size_t) l0];
        lenB = s.lens[(size_t) l1];
        // L[n] получен из L-1[2n]. Укорочение нечётной длины на один
        // отсчёт не является изменением sample rate и не должно менять pitch.
        invScaleA = std::ldexp (1.0f, -l0);
        invScaleB = std::ldexp (1.0f, -l1);
        dataA = s.levels[(size_t) l0].data();
        dataB = s.levels[(size_t) l1].data();
    }

    // Отдельный режим Granular: берём НИЖНИЙ MIP и фильтруем дробный
    // остаток скорости полифазным sinc. Это сохраняет полосу до 14.4 кГц
    // там, где чтение следующего MIP раньше отрезало её целиком. false означает,
    // что для данного источника/скорости безопасного уровня не существует.
    bool setupGranular (const OneShot& s, float rate) noexcept
    {
        if (! std::isfinite (rate) || rate <= 0.0f || s.lens.empty()) return false;
        shot = &s;
        lenOrig = s.lens[0];
        const float lvlF = rate > 1.0f ? std::log2 (rate) : 0.0f;
        const int level = std::min ((int) s.lens.size() - 1, (int) lvlF);
        invScaleA = invScaleB = std::ldexp (1.0f, -level);
        const float q = rate * invScaleA;
        // Если банк закончился до требуемого уровня, фильтра до 2x мало.
        if (q > 2.0f + 1.0e-5f) return false;
        lenA = lenB = s.lens[(size_t) level];
        dataA = dataB = s.levels[(size_t) level].data();
        wA = 1.0f; wB = 0.0f;
        grainBin = q <= 1.0f + 1.0e-6f ? 0
            : std::min (GrainSincKernel::kRateBins,
                        std::max (1, (int) std::ceil ((q - 1.0f) * GrainSincKernel::kRateBins)));
        return true;
    }

    int grainHalf() const noexcept
    {
        return grainBin > 0 ? GrainSincKernel::kHalf : SincKernel::kHalf;
    }

    // позиция в шкале исходника; обычный режим допускает петлю с заворотом,
    // Granular гарантирует границы ещё при запуске зерна.
    inline float read (double posOrig) const noexcept
    {
        if (grainBin >= 0)
        {
            // Для Granular сохраняем дробную часть позиции ДО отделения
            // целого индекса. Преобразование в float всей координаты снова
            // квантовало бы дробь на крупных/поздних позициях записи.
            const double p = posOrig * (double) invScaleA;
            const int i1 = (int) p;
            const float frac = (float) (p - (double) i1);
            return grainBin == 0 ? sincKernel().apply (dataA, lenA, i1, frac)
                                 : grainSincKernel().apply (dataA, lenA, i1, frac, grainBin);
        }
        // Остальные движки используют прежний float-путь для петли/MIP.
        if (wA <= 0.001f) return readLevel (dataB, lenB, (float) (posOrig * invScaleB));
        const float sA = readLevel (dataA, lenA, (float) (posOrig * invScaleA));
        if (wB <= 0.001f) return sA;
        const float sB = readLevel (dataB, lenB, (float) (posOrig * invScaleB));
        return sA * wA + sB * wB;
    }

private:
    static inline float readLevel (const float* d, int len, float p) noexcept
    {
        const float span = (float) (len - 8);
        p = std::fmod (p, span);
        if (p < 0.0f) p += span;
        const int i1 = (int) p;
        return sincKernel().apply (d, len, i1, p - (float) i1);
    }
};

} // namespace orogen
