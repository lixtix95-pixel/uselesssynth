# Лицензии

Проект целиком распространяется на условиях **GNU GPL-3.0-or-later** (текст —
`LICENSE`): в нём есть GPL-3.0 код из плагина MonoJuice 0.9.5 (COMB: адаптация
Odin 2 и ParallelComb от Uhhyou). Автор плагина разрешил использовать его код,
а продажа не планируется, поэтому GPL-совместимость — не проблема, но условия
GPL всё равно действуют на весь бинарник и исходники.

## Сторонний код и его лицензии

| компонент | путь | лицензия | правообладатель |
|---|---|---|---|
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
