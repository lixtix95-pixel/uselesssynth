// Подписи четырёх общих параметров слота. Порядок движков совпадает с
// EngineType/списком sX_type; смысл сверяется с prepare() каждого движка.
// ID P1…P4 в хосте не меняются — здесь только контекстные имена для редактора.
#pragma once

namespace orogen
{
inline const char* engineMacroName (int engine, int macro) noexcept
{
    static constexpr const char* labels[9][4] =
    {
        { "POSITION", "WARP",   "FOLD",     "TILT" },       // Wavetable
        { "RATIO",    "FM 1",   "FM 2",     "FEEDBACK" },   // FM
        { "TILT",     "SHAPE",  "SPREAD",   "MOTION" },     // Harmonic
        { "SCAN",     "GRAIN",  "TONE",     "AIR" },        // Sample
        { "POSITION", "SIZE",   "DENSITY",  "SPREAD" },     // Granular
        { "BRIGHT",   "DECAY",  "HARDNESS", "NONLINEAR" },  // Modal
        { "STRETCH",  "PITCH",  "TILT",     "DRIFT" },      // Resynth
        { "BODY",     "AIR",    "DRIFT",    "BLOOM" },      // Organics
        { "SLICE",    "LENGTH", "REPEATS",  "REVERSE" }     // Chop
    };
    return engine >= 0 && engine < 9 && macro >= 0 && macro < 4
               ? labels[engine][macro] : "?";
}

// Общий host ID table сохранён для всех типов; некоторые движки его не читают,
// CHOP ограничивает индекс до 0…2. Не выдаём фиктивный банк за работающий.
inline const char* engineSourceControlName (int engine) noexcept
{
    switch (engine)
    {
        case 0: return "TABLE 0-7";
        case 1: case 2: return "UNUSED";
        case 3: case 4: case 6: case 7: return "SOURCE 0-7";
        case 5: return "MODEL 0-7";
        case 8: return "LOOP 0-2; 3+ = 2";
        default: return "?";
    }
}
}
