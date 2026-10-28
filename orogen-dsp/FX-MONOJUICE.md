# FX из плагина MonoJuice 0.9.5: что взято и с какими цифрами

> **Лицензии.** Всё, включая GPL-3.0-части (COMB), взято: проект не продаётся,
> поэтому OROGEN распространяется на условиях GPL-3.0-or-later (`LICENSE`,
> `NOTICE.md`). Раздел ниже про «нельзя взять» оставлен как история решения.

Взят архив `MonoJuice_FX_Analyzer` (45 файлов, 776 КБ) — авторский плагин с пятью
FX-слотами. Ниже — разбор по критериям, которые уже применялись к списку с
GitHub: лицензия, зависимости (JUCE), стыкуемость с ядром OROGEN.

## 1. Отбор

| что в плагине | лицензия | зависимости | вердикт |
|---|---|---|---|
| `MaterialResonator.h` — модальный резонатор, 4 профиля | код автора | нет (C++17) | **взят** |
| `GlitchCore.h` — статтер/глитч | код автора | нет (JUCE только опционально) | **взят** |
| `Spectre.h` — MDCT-маска по пикам | **BSD-3** (© 2019 M. Nuzzo) | нет | **взят** |
| `DriveSourceModels.h` + `AirwindowsMasterModels.h` — Tape, Tube, PurestDrive, Density, Wavefolder, Decimator | **MIT** (Airwindows, DaisySP) | нет | **взят** |
| 12 моделей драйва и таблица профилей (`MasterBus.h`) | код автора | вырезаны из JUCE-класса | **взят** (перенесён только шейпер) |
| `OdinComb.h` — COMB, модель ODIN2 | **GPL-3.0-or-later** | нет | **взят** (GPL на весь проект) |
| `ParallelCombModel.h` — COMB, ParallelComb | **GPL-3.0-only** (© T. Endo) | + 6 файлов Uhhyou | **взят** |
| `CombColour.h` — обвязка COMB | **GPL-3.0-only** | над GPL-ядрами | **взят** |
| `ChorusCore.{h,cpp}`, `ChorusTables.h`, `AudioAdapter.{h,cpp}` | код автора | нет (ядро на 44.1 кГц + своя sinc-конверсия) | **взят** |
| `MasterBus.h`, `Rack.h`, `PluginProcessor/Editor` | JUCE + включают GPL-части | JUCE 8 | не переносится |
| `NewEffects.h`, `ReverbFX.h` | код автора | дилей и два реверба у нас уже есть | на потом |
| `SpectreClassic.h` | **BSD-3** | **взят**: `src/instrument/fx_spectre_classic.h` |

GPL-части требуют перевода всего проекта на GPL-3.0 — на это и решено было
пойти (продажа не планируется). Проект целиком теперь GPL-3.0-or-later,
MIT/BSD-уведомления внутри сохранены.

Провенанс и тексты лицензий — `third_party/monojuice/README.md` и
`third_party/monojuice/licenses/`. Один файл пришлось восстановить:
`SpectrumPixelator-BSD-3-Clause.txt` в архиве плагина отсутствовал, хотя
`Spectre.h` на него ссылается (текст стандартный, правообладатель — из заголовка).

## 2. Что перенесено и как

| эффект | адаптер | файл-источник |
|---|---|---|
| **Drive**, 19 моделей | `src/instrument/fx_drive.h` | 12 моделей + профили из `MasterBus.h`, Airwindows/DaisySP из `third_party/monojuice/dsp/`, WDF — наш `fx_wdf.h` |
| **Material**, модальный резонатор | `src/instrument/fx_material.h` | `MaterialResonator.h` |
| **Spectre**, MDCT-маска | `src/instrument/fx_spectre.h` | `Spectre.h` |
| **Glitch**, статтер | `src/instrument/fx_glitch.h` | `GlitchCore.h` |
| **COMB**, 2 модели | `src/instrument/fx_comb.h` | `CombColour.h` + `OdinComb.h` + `ParallelCombModel.h` + Uhhyou |
| **CHORUS**, прошивочное ядро | `src/instrument/fx_chorus_fw.h` | `ChorusCore.*` + `AudioAdapter.*` + `ChorusTables.h` |
| **SPECTRE CLASSIC** | `src/instrument/fx_spectre_classic.h` | `SpectreClassic.h` |

Что изменено при переносе (всё — осознанно):

1. **JUCE-оверсэмплер заменён на наш полифазный 4x** (`Fir4x` из `fx_wdf.h`):
   у него измеренный алиасинг (−59 дБ на 5 кГц) и нет зависимости от JUCE.
   WDF-модель внутри драйва не передискретизируется второй раз — иначе
   получалось 16x и 14.5 % ядра вместо 3.35 %.
2. **Калибровка громкости драйва.** В MonoJuice уровень у моделей разный (до
   15 дБ разницы). Добавлена двухточечная поправка (замер RMS на 220 Гц при
   amount 0 и 1, между ними линейно): ручка DRIVE меняет характер, но не
   громкость.
3. **`std::tanh` заменён на `fastTanh`, `std::sin` — на `fastSin/fastCosTbl`,
   `std::log10/pow` в дБ-пересчётах — на `fastLog2/fastExp2`.** Там, где точность
   важнее скорости (дрейф в Organics/Resynth), оставлен точный `sin` — см.
   `PERFORMANCE.md`.
4. **Обход по нулевому миксу.** Spectre при `mix = 0` не считает MDCT и не
   вносит задержку окна; Glitch и Material при нулевом миксе не работают вовсе.
5. **История глитча ограничена 4 с** (в оригинале до 24 с = ~9 МБ стерео):
   адаптер готовит движок на «высокой» частоте, как сам MonoJuice делает при
   96+ кГц. Поднять — `GlitchFx::kHistorySeconds`.
6. **COMB: внешний микс вместо внутреннего.** `CombColour` смешивает dry/wet
   параметром `mix`, который сглаживается 10 мс. Наш адаптер ставит его в 100 %
   и смешивает сам — так `combMix = 0` можно сделать полным обходом (эффект не
   считается вовсе), а не «wet на 0 %». Остальные параметры — «человеческие»
   (Гц, дБ, проценты) и применяются лениво: `set()` у COMB пересчитывает
   коэффициенты, дёргать его каждый сэмпл нельзя.
7. **CHORUS: внешний микс с компенсацией задержки.** Ядро всегда работает на
   44.1 кГц (так в оригинале), при другой частоте `AudioAdapter` делает
   64-отводную sinc-конверсию, поэтому задержка разная: 16 отсчётов на 44.1 кГц,
   **87** на 48 кГц, **139** на 96 кГц. Внутренний MIX прошивки фиксирован на
   127 (полностью обработанный сигнал), dry задерживается на `latency()`,
   иначе dry/wet складывались бы со сдвигом и давали гребёнку вместо хоруса.
   Проверено: при `mix = 0` выход совпадает с задержанным входом **бит в бит**
   (расхождение 0.0), при `mix = 1` и выключенной модуляции — точная задержанная
   копия (корреляция 1.000).
8. **Сторонние `.cpp` собираются с `-w`**: `ChorusCore.cpp` — машинно
   сгенерированный код, свои предупреждения в нём не правим (файл копируется
   без изменений).

## 3. Интеграция в цепочку

```
Drive (19 моделей) → COMB (ODIN2 / ParallelComb) → Chorus → Ping-pong Delay →
Reverb → Plate → EQ → Material → Spectre → Glitch → Compressor → Limiter
```

`FxChain::Settings` получил: `driveModel` (0…18), `driveTone`,
`chorusModel` (0 — наш простой хорус, 1 — прошивочный) и `chorusFw` (8 параметров
DEL/DEP/SPD/MIX/FB/WID/LP/INP, 0…127),
`combMix/Model/DriveDb/OutputDb/Depth/Rate/Envelope`, `combFreqA/FreqB`,
`combResA/ResB`, `combBalance`, `combRouting` (0 — A, 1 — A→B, 2 — A+B),
`combNegative`, `combDelayMs`, `combFeedback`, `combHighpass`, `combTune`,
`combOversample`, `combMidSide`,
`materialMix/Mode/Excite/Tone/Damp/Size/Feedback/Follow`,
`spectreMix/Window/Prominence/Shift/Peaks/Width/Freeze`,
`glitchMix/Chance/LengthMs/Repeats/Reverse/Pitch`.

## 4. Замеры

Уровень моделей драйва (220 Гц, вход RMS 0.283) — после калибровки:

| положение ручки | разброс по 19 моделям |
|---|---|
| amount = 0 | ±0.0 дБ |
| amount = 0.5 | от −1.7 до +4.4 дБ (худшая — Wavefolder) |
| amount = 1 | ±0.0 дБ |

Проверки в `tests/test_fx.cpp` (блоки 8–15, всего 40 проверок в наборе):

| что | результат |
|---|---|
| 19 моделей × 3 положения ручки | NaN 0, пик ≤ 2.44 (Rectifier — двухполупериодное выпрямление), уровень в пределах 4.4 дБ |
| «модели звучат по-разному» (Tube против Rectifier) | разница сигналов 2.4 дБ |
| Material, 4 профиля (импульс) | T60 = 2.02 / 1.57 / 2.08 / 4.36 с, пик 0.32 |
| Spectre (окно 2048, prominence 6 дБ) | пик 0.351, NaN 0, тон 440 Гц на выходе 0.240 при RMS 0.190 |
| Glitch (4 с, chance 0.6) | пик 0.400, RMS 0.199, NaN 0 |
| вся цепочка со всеми эффектами, 5 с | пик 0.676, RMS 0.273, NaN 0 |
| COMB ODIN2, resonance 50 / 75 / 95 | усиление 220 Гц **+3.5 / +7.9 / +20.0 дБ**, пик 0.45 / 0.75 / 3.15, NaN 0 |
| COMB ODIN2, схемы 0 / 1 / 2 | RMS 0.527 / 0.155 / 0.289 (последовательная тише, как и должно) |
| COMB ParallelComb, feedback 25 / 90 % | пик 1.000 (внутренний лимитер), хвост через 5 с 1.07e2 / 4.73e3, NaN 0 |
| CHORUS прошивочный: `mix = 0` | выход = задержанный вход, расхождение **0.0** |
| CHORUS прошивочный: без модуляции / с модуляцией | корреляция с dry **1.000** / **0.241** |
| вся цепочка с COMB и прошивочным хорусом, 5 с | пик 0.767, RMS 0.311, NaN 0 |

Стоимость (стерео, 48 кГц, 10 с, % одного ядра):

| эффект | % ядра |
|---|---|
| COMB: ODIN2 (2 линии) | 0.17 |
| COMB: ParallelComb (4 тапа) | 0.49 |
| COMB: ParallelComb с 16x оверсэмплингом | 12.47 |
| CHORUS прошивочный (48 кГц, с конверсией) | 2.04 |
| CHORUS наш простой | 0.13 |
| Drive: Tanh (без оверсэмплинга) | 0.25 |
| Drive: Air Tape | 1.19 |
| Drive: Fuzz (4x) | 2.88 |
| Drive: Diode WDF (4x внутри) | 3.35 |
| Material: metal (24 моды) | 0.40 |
| Material: latex (4 моды) | 0.51 |
| Spectre: окно 1024 / 4096 / 16384 | 0.72 / 0.84 / 1.21 |
| Glitch | 0.35 |
| FxChain базовый | 0.63 |
| FxChain + material + spectre + glitch | 2.27 |
| FxChain всё, драйв Diode WDF | 5.71 |

Демо: `audio/comb_chorus_demo.wav` (24 с) — sweep resonance 40→95 у ODIN2, три
схемы включения, рост обратной связи ParallelComb, микс прошивочного хоруса
0→1 и всё вместе. Пик 0.890, клипов 0, DC +0.0005.

Демо: `audio/fx_demo.wav` (34 с) — 19 моделей драйва по 0.5 с, четыре профиля
Material, три окна Spectre со сдвигом, статтер, и всё вместе с хорусом/дилеем/
plate. Пик 0.890, клипов 0, DC +0.0012.

## 5. Что стоит взять следующим шагом

1. **Pitch-шифтер и гранулярный FX** (`NewEffects.h`) — в инструменте есть
   гранулярный движок, но не эффект.
3. **Анализатор** (`PluginProcessor.cpp`): 24 лог-полосы + вектороскоп
   Mid/Side на фиксированных массивах — пригодится в GUI будущего VST3.
4. **Задержка в хосте.** Добавлен `FxChain::latency()` — сумма задержек
   активных эффектов (0 без них, 2048 у Spectre, +87 у прошивочного хоруса).
   Остаётся вызвать `setLatencySamples (fx.latency())` из JUCE-обёртки; внутри
   dry/wet задержка уже скомпенсирована.
