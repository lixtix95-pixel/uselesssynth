# Лицензии

Исходники DSP-проекта предоставлены на условиях **GNU GPL-3.0-or-later**
(текст — `LICENSE`), но некоторые включённые файлы обозначены **GPL-3.0-only**
(в частности, ParallelComb). Сборка VST3 дополнительно использует JUCE 8,
доступный по AGPL-3.0 либо по отдельной коммерческой лицензии. Лицензии
компонентов и их авторство следует учитывать вместе; условия и тексты GPL,
AGPL и JUCE поставляются с архивами. Код с GPL-3.0-only сохраняет собственные
SPDX-метки и уведомления об авторстве.

## Сторонний код и его лицензии

| компонент | путь | лицензия | правообладатель |
|---|---|---|---|
| JUCE 8.0.15 (VST3-обёртка) | FetchContent, https://github.com/juce-framework/JUCE | AGPL-3.0 или коммерческая | © Raw Material Software Limited |
| Dattorro plate reverb | `third_party/dattorro-verb/` | MIT | © 2022 Pauli Pölkki |
| WDF (схемная сатурация) | `third_party/chowdsp_wdf/` | BSD-3-Clause | © Jatin Chowdhury |
| Odin 2 comb (перенос) | `third_party/monojuice/dsp/OdinComb.h` | **GPL-3.0-or-later** | © 2020-2021 TheWaveWarden |
| ParallelComb | `third_party/monojuice/dsp/ParallelCombModel.h`, `third_party/monojuice/third_party/Uhhyou/` | **GPL-3.0-only** | © Takamitsu Endo (Uhhyou) |
| CombColour, ChorusCore, AudioAdapter, Material, Glitch, Drive-модели | `third_party/monojuice/dsp/` | код автора плагина MonoJuice; части под GPL-3.0 в силу сборки (см. выше) | © MonomachineNova / автор плагина |
| Spectrum Pixelator (Spectre, EXTENDED и CLASSIC) | `third_party/monojuice/dsp/Spectre.h`, `SpectreClassic.h` | BSD-3-Clause | © 2019 Michael Nuzzo |
| Airwindows-модели драйва | `third_party/monojuice/dsp/AirwindowsMasterModels.h`, `DriveSourceModels.h` | MIT | © 2016 Chris Johnson |
| DaisySP (Wavefolder, Decimator) | там же | MIT | © 2020 Electrosmith |

Тексты лицензий — `third_party/monojuice/licenses/` и рядом с соответствующими
каталогами в `third_party/`. Файлы стороннего кода копируются **без правок**;
все адаптации живут в `src/instrument/fx_*.h` с указанием источника в шапке.
