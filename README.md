# OROGEN — рабочее пространство

Синтезатор по мотивам Terrain (9 движков, 16 фильтров, 16 эффектов, 8 слотов A–H),
DSP-ядро на C++ без зависимостей + обёртка VST3 на JUCE 8.

| каталог | что внутри |
|---|---|
| `orogen-dsp/` | DSP-ядро: 9 движков (включая слайсер CHOP), инструмент, 16 фильтров, 16 эффектов, арпеджиатор, мод-матрица. Сборка `make`; проверку кода завершения тестов см. ниже; демо `make demo` → `audio/`. |
| `orogen-plugin/` | VST3-обёртка: 298 параметров APVTS, 7 пресетов, GUI 1280×800 и оверлей 298/298. Локальный `artifact/OROGEN.vst3` — Linux x86-64, Release, stripped, 8 961 688 байт. Бинарный `.so` исключён из истории git. |
| `orogen/` | веб-разбор движков (`index.html`, `ENGINES.md`) |
| `terrain-deep-dive.md` | разбор оригинала: спецификации, архитектура, план |
| `OROGEN-vst3-vs2022.zip` | Исходники и скрипты Visual Studio 2022, 2 878 094 байта; SHA-256 `fb424e6dc893bc9e946e6449c1ff6fd9946b606a96ab8eea4e28ed640af15444`. Готового Windows VST3 нет; Windows-сборка не проверялась. |
| `OROGEN-linux-vst3.zip` | Linux x86-64 VST3 (не входит в git/bundle), 3 989 099 байт; SHA-256 `e7395c4cfc21c4260f6cd8cacf7fec6db02c73dc61fb4b29979328cf4c4ecbb8`. ELF требует GLIBC ≥ 2.38 и GLIBCXX ≥ 3.4.32. |
| `OROGEN-granular-raw.wav` | Диагностический Fm Tine из движка без FX и пост-нормировки, 48 кГц, mono float32; генерируется `tools/render_granular_raw.cpp`. |
| `OROGEN-granular-choir-unity.wav` | Та же диагностика, Choir Ah; ненормированный пик 1.311852 может быть срезан проигрывателем. |

Текущий результат (29.10.2026): **185 проверок `test_plugin`, 0 провалов**;
11 DSP-наборов — **310 проверок, 0 провалов**; `verify_vst3` — **32/32**.
Исправлены атака, автоматизация и границы зёрен Granular, фаза общего MIP,
склейка Organics и разрыв на пороге выходного FX-лимитера без смены порога.
Ранее исправлены смена движка на удержанной ноте и 200-мс
pre-delay Plate; первые 294 ID сохранены. ROBIN всегда добавляет 1024 отсчёта
задержки, даже при mix=0. Примеры GUI: `orogen-plugin/docs/ui_engine_granular.png`,
`ui_params_slot_a.png`, `ui_params_fx_plate.png`. Превью Morph показывает
фактическую смесь слотов и их фильтры для ноты C4 **до мастер-FX**, не выход DAW.

Совместимость: звук старых проектов может измениться из-за Granular/MIP/Organics
и исправления лимитера при перегрузе; Plate pre-delay выше 100 мс теперь использует
сохранённое значение вместо прежнего ошибочного предела. Отдельной проверки в DAW
и Windows/MSVC не проводилось.
Пики сырого движка некоторых источников могут превышать 1.0 во float; подробности
и WAV без нормировки — в `orogen-dsp/ENGINE-05-granular.md`.

Документы: `orogen-dsp/FX.md` (эффекты и замеры), `orogen-dsp/INSTRUMENT.md`,
`orogen-dsp/FILTERS.md` (16 моделей фильтра и их АЧХ),
`orogen-dsp/VERIFICATION.md` (310 проверок последнего прогона), `orogen-dsp/PERFORMANCE.md`,
`orogen-plugin/PLUGIN.md` (обёртка, банк программ), `gui-spec.md` (раскладка UI и сверка с макетом).

Проверка DSP с сохранением кодов выхода (`make test` теперь также
останавливается на первом неуспешном тесте):

```bash
cd orogen-dsp
make -j2 all
for n in wavetable fm harmonic sample granular modal resynth organics instrument fx chop; do
    ./build/test_$n || exit 1
done
```

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
