CC       ?= gcc
CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter -march=native
INC      := -Isrc
# GPL-части MonoJuice: сторонний код, собирается с -w (свои предупреждения не чиним,
# файлы копируются без правок)
MONOJUICE_OBJ := build/monojuice_chorus.o build/monojuice_adapter.o

TEST_BINS := build/test_wavetable build/test_fm build/test_harmonic build/test_sample \
             build/test_granular build/test_granular_reverse build/test_granular_streams build/test_vocal_vibrato build/test_granular_vocal \
             build/test_modal build/test_resynth build/test_organics \
             build/test_instrument build/test_meters build/test_fx build/test_chop
DEMO_BINS := build/render_demo build/render_demo_sample build/render_demo_granular build/render_demo_modal \
             build/render_demo_resynth build/render_demo_organics build/render_demo_synth build/render_demo_instrument \
             build/render_demo_fx build/render_demo_comb build/render_demo_chop

all: $(TEST_BINS) $(DEMO_BINS)

build/test_wavetable: tests/test_wavetable.cpp src/engines/wavetable.h src/engines/halfband.h src/content/wavetable_bank.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tests/test_wavetable.cpp -o $@

build/test_fm: tests/test_fm.cpp tests/engine_suite.h src/engines/fm.h src/engines/engine.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tests/test_fm.cpp -o $@

build/test_harmonic: tests/test_harmonic.cpp tests/engine_suite.h src/engines/harmonic.h src/engines/engine.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tests/test_harmonic.cpp -o $@

build/test_sample: tests/test_sample.cpp tests/engine_suite.h src/engines/sample.h src/content/sample_bank.h src/engines/vibrato_phase.h src/engines/engine.h src/core/*.h src/engines/halfband.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tests/test_sample.cpp -o $@

build/test_organics: tests/test_organics.cpp tests/engine_suite.h src/engines/organics.h src/engines/mip_reader.h src/engines/sinc_interp.h src/content/sample_bank.h src/engines/vibrato_phase.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tests/test_organics.cpp -o $@

build/test_resynth: tests/test_resynth.cpp tests/engine_suite.h src/engines/resynth.h src/content/resynth_data.h src/content/sample_bank.h src/engines/vibrato_phase.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tests/test_resynth.cpp -o $@

build/test_modal: tests/test_modal.cpp tests/engine_suite.h src/engines/modal.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tests/test_modal.cpp -o $@

build/test_granular: tests/test_granular.cpp tests/engine_suite.h src/engines/granular.h src/engines/engine.h src/engines/mip_reader.h src/engines/sinc_interp.h src/content/sample_bank.h src/engines/vibrato_phase.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tests/test_granular.cpp -o $@

build/test_granular_reverse: tests/test_granular_reverse.cpp tests/engine_suite.h src/engines/granular.h src/engines/engine.h src/engines/mip_reader.h src/engines/sinc_interp.h src/content/sample_bank.h src/engines/vibrato_phase.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tests/test_granular_reverse.cpp -o $@

build/test_granular_streams: tests/test_granular_streams.cpp tests/engine_suite.h src/instrument/*.h src/engines/*.h src/content/*.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tests/test_granular_streams.cpp -o $@

build/test_vocal_vibrato: tests/test_vocal_vibrato.cpp src/content/sample_bank.h src/engines/vibrato_phase.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tests/test_vocal_vibrato.cpp -o $@

build/test_granular_vocal: tests/test_granular_vocal.cpp src/engines/granular.h src/engines/mip_reader.h src/engines/sinc_interp.h src/content/sample_bank.h src/engines/vibrato_phase.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tests/test_granular_vocal.cpp -o $@

build/render_vocal_source: tools/render_vocal_source.cpp src/content/sample_bank.h src/engines/vibrato_phase.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/render_vocal_source.cpp -o $@

build/test_meters: tests/test_meters.cpp tests/engine_suite.h src/instrument/*.h src/engines/*.h src/content/*.h src/core/*.h $(MONOJUICE_OBJ)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) -pthread tests/test_meters.cpp $(MONOJUICE_OBJ) -o $@

# Linux/GCC: отдельно, TSan и ASan нельзя смешивать. Компилируем сторонние
# единицы тоже с инструментированием. -no-pie помогает TSan на этом раннере.
build/test_meters_tsan: tests/test_meters.cpp tests/engine_suite.h src/instrument/*.h src/engines/*.h src/content/*.h src/core/*.h third_party/monojuice/dsp/*.cpp third_party/monojuice/dsp/*.h
	@mkdir -p build
	$(CXX) -O1 -g1 -std=c++17 -fsanitize=thread -fno-omit-frame-pointer -no-pie -pthread $(INC) tests/test_meters.cpp third_party/monojuice/dsp/ChorusCore.cpp third_party/monojuice/dsp/AudioAdapter.cpp -o $@

.PHONY: tsan-meters
tsan-meters: build/test_meters_tsan
	OROGEN_METER_RACE_ONLY=1 TSAN_OPTIONS=halt_on_error=1 ./build/test_meters_tsan

build/render_granular_streams: tools/render_granular_streams.cpp src/instrument/*.h src/engines/*.h src/content/*.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/render_granular_streams.cpp -o $@

# Отдельно от CPU-бенчмарка: санитайзеры заметно замедляют горячий цикл.
build/test_granular_safety_asan: tests/test_granular_safety.cpp src/engines/granular.h src/engines/engine.h src/engines/mip_reader.h src/engines/sinc_interp.h src/engines/halfband.h src/content/sample_bank.h src/engines/vibrato_phase.h src/core/*.h
	@mkdir -p build
	$(CXX) -O1 -g1 -std=c++17 -fsanitize=address,undefined -fno-omit-frame-pointer $(INC) tests/test_granular_safety.cpp -o $@

.PHONY: asan-granular
asan-granular: build/test_granular_safety_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./build/test_granular_safety_asan

build/test_granular_streams_asan: tests/test_granular_streams.cpp tests/engine_suite.h src/instrument/*.h src/engines/*.h src/content/*.h src/core/*.h
	@mkdir -p build
	$(CXX) -O1 -g1 -std=c++17 -fsanitize=address,undefined -fno-omit-frame-pointer $(INC) tests/test_granular_streams.cpp -o $@

.PHONY: asan-streams
asan-streams: build/test_granular_streams_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./build/test_granular_streams_asan

build/test_meters_asan: tests/test_meters.cpp tests/engine_suite.h src/instrument/*.h src/engines/*.h src/content/*.h src/core/*.h third_party/monojuice/dsp/*.cpp third_party/monojuice/dsp/*.h
	@mkdir -p build
	$(CXX) -O1 -g1 -std=c++17 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread $(INC) tests/test_meters.cpp third_party/monojuice/dsp/ChorusCore.cpp third_party/monojuice/dsp/AudioAdapter.cpp -o $@

.PHONY: asan-meters
asan-meters: build/test_meters_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./build/test_meters_asan

build/test_delay_safety_asan: tests/test_delay_safety.cpp src/instrument/*.h src/core/*.h third_party/monojuice/dsp/*.h
	@mkdir -p build
	$(CXX) -O1 -g1 -std=c++17 -fsanitize=address,undefined -fno-omit-frame-pointer $(INC) tests/test_delay_safety.cpp -o $@

.PHONY: asan-delay
asan-delay: build/test_delay_safety_asan
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./build/test_delay_safety_asan

build/render_demo_fx: tools/render_demo_fx.cpp src/instrument/*.h src/core/*.h third_party/monojuice/dsp/*.h $(MONOJUICE_OBJ)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/render_demo_fx.cpp $(MONOJUICE_OBJ) -o $@

build/render_demo_comb: tools/render_demo_comb.cpp src/instrument/*.h src/core/*.h $(MONOJUICE_OBJ)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/render_demo_comb.cpp $(MONOJUICE_OBJ) -o $@

build/test_chop: tests/test_chop.cpp tests/engine_suite.h src/engines/chop.h src/engines/mip_reader.h src/content/loop_bank.h src/content/sample_bank.h src/engines/vibrato_phase.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tests/test_chop.cpp -o $@

build/render_demo_chop: tools/render_demo_chop.cpp src/engines/chop.h src/content/loop_bank.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/render_demo_chop.cpp -o $@

build/render_demo_instrument: tools/render_demo_instrument.cpp src/instrument/*.h src/engines/*.h src/content/*.h src/core/*.h $(MONOJUICE_OBJ)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/render_demo_instrument.cpp $(MONOJUICE_OBJ) -o $@

build/monojuice_chorus.o: third_party/monojuice/dsp/ChorusCore.cpp third_party/monojuice/dsp/ChorusCore.h third_party/monojuice/dsp/ChorusTables.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -w -c third_party/monojuice/dsp/ChorusCore.cpp -o $@

build/monojuice_adapter.o: third_party/monojuice/dsp/AudioAdapter.cpp third_party/monojuice/dsp/AudioAdapter.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -w -c third_party/monojuice/dsp/AudioAdapter.cpp -o $@

build/verb_ref.o: third_party/dattorro-verb/verb.c
	@mkdir -p build
	$(CC) -O2 -c third_party/dattorro-verb/verb.c -o $@

build/test_fx: tests/test_fx.cpp build/verb_ref.o src/instrument/*.h src/core/*.h third_party/chowdsp_wdf/chowdsp_wdf.h $(MONOJUICE_OBJ)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tests/test_fx.cpp build/verb_ref.o $(MONOJUICE_OBJ) -o $@

build/test_instrument: tests/test_instrument.cpp src/instrument/*.h src/engines/*.h src/content/*.h src/core/*.h $(MONOJUICE_OBJ)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tests/test_instrument.cpp $(MONOJUICE_OBJ) -o $@

build/render_demo_synth: tools/render_demo_synth.cpp src/engines/fm.h src/engines/harmonic.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/render_demo_synth.cpp -o $@

build/render_demo_organics: tools/render_demo_organics.cpp src/engines/organics.h src/engines/mip_reader.h src/engines/sinc_interp.h src/content/sample_bank.h src/engines/vibrato_phase.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/render_demo_organics.cpp -o $@

build/render_demo_resynth: tools/render_demo_resynth.cpp src/engines/resynth.h src/content/resynth_data.h src/content/sample_bank.h src/engines/vibrato_phase.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/render_demo_resynth.cpp -o $@

build/render_demo_modal: tools/render_demo_modal.cpp src/engines/modal.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/render_demo_modal.cpp -o $@

build/render_demo_granular: tools/render_demo_granular.cpp src/engines/granular.h src/engines/engine.h src/engines/mip_reader.h src/engines/sinc_interp.h src/content/sample_bank.h src/engines/vibrato_phase.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/render_demo_granular.cpp -o $@

# Диагностический float WAV напрямую из движка; без нормировки/лимитера.
build/render_granular_raw: tools/render_granular_raw.cpp src/engines/granular.h src/engines/engine.h src/engines/mip_reader.h src/engines/sinc_interp.h src/content/sample_bank.h src/engines/vibrato_phase.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/render_granular_raw.cpp -o $@

build/render_granular_reverse: tools/render_granular_reverse.cpp src/engines/granular.h src/engines/engine.h src/engines/mip_reader.h src/engines/sinc_interp.h src/content/sample_bank.h src/engines/vibrato_phase.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/render_granular_reverse.cpp -o $@

build/bench_granular_reverse: tools/bench_granular_reverse.cpp src/engines/granular.h src/engines/engine.h src/engines/mip_reader.h src/engines/sinc_interp.h src/content/sample_bank.h src/engines/vibrato_phase.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/bench_granular_reverse.cpp -o $@

build/render_granular_transient: tools/render_granular_transient.cpp src/engines/granular.h src/engines/engine.h src/engines/mip_reader.h src/engines/sinc_interp.h src/content/sample_bank.h src/engines/vibrato_phase.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/render_granular_transient.cpp -o $@

build/render_granular_pitch: tools/render_granular_pitch.cpp src/engines/granular.h src/engines/engine.h src/engines/mip_reader.h src/engines/sinc_interp.h src/content/sample_bank.h src/engines/vibrato_phase.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/render_granular_pitch.cpp -o $@

build/render_granular_short: tools/render_granular_short.cpp src/engines/granular.h src/engines/engine.h src/engines/mip_reader.h src/engines/sinc_interp.h src/content/sample_bank.h src/engines/vibrato_phase.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/render_granular_short.cpp -o $@

build/analyze_choir_source: tools/analyze_choir_source.cpp src/content/sample_bank.h src/engines/vibrato_phase.h src/engines/halfband.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/analyze_choir_source.cpp -o $@

build/render_demo_sample: tools/render_demo_sample.cpp src/engines/sample.h src/engines/halfband.h src/content/sample_bank.h src/engines/vibrato_phase.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/render_demo_sample.cpp -o $@

build/render_demo: tools/render_demo.cpp src/engines/wavetable.h src/engines/halfband.h src/content/wavetable_bank.h src/core/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(INC) tools/render_demo.cpp -o $@

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; ./$$t; done

demo: $(DEMO_BINS)
	@for d in $(DEMO_BINS); do ./$$d; done

clean:
	rm -rf build
