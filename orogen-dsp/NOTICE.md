# Лицензии

Исходники DSP-проекта предоставлены на условиях **GNU GPL-3.0-or-later**
(текст — `LICENSE`), но некоторые включённые файлы обозначены **GPL-3.0-only**
(в частности, ParallelComb и маршрутизации Digitone-style FM). Сборка VST3 дополнительно использует JUCE 8,
доступный по AGPL-3.0 либо по отдельной коммерческой лицензии. Лицензии
компонентов и их авторство следует учитывать вместе; условия и тексты GPL,
AGPL и JUCE поставляются с архивами. Код с GPL-3.0-only сохраняет собственные
SPDX-метки и уведомления об авторстве.

## Сторонний код и его лицензии

| компонент | путь | лицензия | правообладатель |
|---|---|---|---|
| JUCE 8.0.15 (VST3-обёртка) | FetchContent, https://github.com/juce-framework/JUCE | AGPL-3.0 или коммерческая | © Raw Material Software Limited |
| ChoralRoot FM-1: адаптированные графы маршрутизации | `src/engines/digitone_routing.h`, https://github.com/Quixotic7/ChoralRootFM1 | GPL-3.0-only | ChoralRoot FM-1 contributors; upstream commit `ec9c54fdd7a0a544e8ec82bec196db745868f057` |
| Dattorro plate reverb | `third_party/dattorro-verb/` | MIT | © 2022 Pauli Pölkki |
| WDF (схемная сатурация) | `third_party/chowdsp_wdf/` | BSD-3-Clause | © Jatin Chowdhury |
| Odin 2 comb (перенос) | `third_party/monojuice/dsp/OdinComb.h` | **GPL-3.0-or-later** | © 2020-2021 TheWaveWarden |
| ParallelComb | `third_party/monojuice/dsp/ParallelCombModel.h`, `third_party/monojuice/third_party/Uhhyou/` | **GPL-3.0-only** | © Takamitsu Endo (Uhhyou) |
| CombColour, ChorusCore, AudioAdapter, Material, Glitch, Drive-модели | `third_party/monojuice/dsp/` | Учитывать лицензии отдельных файлов; корневой LICENSE репозитория MonoHuice 0.9.15 не найден | автор плагина / указанные в файлах авторы |
| Spectrum Pixelator (Spectre, EXTENDED и CLASSIC) | `third_party/monojuice/dsp/Spectre.h`, `SpectreClassic.h` | BSD-3-Clause | © 2019 Michael Nuzzo |
| Airwindows-модели драйва | `third_party/monojuice/dsp/AirwindowsMasterModels.h`, `DriveSourceModels.h` | MIT | © 2016 Chris Johnson |
| DaisySP (Wavefolder, Decimator) | там же | MIT | © 2020 Electrosmith |

Тексты лицензий — `third_party/monojuice/licenses/` и рядом с соответствующими
каталогами в `third_party/`. В `ChorusCore` и `AudioAdapter.h` есть переключатель совместимости OROGEN;
`GlitchCore.h` и `ChorusCore` обновлены из репозитория MonoHuice 0.9.15
(commit `e98c79a8697ba9ade45acc5f15548c5c2c02ab3d`), предоставленного
пользователем с разрешением использовать код. Корневой LICENSE этого
репозитория не обнаружен, поэтому его общая лицензия не утверждается;
перед распространением следует уточнить права у правообладателя.

Linux-сборка JUCE 8.0.15 использует локальную при сборке правку Xdnd в
`orogen-plugin/cmake/PatchJuceXdnd.cmake`: она компилирует копию исходного
файла JUCE с исправлениями timestamp/hover, не изменяя исходный каталог JUCE.
Лицензию JUCE следует учитывать и для этой копии. Исходный архив OROGEN не
включает дерево JUCE; точную версию 8.0.15 необходимо получить отдельно по
инструкциям сборки. При дальнейшей передаче бинарников обеспечьте получателям
соответствующий исходный код и соблюдение применимых лицензий.
