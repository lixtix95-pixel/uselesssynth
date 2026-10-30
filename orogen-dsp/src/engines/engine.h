// Общий интерфейс движка. Один движок = один слот осциллятора в голосе.
// Контракт:
//   start()   — note-on: сброс фаз/возбуждение мод (НЕ на каждый блок)
//   prepare() — тяжёлые расчёты: коэффициенты, MIP-уровень, инкременты
//   render()  — горячий цикл: ровно один сэмпл, без делений/аллокаций/библиотечных вызовов
#pragma once

#include "../core/dsp.h"

namespace orogen {

// Параметры, которые голос передаёт движку. POD — копируется на блок,
// движок не знает ничего про JUCE/APVTS.
struct EngineParams
{
    float p[4]     = { 0.3f, 0.2f, 0.0f, 0.3f };  // нормализованные 0..1, смысл зависит от движка
    float level    = 1.0f;                        // 0..1
    float pan      = 0.0f;                        // -1..1
    float tuneSemis = 0.0f;                       // полутоны
    float detune   = 0.3f;                        // 0..1 -> разброс в центах, свой на голос
    int   table    = 0;                           // выбор контента (таблица/источник/тело)
    bool  on       = true;
    float grainReverse = 0.0f;                    // Granular: вероятность реверса нового зерна, 0..1
};

class EngineBase
{
public:
    virtual ~EngineBase() = default;

    virtual const char* name() const = 0;
    virtual void setSampleRate (double sr) = 0;
    // Устойчивое значение ±1 для данной ноты/голоса. По умолчанию движки,
    // которым это не нужно, его игнорируют; параметр не является ID хоста.
    virtual void setNoteRandom (float) noexcept {}
    virtual void start (int midiNote, float velocity) = 0;
    virtual void prepare (const EngineParams& p) = 0;
    virtual float render (const EngineParams& p) = 0;
    virtual void stop() = 0;

    // Средняя «стоимость» движка — нужна для voice stealing и бюджета CPU.
    virtual float costFactor() const { return 1.0f; }
};

} // namespace orogen
