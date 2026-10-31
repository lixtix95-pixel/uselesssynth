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
// Mode 0 сохраняет прежнее поведение OROGEN. Mode 1/V1 использует
// внутренний MIX, mode 2/V2 — внешний MIX и линейное сглаживание 20 мс.
// В V1/V2 включена защита MonoJuice 0.9.8 для границы DEL=0.
// Внешний dry задерживается на latency() отсчётов, чтобы совпасть с wet.
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
        adapter.setDelayZeroGuard (mode != 0);
        adapter.setSmoothing (mode != 1);
        pushParameters();
        dryL.assign ((size_t) latencyFrames + 8, 0.0f);
        dryR.assign ((size_t) latencyFrames + 8, 0.0f);
        write = 0;
        mixStep = 1.0f / (float) std::max (1.0, sr * 0.020);
        mixCurrent = mix;
        processed = false;
    }

    int latency() const { return latencyFrames; }

    // 0 = old OROGEN; 1 = MonoJuice V1; 2 = MonoJuice V2.
    void setMode (int value)
    {
        const int next = clamp (value, 0, 2);
        if (mode == next) return;
        mode = next;
        adapter.setDelayZeroGuard (mode != 0);
        adapter.setSmoothing (mode != 1);
        mixCurrent = mix;
        pushParameters();
    }

    void setParameters (const std::array<int, 8>& p)
    {
        params = p;
        for (auto& v : params) v = clamp (v, 0, 127);
        pushParameters();
    }

    void setParameter (int index, int value)
    {
        if (index < 0 || index > 7) return;
        auto p = params;
        p[(size_t) index] = clamp (value, 0, 127);
        setParameters (p);
    }

    void setMix (float m)
    {
        const float next = clamp (m, 0.0f, 1.0f);
        if (mix == next) return;
        mix = next;
        if (! processed || mode != 2) mixCurrent = mix;
        if (mode == 1) pushParameters();
    }

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

        processed = true;
        if (mode == 1) { l = wl; r = wr; }
        else
        {
            if (mode == 2) mixCurrent += clamp (mix - mixCurrent, -mixStep, mixStep);
            else mixCurrent = mix;
            l = dryLv + (wl - dryLv) * mixCurrent;
            r = dryRv + (wr - dryRv) * mixCurrent;
        }
        if (isBad (l)) l = 0.0f;
        if (isBad (r)) r = 0.0f;
    }

private:
    void pushParameters()
    {
        auto p = params;
        p[(size_t) MIX] = mode == 1 ? clamp ((int) std::lround (mix * 127.0f), 0, 127) : 127;
        adapter.setParameters (p);
    }

    nova::AudioAdapter adapter;
    std::array<int, 8> params = nova::ChorusCore::defaults;
    std::vector<float> dryL, dryR;
    int write = 0, latencyFrames = 16;
    float mix = 0.0f, mixCurrent = 0.0f, mixStep = 1.0f;
    int mode = 0;
    bool processed = false;
};

} // namespace orogen
