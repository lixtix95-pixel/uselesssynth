# Сторонний DSP из MonoJuice / MonoHuice

Первоначально файлы в `dsp/` взяты из предоставленного пользователем архива
MonoJuice 0.9.5. Затем `GlitchCore.h`, `ChorusCore.h` и `ChorusCore.cpp`
обновлены по предоставленному пользователем репозиторию
https://github.com/lixtix95-pixel/monohuice-0.9.15, commit
`e98c79a8697ba9ade45acc5f15548c5c2c02ab3d`. Пользователь разрешил включение
этого кода в проект. В этом коммите не найден корневой LICENSE репозитория:
лицензия всего репозитория здесь **не предполагается**. Уведомления и лицензии
включённых сторонних компонентов сохранены. Перед дальнейшим распространением
нужно отдельно удостовериться в правах на исходный код автора и совместимости
условий. `ChorusCore` дополнительно получил переключатель совместимости DEL=0;
`AudioAdapter.h` передаёт этот переключатель. `Rack.h`, `MasterBus.h`,
`PluginProcessor.*` и `PluginEditor.*` целиком не перенесены.

## Что лежит и под какой лицензией

| файл | происхождение | лицензия |
|---|---|---|
| `dsp/FirmwarePrimitives.h` | MonoJuice (`Frame`, `History`, миксы, окно) | код автора плагина |
| `dsp/MaterialResonator.h` | MonoJuice, модальный резонатор (4 профиля) | код автора плагина |
| `dsp/GlitchCore.h` | MonoJuice, статтер/глитч | код автора плагина |
| `dsp/Spectre.h` | MonoJuice, порт Spectrum Pixelator (Michael Nuzzo) | **BSD-3-Clause**, © 2019 Michael Nuzzo |
| `dsp/AirwindowsMasterModels.h` | адаптации Airwindows PurestDrive, Density, Pressure4, ButterComp2 | **MIT**, © 2016 Chris Johnson |
| `dsp/DriveSourceModels.h` | адаптации Airwindows Tape/Tube и DaisySP Wavefolder/Decimator | **MIT** (Airwindows, © 2016; DaisySP, © 2020 Electrosmith) |
| `dsp/ChorusTables.h` | таблицы прошивочного хоруса (`widthTable`, `lowpassTable`) | код автора плагина |
| `dsp/ChorusCore.{h,cpp}` | статически транслированная процедура CHORUS из прошивки OS 1.32B | код автора плагина |
| `dsp/AudioAdapter.{h,cpp}` | обвязка автора: ядро хоруса всегда на 44.1 кГц, 64-отводная sinc-конверсия | код автора плагина |
| `dsp/OdinComb.h` | COMB, модель ODIN2 | **GPL-3.0-or-later**, © 2020-2021 TheWaveWarden |
| `dsp/ParallelCombModel.h` | COMB, модель ParallelComb | **GPL-3.0-only**, © Takamitsu Endo |
| `dsp/CombColour.h` | слой драйва/модуляции над двумя COMB | **GPL-3.0-only**, © автор плагина |
| `third_party/Uhhyou/**` | DSP Uhhyou, нужный ParallelComb (6 файлов) | **GPL-3.0-only**, © Takamitsu Endo |
| `dsp/SpectreClassic.h` | вторая реализация Spectre (подключена) | **BSD-3-Clause**, © 2019 M. Nuzzo |
| `licenses/Airwindows-MIT.txt` | текст лицензии из архива плагина | MIT |
| `licenses/DaisySP-MIT.txt` | текст лицензии из архива плагина (с уведомлениями Plaits/Soundpipe) | MIT |
| `licenses/SpectrumPixelator-BSD-3-Clause.txt` | стандартный текст BSD-3 с указанием © 2019 Michael Nuzzo | BSD-3 |

В архиве плагина файла `Licenses/SpectrumPixelator-BSD-3-Clause.txt` не было
(на него ссылается заголовок `Spectre.h`), поэтому текст восстановлен по
стандартной формулировке BSD-3-Clause с тем же правообладателем, что указан в
заголовке файла. Если у автора есть оригинал — заменить.

## Что НЕ взято и почему

| файл плагина | причина |
|---|---|
| `Rack.h`, `MasterBus.h`, `PluginProcessor.*`, `PluginEditor.*` | JUCE-зависимые (AudioBuffer/APVTS/GUI), тащат за собой весь плагин |
| `NewEffects.h`, `ReverbFX.h`, `FXChain.h` | у OROGEN уже есть дилей и два реверба; при необходимости берутся так же, как остальное |

## Лицензионный статус

Собственный код OROGEN сопровождается GPL-3.0-or-later. При этом файлы
Uhhyou и другие компоненты имеют собственные SPDX и лицензии, включая
GPL-3.0-only; их условия нельзя заменить общей лицензией проекта. Для кода из
MonoHuice см. оговорку об отсутствии корневого LICENSE выше. Некоммерческое
использование само по себе не отменяет условий GPL/AGPL.
