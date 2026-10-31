# OROGEN — рабочее пространство

Синтезатор по мотивам Terrain (9 движков, 16 фильтров, 16 эффектов, 8 слотов A–H),
DSP-ядро на C++ без зависимостей + обёртка VST3 на JUCE 8.

| каталог | что внутри |
|---|---|
| `orogen-dsp/` | DSP-ядро: 9 движков (включая слайсер CHOP), инструмент, 16 фильтров, 16 эффектов, арпеджиатор, мод-матрица. Сборка `make`; проверку кода завершения тестов см. ниже; демо `make demo` → `audio/`. |
| `orogen-plugin/` | VST3-обёртка: 306 параметров APVTS, 7 пресетов, GUI 1280×800 и оверлей 306/306. Локальный `artifact/OROGEN.vst3` — Linux x86-64, Release, stripped, 8 519 040 байт. Бинарный `.so` исключён из истории git. |
| `orogen/` | веб-разбор движков (`index.html`, `ENGINES.md`) |
| `terrain-deep-dive.md` | разбор оригинала: спецификации, архитектура, план |
| `OROGEN-vst3-vs2022.zip` | Исходники и скрипты Visual Studio 2022. Готового Windows VST3 нет; Windows-сборка не проверялась. |
| `OROGEN-linux-vst3.zip` | Собранный Linux x86-64 VST3 (не входит в git/bundle). ELF требует GLIBC ≥ 2.38 и GLIBCXX ≥ 3.4.32. |
| `OROGEN-granular-raw.wav` | Диагностический Fm Tine из движка без FX и пост-нормировки, 48 кГц, mono float32; генерируется `tools/render_granular_raw.cpp`. |
| `OROGEN-granular-choir-unity.wav` | Та же диагностика, Choir Ah; ненормированный пик 1.312574 может быть срезан проигрывателем. |
| `OROGEN-granular-transient.wav` | 4-секундный тест тихого предудара, 48 кГц mono float32; пик 0.550595, без FX/пост-нормировки. |
| `OROGEN-granular-pitch-before.wav` / `...-after.wav` | Прямые 3-с рендеры до/после исправления высоты: 96 кГц mono float32, без FX и нормировки; эталон 297.301779 Гц. |
| `OROGEN-granular-short-before.wav` / `...-after.wav` | Диагностика границы 10 мс: 4 с, чередование синтетических источников длиной 479/480 отсчётов, 48 кГц, mono float32, без FX и нормировки. Это не музыкальное демо и не загрузка пользовательских файлов в GUI. |
| `OROGEN-granular-forward.wav` / `...-mixed.wav` / `...-reverse.wav` | Один и тот же Fm Tine, около 6 с, вероятность реверса 0/50/100%; чистый Granular, 48 кГц mono float32, без FX и нормировки. Конец обрезан по переходу через ноль. |
| `OROGEN-granular-stereo-before.wav` / `...-after.wav` | Диагностика независимости двух слотов при Reverse 50%: ~4 с, 48 кГц stereo float32, `Voice` с фильтрами и встроенным soft clip, без мастер-FX и пост-нормировки. |
| `orogen-plugin/docs/ui_params_grain_reverse.png` | Скриншот секции SLOT A с параметром Grain Reverse при выбранном Granular. |
| `orogen-plugin/docs/ui_lfo_wrap.gif` | Таймер LFO S&H при переходе за край дорожки после смены масштаба. |

Текущий результат (31.10.2026): **217/217 `test_plugin`** (native GUI под
Xvfb + openbox), 14 DSP-наборов — **370/370**, внешний JUCE VST3-хост —
**54/54** с реальным состоянием предыдущего VST3. Granular использует отдельные
воспроизводимые потоки Reverse для разных нот и слотов: два идентичных слоя
при 50% дают ΔRMS/RMS **0 → 0.991847** в Voice и **0.993401** в VST3-хосте.
На 0/100% контрольные рендеры до/после побайтно совпали.

Метры слотов получают пики окнами 10 мс и затухают; до исправления TSan
обнаруживал гонку с GUI, после — нет в проверенном сценарии. При остановленном
аудиоколбэке GUI гасит старый уровень через 250 мс. Нативный repaint метра:
**1 024 000 → 400 пикселей** на вызов; оставшиеся следы — 0 пикселей.
Макет 1280×800 сохранён, параметры **306/306** доступны, подписи PARAMS
увеличены 7 → 10 px; новый статус показывает существующий Grain Reverse.
Сохранены все 298 прежних ID и их порядок, 8 новых добавлены в конец.
Пользовательская загрузка аудиофайлов в GUI не реализована. Из распакованного
source ZIP повторно прошли **370/370** DSP-проверок; оба архива прошли CRC, а
VST3 в Linux ZIP побайтно совпадает с проверенным артефактом.

Прямой Granular на синтетическом источнике 26 с: ошибка высоты **−299.9995 →
+0.0002 цента**; через MIDI/VST3 с фабричным источником **−13.2960 → +0.3787**.
ASan/UBSan Granular: 240 сценариев/3013 зёрен с автоматизацией, 432 сценария
вероятности/3114 зёрен, 216 коротких/2016 зёрен и 30 быстрых + 96 медленных
на длинном источнике — ошибок 0; ещё 4 проверки крайних длинных зёрен при
96 кГц: заворотов sinc 0. Предыдущая проверка Delay — 18 с на 44.1/48/96 кГц,
ошибок 0. Два заданных сценария Choir в VST3 дали пики
**0.356468 / 0.821370** с 0/96000 отсчётов на пороге 0.98. Это не гарантия
для других настроек: сырой Choir достигает **1.312574**, headroom открыт.
Также в фабричном источнике измерено сильное отклонение высоты от формулы
множителя вибрато; общий банк не менялся. Для выпуска 30.10 из распакованного
source ZIP собраны Granular **49/49**, Reverse **8/8**, Instrument **68/68**,
FX **98/98**; обратный WAV совпал побайтно. Методика и ограничения —
`orogen-dsp/ENGINE-05-granular.md`.

Ранее исправлены атака и границы зёрен, фаза общего MIP, склейка Organics,
разрыв FX-лимитера, смена движка на удержанной ноте и 200-мс pre-delay Plate;
все 298 ID прежней версии сохранены. ROBIN всегда добавляет 1024 отсчёта задержки, даже
при mix=0. Примеры GUI: `orogen-plugin/docs/ui_engine_granular.png`,
`ui_params_slot_a.png`, `ui_params_fx_plate.png`. Превью Morph показывает
фактическую смесь слотов и их фильтры для ноты C4 **до мастер-FX**, не выход DAW.

Совместимость: звук старых проектов может измениться из-за Granular/MIP/Organics,
исправления Pan/Level и лимитера при перегрузе; Delay Feedback выше 0.92 и Plate
pre-delay выше 100 мс теперь используют сохранённые значения вместо прежних
ошибочных пределов. Исправления нормировки и точности высоты Granular и
поведения Delay при автоматизации Mix/Time также могут изменить звук прежних
проектов. Detune и Pitch фиксируются для каждого зерна при его запуске: на удержанной ноте
автоматизация действует на последующие зёрна. При Reverse между 0 и 1 ранее
сохранённые патчи могут звучать иначе из-за независимых потоков направления.
Отдельной
проверки в DAW и Windows/MSVC не проводилось.
Пики сырого движка некоторых источников могут превышать 1.0 во float; подробности
и WAV без нормировки — в `orogen-dsp/ENGINE-05-granular.md`.

Документы: `orogen-dsp/FX.md` (эффекты и замеры), `orogen-dsp/INSTRUMENT.md`,
`orogen-dsp/FILTERS.md` (16 моделей фильтра и их АЧХ),
`orogen-dsp/VERIFICATION.md` (370 проверок последнего прогона), `orogen-dsp/PERFORMANCE.md`,
`orogen-plugin/PLUGIN.md` (обёртка, банк программ), `gui-spec.md` (раскладка UI и сверка с макетом).

Проверка DSP с сохранением кодов выхода (`make test` теперь также
останавливается на первом неуспешном тесте):

```bash
cd orogen-dsp
make -j1 all
make test             # запускать отдельно от сборки ради честного CPU-теста
make asan-granular    # отдельный стенд безопасности, без CPU-бенчмарка
make asan-streams asan-meters  # ASan/UBSan голосов и метров
make tsan-meters      # Linux/GCC, отдельно от ASan
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
