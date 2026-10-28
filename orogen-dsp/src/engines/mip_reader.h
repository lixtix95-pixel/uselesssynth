// Чтение one-shot'а через MIP-уровни — общее для движков Sample и Granular.
//
// Задача одна и та же у обоих: питч-рейт задаётся пользователем в реальном
// времени, поэтому полосу приходится сужать на лету. Здесь собрано всё, что
// было выяснено замерами при доводке движка Sample:
//   • позиция живёт в шкале ИСХОДНИКА, уровень — только выбор таблицы;
//   • масштаб считается по фактическим длинам уровней (lenA/lenOrig),
//     а не по 2^L — иначе расстройка 0.2 % и расползание основного тона;
//   • чтение — полифазный windowed-sinc (см. sinc_interp.h);
//   • кроссфейд между двумя соседними уровнями по log2(rate).
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

    void setup (const OneShot& s, float rate) noexcept
    {
        shot = &s;
        lenOrig = s.lens[0];

        float lvlF = (rate > 1.0f) ? std::log2 (rate) : 0.0f;
        lvlF = clamp (lvlF, 0.0f, (float) (s.lens.size() - 1) - 0.001f);
        const int l0 = (int) lvlF;
        const int l1 = std::min ((int) s.lens.size() - 1, l0 + 1);
        const float t = lvlF - (float) l0;

        wA = 1.0f - t;
        wB = t;
        lenA = s.lens[(size_t) l0];
        lenB = s.lens[(size_t) l1];
        invScaleA = (float) lenA / (float) lenOrig;
        invScaleB = (float) lenB / (float) lenOrig;
        dataA = s.levels[(size_t) l0].data();
        dataB = s.levels[(size_t) l1].data();
    }

    // позиция в шкале исходника; читает с заворотом (для гранул и петли)
    inline float read (float posOrig) const noexcept
    {
        const float sA = readLevel (dataA, lenA, posOrig * invScaleA);
        if (wB <= 0.001f) return sA;
        const float sB = readLevel (dataB, lenB, posOrig * invScaleB);
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
