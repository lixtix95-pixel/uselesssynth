// CHORUS из плагина MonoJuice 0.9.5 — статически транслированная процедура
// прошивки OS 1.32B (не эмулятор: нет декодера инструкций, JIT и загрузчика).
//
//   third_party/monojuice/dsp/ChorusCore.{h,cpp}  — само ядро, 24-битная
//        арифметика, 16 кадров за вызов (process16)
//   third_party/monojuice/dsp/ChorusTables.h      — таблицы процедуры
//   third_party/monojuice/dsp/AudioAdapter.{h,cpp} — обвязка автора плагина:
//        ядро всегда работает на 44.1 кГц, при другой частоте — 64-отводная
//        оконная sinc-конверсия; сглаживание параметров ~20 мс
//
// Всё скопировано без правок. Наша обвязка добавляет только внешний микс:
// внутренний MIX прошивки всегда 127 (полностью обработанный сигнал), а dry
// задерживается на latency() отсчётов — иначе dry/wet складывались бы со
// сдвигом и давали гребёнку.
//
// Параметры ядра (0…127, как на панели): DEL / DEP / SPD / MIX / FB / WID / LP / INP.
// Стоимость: 0.9 % ядра (стерео, 48 кГц).
#pragma once

#include "../../third_party/monojuice/dsp/AudioAdapter.h"
#include "../core/dsp.h"

#include <array>
#include <vector>

namespace orogen
{

class FirmwareChorusFx
{
public:
    enum Param { DEL = 0, DEP = 1, SPD = 2, MIX = 3, FB = 4, WID = 5, LP = 6, INP = 7 };

    void prepare (double sr)
    {
        latencyFrames = adapter.prepare (sr);
        adapter.setSmoothing (true);
        adapter.setParameters (params);
        dryL.assign ((size_t) latencyFrames + 8, 0.0f);
        dryR.assign ((size_t) latencyFrames + 8, 0.0f);
        write = 0;
    }

    int latency() const { return latencyFrames; }

    void setParameters (const std::array<int, 8>& p)
    {
        params = p;
        params[(size_t) MIX] = 127;         // микс считаем снаружи
        for (auto& v : params) v = clamp (v, 0, 127);
        adapter.setParameters (params);
    }

    void setParameter (int index, int value)
    {
        if (index < 0 || index > 7) return;
        auto p = params;
        p[(size_t) index] = clamp (value, 0, 127);
        setParameters (p);
    }

    void setMix (float m) { mix = clamp (m, 0.0f, 1.0f); }

    inline void process (float& l, float& r) noexcept
    {
        if (dryL.empty()) return;

        const int size = (int) dryL.size();
        dryL[(size_t) write] = l;
        dryR[(size_t) write] = r;
        int rd = write - latencyFrames;
        if (rd < 0) rd += size;
        const float dryLv = dryL[(size_t) rd];
        const float dryRv = dryR[(size_t) rd];
        if (++write >= size) write = 0;

        float wl = l, wr = r;
        adapter.processFrame (l, r, wl, wr, false);

        l = dryLv + (wl - dryLv) * mix;
        r = dryRv + (wr - dryRv) * mix;
        if (isBad (l)) l = 0.0f;
        if (isBad (r)) r = 0.0f;
    }

private:
    nova::AudioAdapter adapter;
    std::array<int, 8> params = nova::ChorusCore::defaults;
    std::vector<float> dryL, dryR;
    int write = 0, latencyFrames = 16;
    float mix = 0.0f;
};

} // namespace orogen
