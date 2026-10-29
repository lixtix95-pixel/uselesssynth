# OROGEN — рабочее пространство

Синтезатор по мотивам Terrain (9 движков, 16 фильтров, 16 эффектов, 8 слотов A–H),
DSP-ядро на C++ без зависимостей + обёртка VST3 на JUCE 8.

| каталог | что внутри |
|---|---|
| `orogen-dsp/` | DSP-ядро: 9 движков (включая слайсер CHOP), инструмент, 16 фильтров, 16 эффектов, арпеджиатор, мод-матрица. Сборка `make`, тесты `make test`, демо `make demo` → `audio/` |
| `orogen-plugin/` | VST3-обёртка: 294 параметра APVTS, 7 пресетов, интерфейс по макету 1280×800 + оверлей всех 294 параметров (`docs/ui_main.png`, `docs/ui_params_*.png`, спецификация — `gui-spec.md`). `artifact/OROGEN.vst3` — собранный плагин (8.85 МБ, Release, stripped, с интерфейсом по макету и интерактивным главным экраном) |
| `orogen/` | веб-разбор движков (`index.html`, `ENGINES.md`) |
| `terrain-deep-dive.md` | разбор оригинала: спецификации, архитектура, план |
| `OROGEN-vst3-vs2022.zip` | **готовый кит для сборки VST3 в Visual Studio 2022**: исходники + `build_vst3.bat` / `build_vst3.ps1` / `CMakePresets.json` + инструкция `README-BUILD-WINDOWS.md`. 1809 КБ, 109 файлов, sha256 `e3e002e3…` (ревизия 19: 19 интерактивных зон главного экрана — дисплей тянется (позиция/варп таблицы), чипы FX листаются (10 эффектов на 2 страницах), 8 маршрутов мод-матрицы с приёмником, банк программ в GUI; LOAD/SAVE с меню банка; анимации интерфейса (блок 11), дисплей показывает форму выбранного движка — рендер из DSP (блок 12: переключение движка в селекторе слышно 36/36 пар, видно по 20 204 пикселям); `test_plugin` — 141 проверка, провалов 0 |

Документы: `orogen-dsp/FX.md` (эффекты и замеры), `orogen-dsp/INSTRUMENT.md`,
`orogen-dsp/FILTERS.md` (16 моделей фильтра и их АЧХ),
`orogen-dsp/VERIFICATION.md` (263 проверки), `orogen-dsp/PERFORMANCE.md`,
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

# стенды без дисплея: 0…7 — слоты, 8…11 — секции колонки, 100 — все параметры
./orogen-plugin/build-cmake/render_ui orogen-plugin/docs/ui_main.png 0 1280 800
```

Без swap `cc1plus` при сборке `juceaide` убивается OOM (проверено: конфигурация
падала с `fatal error: Killed signal terminated program cc1plus`).
