# OROGEN — рабочее пространство

Синтезатор по мотивам Terrain (9 движков, 16 фильтров, 16 эффектов, 8 слотов A–H),
DSP-ядро на C++ без зависимостей + обёртка VST3 на JUCE 8.

| каталог | что внутри |
|---|---|
| `orogen-dsp/` | DSP-ядро: 9 движков (включая слайсер CHOP), инструмент, 16 фильтров, 16 эффектов, арпеджиатор, мод-матрица. Сборка `make`, тесты `make test`, демо `make demo` → `audio/` |
| `orogen-plugin/` | VST3-обёртка: 294 параметра APVTS, 7 пресетов, GUI 1280×800 и оверлей 294/294. Локальный `artifact/OROGEN.vst3` — Linux x86-64, Release, stripped, 8 941 176 байт. Бинарный `.so` исключён из истории git. |
| `orogen/` | веб-разбор движков (`index.html`, `ENGINES.md`) |
| `terrain-deep-dive.md` | разбор оригинала: спецификации, архитектура, план |
| `OROGEN-vst3-vs2022.zip` | Исходники и скрипты сборки Visual Studio 2022; ревизия 21, 2 594 837 байт, 114 файлов, sha256 `9e8deec66e3bcf6ae477805dd97577864db0b5dc461d231be70d8a848c52f85a`. Сборка Windows не проверялась в Linux-песочнице. |
| `OROGEN-linux-vst3.zip` | Локально собранный Linux x86-64 VST3 (не входит в git/bundle): 3 960 252 байта, ELF требует GLIBC ≥ 2.38 и GLIBCXX 3.4.32. |

Текущий результат: **169 проверок `test_plugin`, провалов 0 в двух полных
прогонах**; `verify_vst3` — **24/24**. Переходы страниц и hover см.
`orogen-plugin/docs/ui_transitions.gif`. Превью Morph показывает фактическую
смесь слотов и их фильтры для ноты C4 **до мастер-FX**, а не выход DAW;
примеры — `orogen-plugin/docs/ui_morph_a.png`, `ui_morph_ab.png`, `ui_morph_h.png`.

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

# полная проверка и быстрые GUI-регрессии
./orogen-plugin/build-cmake/test_plugin
./orogen-plugin/build-cmake/test_plugin --gui-only

# стенды без дисплея: 0…7 — слоты, 8…11 — секции колонки, 100 — все параметры
./orogen-plugin/build-cmake/render_ui orogen-plugin/docs/ui_main.png 0 1280 800
```

Без swap `cc1plus` при сборке `juceaide` убивается OOM (проверено: конфигурация
падала с `fatal error: Killed signal terminated program cc1plus`).
