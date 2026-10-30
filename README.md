# OROGEN — рабочее пространство

Синтезатор по мотивам Terrain (9 движков, 16 фильтров, 16 эффектов, 8 слотов A–H),
DSP-ядро на C++ без зависимостей + обёртка VST3 на JUCE 8.

| каталог | что внутри |
|---|---|
| `orogen-dsp/` | DSP-ядро: 9 движков (включая слайсер CHOP), инструмент, 16 фильтров, 16 эффектов, арпеджиатор, мод-матрица. Сборка `make`; проверку кода завершения тестов см. ниже; демо `make demo` → `audio/`. |
| `orogen-plugin/` | VST3-обёртка: 298 параметров APVTS, 7 пресетов, GUI 1280×800 и оверлей 298/298. Локальный `artifact/OROGEN.vst3` — Linux x86-64, Release, stripped, 8 514 944 байта. Бинарный `.so` исключён из истории git. |
| `orogen/` | веб-разбор движков (`index.html`, `ENGINES.md`) |
| `terrain-deep-dive.md` | разбор оригинала: спецификации, архитектура, план |
| `OROGEN-vst3-vs2022.zip` | Исходники и скрипты Visual Studio 2022, 3 074 813 байт; SHA-256 `403f3c9bc18bf0d4ae37ced0165d68a63c0bbb9ad32ce26e54cf87b5588037d3`. Готового Windows VST3 нет; Windows-сборка не проверялась. |
| `OROGEN-linux-vst3.zip` | Linux x86-64 VST3 (не входит в git/bundle), 3 547 538 байт; SHA-256 `dc8ed3e9c763022e87d52b4b635fe3f38f8d6cf6e7b461604a6d1b76a3098b6a`. ELF требует GLIBC ≥ 2.38 и GLIBCXX ≥ 3.4.32. |
| `OROGEN-granular-raw.wav` | Диагностический Fm Tine из движка без FX и пост-нормировки, 48 кГц, mono float32; генерируется `tools/render_granular_raw.cpp`. |
| `OROGEN-granular-choir-unity.wav` | Та же диагностика, Choir Ah; ненормированный пик 1.312574 может быть срезан проигрывателем. |
| `OROGEN-granular-transient.wav` | 4-секундный тест тихого предудара, 48 кГц mono float32; пик 0.550595, без FX/пост-нормировки. |
| `OROGEN-granular-pitch-before.wav` / `...-after.wav` | Прямые 3-с рендеры до/после исправления высоты: 96 кГц mono float32, без FX и нормировки; эталон 297.301779 Гц. |
| `orogen-plugin/docs/ui_lfo_wrap.gif` | Таймер LFO S&H при переходе за край дорожки после смены масштаба. |

Текущий результат (30.10.2026): **200 проверок `test_plugin`, 0 провалов**
(включая native GUI под Xvfb + openbox); 11 DSP-наборов — **338 проверок,
0 провалов**; `verify_vst3` — **41/41**. Исправлены расстройка Granular на
поздней позиции и утечка 298 контролов оверлея при закрытии GUI. Хост проверил
6 повторных циклов открытия и выгрузки VST3; слоты и параметры сохранены.

Прямой Granular на синтетическом источнике 26 с: ошибка высоты **−299.9995 →
+0.0002 цента**; через MIDI/VST3 с фабричным источником **−13.2960 → +0.3787**.
ASan/UBSan Granular: 240 сценариев, 3013 зёрен плюс 15 быстрых и 48 медленных
на длинном источнике, 0 небезопасных чтений и NaN; Delay — 18 с на 44.1/48/96
кГц, ошибок нет. Из распакованного source ZIP собраны и пройдены Granular
**41/41**, Instrument **68/68** и FX **98/98**, тестовый pitch-WAV совпал
побайтно; распакованный Linux VST3 прошёл хост **41/41**. Сырой Choir достигает
**1.312574**: headroom остаётся открытым, см. `orogen-dsp/ENGINE-05-granular.md`.

Ранее исправлены атака и границы зёрен, фаза общего MIP, склейка Organics,
разрыв FX-лимитера, смена движка на удержанной ноте и 200-мс pre-delay Plate;
первые 294 ID сохранены. ROBIN всегда добавляет 1024 отсчёта задержки, даже
при mix=0. Примеры GUI: `orogen-plugin/docs/ui_engine_granular.png`,
`ui_params_slot_a.png`, `ui_params_fx_plate.png`. Превью Morph показывает
фактическую смесь слотов и их фильтры для ноты C4 **до мастер-FX**, не выход DAW.

Совместимость: звук старых проектов может измениться из-за Granular/MIP/Organics,
исправления Pan/Level и лимитера при перегрузе; Delay Feedback выше 0.92 и Plate
pre-delay выше 100 мс теперь используют сохранённые значения вместо прежних
ошибочных пределов. Исправления нормировки и точности высоты Granular и
поведения Delay при автоматизации Mix/Time также могут изменить звук прежних
проектов. Detune и Pitch фиксируются для каждого зерна при его запуске: на удержанной ноте
автоматизация действует на последующие зёрна. Отдельной
проверки в DAW и Windows/MSVC не проводилось.
Пики сырого движка некоторых источников могут превышать 1.0 во float; подробности
и WAV без нормировки — в `orogen-dsp/ENGINE-05-granular.md`.

Документы: `orogen-dsp/FX.md` (эффекты и замеры), `orogen-dsp/INSTRUMENT.md`,
`orogen-dsp/FILTERS.md` (16 моделей фильтра и их АЧХ),
`orogen-dsp/VERIFICATION.md` (338 проверок последнего прогона), `orogen-dsp/PERFORMANCE.md`,
`orogen-plugin/PLUGIN.md` (обёртка, банк программ), `gui-spec.md` (раскладка UI и сверка с макетом).

Проверка DSP с сохранением кодов выхода (`make test` теперь также
останавливается на первом неуспешном тесте):

```bash
cd orogen-dsp
make -j1 all
make test             # запускать отдельно от сборки ради честного CPU-теста
make asan-granular    # отдельный стенд безопасности, без CPU-бенчмарка
make asan-delay       # отдельный стенд безопасности Delay
```

Сборка плагина на **Windows / Visual Studio 2022** — распаковать
`OROGEN-vst3-vs2022.zip` и запустить `build_vst3.bat` (JUCE скачается сам).

Сборка плагина в Linux-песочнице (нужны системные dev-пакеты X11/ALSA и `cmake`+`ninja`):

```bash
sudo apt-get install -y cmake ninja-build git pkg-config libx11-dev libxrandr-dev \
     libxinerama-dev libxcursor-dev libxext-dev libfreetype-dev libfontconfig1-dev \
     libasound2-dev libcurl4-openssl-dev xvfb openbox

# конфигурация: juceaide собирается на этапе configure и на 2 ГБ RAM без свопа
# падает по OOM — нужен swap и последовательная сборка
CMAKE_BUILD_PARALLEL_LEVEL=1 cmake -S orogen-plugin -B orogen-plugin/build-cmake \
      -G Ninja -DCMAKE_BUILD_TYPE=Release -DOROGEN_BUILD_VST3=ON
CMAKE_BUILD_PARALLEL_LEVEL=1 ninja -C orogen-plugin/build-cmake -j1 \
      OROGEN_VST3 render_ui test_plugin verify_vst3

# Полный тест с настоящим X11-окном требует Xvfb и window manager, например openbox:
xvfb-run -a sh -c 'openbox >/dev/null 2>&1 & wm=$!; sleep 1; \
    OROGEN_GUI_NATIVE=1 ./orogen-plugin/build-cmake/test_plugin; rc=$?; \
    kill "$wm" 2>/dev/null; exit "$rc"'

# Внешний VST3-хост и кадр UI:
xvfb-run -a ./orogen-plugin/build-cmake/verify_vst3 \
    "$(realpath orogen-plugin/build-cmake/OROGEN_artefacts/Release/VST3/OROGEN.vst3)" 1
xvfb-run -a ./orogen-plugin/build-cmake/render_ui \
    "$(pwd)/orogen-plugin/docs/ui_main.png" 0 1280 800
```

Без swap `cc1plus` при сборке `juceaide` убивается OOM (проверено: конфигурация
падала с `fatal error: Killed signal terminated program cc1plus`).
