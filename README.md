# OROGEN — рабочее пространство

Синтезатор по мотивам Terrain (9 движков, 16 фильтров, 16 эффектов, 8 слотов A–H),
DSP-ядро на C++ без зависимостей + обёртка VST3 на JUCE 8.

| каталог | что внутри |
|---|---|
| `orogen-dsp/` | DSP-ядро: 9 движков (включая слайсер CHOP), инструмент, 16 фильтров, 16 эффектов, арпеджиатор, мод-матрица. Сборка `make`, тесты `make test`, демо `make demo` → `audio/` |
| `orogen-plugin/` | VST3-обёртка: 298 параметров APVTS, 7 пресетов, GUI 1280×800 и оверлей 298/298. Локальный `artifact/OROGEN.vst3` — Linux x86-64, Release, stripped, 8 953 464 байта. Бинарный `.so` исключён из истории git. |
| `orogen/` | веб-разбор движков (`index.html`, `ENGINES.md`) |
| `terrain-deep-dive.md` | разбор оригинала: спецификации, архитектура, план |
| `OROGEN-vst3-vs2022.zip` | Исходники и скрипты Visual Studio 2022, 2 865 698 байт, SHA-256 `8e5393fb0730557fda67d3f3d5bcfa83a4953be9c23617769a6f35cdb0a5c7ef`. Готового Windows VST3 нет; сборка Windows не проверялась. |
| `OROGEN-linux-vst3.zip` | Linux x86-64 VST3 (не входит в git/bundle), 3 986 762 байта, SHA-256 `c08a05d5c00eba06a47ab5bf60888219fcc727117ddbffae1df752242c485894`. ELF требует GLIBC ≥ 2.38 и GLIBCXX 3.4.32. |

Текущий результат (29.10.2026): **182 проверки `test_plugin`, 0 провалов**;
11 DSP-наборов — **294 проверки, 0 провалов**; `verify_vst3` — **29/29**.
Исправлены границы зёрен Granular, смена движка на удержанной ноте и 200-мс
pre-delay Plate. У Plate открыты ещё четыре параметра, первые 294 ID сохранены.
Примеры GUI: `orogen-plugin/docs/ui_engine_granular.png`, `ui_params_slot_a.png`,
`ui_params_fx_plate.png`; переходы — `ui_transitions.gif`. Превью Morph показывает
фактическую смесь слотов и их фильтры для ноты C4 **до мастер-FX**, не выход DAW.

Совместимость: проекты с прежним Plate pre-delay выше 100 мс теперь используют
сохранённое значение, а не прежний ошибочный предел 100 мс. Отдельной проверки
этой сборки в DAW и Windows/MSVC не проводилось.

Документы: `orogen-dsp/FX.md` (эффекты и замеры), `orogen-dsp/INSTRUMENT.md`,
`orogen-dsp/FILTERS.md` (16 моделей фильтра и их АЧХ),
`orogen-dsp/VERIFICATION.md` (294 проверки последнего прогона), `orogen-dsp/PERFORMANCE.md`,
`orogen-plugin/PLUGIN.md` (обёртка, банк программ), `gui-spec.md` (раскладка UI и сверка с макетом).

Сборка плагина на **Windows / Visual Studio 2022** — распаковать
`OROGEN-vst3-vs2022.zip` и запустить `build_vst3.bat` (JUCE скачается сам).

Сборка плагина в Linux-песочнице (нужны системные dev-пакеты X11/ALSA и `cmake`+`ninja`):

```bash
sudo apt-get install -y cmake ninja-build git pkg-config libx11-dev libxrandr-dev \
     libxinerama-dev libxcursor-dev libxext-dev libfreetype-dev libfontconfig1-dev \
     libasound2-dev libcurl4-openssl-dev

# конфигурация: juceaide собирается на этапе configure и на 2 ГБ RAM без свопа
# падает по OOM — нужен swap и последовательная сборка
CMAKE_BUILD_PARALLEL_LEVEL=1 cmake -S orogen-plugin -B orogen-plugin/build-cmake \
      -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOROGEN_BUILD_VST3=OFF
CMAKE_BUILD_PARALLEL_LEVEL=1 ninja -C orogen-plugin/build-cmake -j2 render_ui test_plugin

# полная проверка и быстрые GUI-регрессии
./orogen-plugin/build-cmake/test_plugin
./orogen-plugin/build-cmake/test_plugin --gui-only

# стенды без дисплея: 0…7 — слоты, 8…11 — секции колонки, 100 — все параметры
./orogen-plugin/build-cmake/render_ui orogen-plugin/docs/ui_main.png 0 1280 800
```

Без swap `cc1plus` при сборке `juceaide` убивается OOM (проверено: конфигурация
падала с `fatal error: Killed signal terminated program cc1plus`).
