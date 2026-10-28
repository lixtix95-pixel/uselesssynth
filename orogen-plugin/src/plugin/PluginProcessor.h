// OROGEN — VST3-обёртка над DSP-ядром (orogen-dsp).
//
// Разделение ответственности:
//   * DSP (движки, голос, патчер, эффекты) — целиком в orogen-dsp, ни одной
//     строки JUCE, и это проверено стендами;
//   * здесь только: параметры хоста (APVTS), MIDI, задержка, пресеты, шины.
//
// Значения параметров идут в Patch/FxChain::Settings, а не наоборот: структуры —
// источник истины, параметры — их автоматизируемое зеркало. Поэтому пресет,
// загруженный из JSON, звучит ровно так же, как в офлайн-рендере.
//
// ВАЖНО про аудио-поток: все указатели на параметры кэшируются в конструкторе.
// В processBlock нет ни одной операции с juce::String и ни одной аллокации
// (кроме случая смены типа движка в слоте — см. комментарий в processBlock).
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "instrument/instrument.h"
#include "Params.h"
#include "PresetJson.h"

#include <array>
#include <atomic>
#include <vector>

namespace orogen
{

class OrogenProcessor : public juce::AudioProcessor,
                        private juce::AsyncUpdater,
                        private juce::Timer
{
public:
    OrogenProcessor();
    ~OrogenProcessor() override = default;

    // --- AudioProcessor ---
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    using juce::AudioProcessor::processBlock;   // не скрывать processBlock(double)
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "OROGEN"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 12.0; }

    int getNumPrograms() override { return (int) presets.size(); }
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // --- прикладное ---
    juce::AudioProcessorValueTreeState apvts;

    // Собрать Patch + FxChain::Settings из текущих значений параметров.
    // Вызывается из аудио-потока: без аллокаций, указатели уже закэшированы.
    void collectPatch (Patch& p, FxChain::Settings& fx) const noexcept;
    // Записать структуры в параметры (message thread, после загрузки пресета).
    void applyToParams (const Patch& p, const FxChain::Settings& fx);

    juce::StringArray presetNames() const;
    void loadPreset (int index);
    bool loadPresetFile (const juce::File& f);
    bool savePresetFile (const juce::File& f, const juce::String& name) const;

    // Банк: все программы (имя + патч + настройки FX) в одном JSON-файле.
    // Нужен потому, что changeProgramName() меняет имя только в памяти, а
    // хостовый «Save preset» сохраняет один патч, а не набор.
    juce::var bankToVar() const;
    bool saveBankFile (const juce::File& f) const;
    bool loadBankFile (const juce::File& f);
    int  addProgram (const juce::String& name);      // копия текущих параметров
    const Patch& currentPatch() const { return patch; }

    Instrument& engine() { return inst; }
    const Instrument& engine() const { return inst; }
    int reportedLatency() const { return latencySamples.load(); }
    // Длительность последнего блока в секундах — для замеров нагрузки.
    double lastBlockSeconds() const { return blockSeconds.load(); }
    // Сколько сэмплов реально отрендерено (для стендов).
    long long renderedSamples() const { return rendered.load(); }

private:
    void handleAsyncUpdate() override;      // setLatencySamples() из message thread
    // message thread, 30 Гц: готовит движки для изменившихся типов слотов и
    // убирает заменённые. Аудио-поток только подхватывает готовое (без аллокаций).
    void timerCallback() override;
    void renderRange (juce::AudioBuffer<float>& buf, int start, int num);
    void cachePointers();
    // Хэш всех значений параметров: нужен, чтобы не дёргать setPatch()/setFx()
    // каждый блок. Это не микрооптимизация — FxChain::set() переустанавливает
    // размеры линий задержки и зеркал реверба, и вызов «на всякий случай»
    // каждый блок даёт другой звук, чем однократная установка.
    std::uint64_t hashParams() const noexcept;
    std::uint64_t slotTypeHash() const noexcept;

    struct SlotRefs
    {
        std::atomic<float>* type = nullptr;
        std::atomic<float>* on = nullptr;
        std::atomic<float>* filter = nullptr;
        std::vector<std::atomic<float>*> f;     // по порядку slotParamTable
    };

    EngineBank bank;
    Instrument inst;
    Patch patch = patchInit();
    FxChain::Settings fxSettings;

    struct Preset { juce::String name; Patch patch; FxChain::Settings fx; };
    std::vector<Preset> presets;
    int currentProgram = 0;

    // кэш указателей на параметры
    std::array<SlotRefs, kNumSlots> slotRefs;
    // gain master glide width mode modWheel morph — ровно 7, порядок = индексы в collectPatch()
    std::array<std::atomic<float>*, 7> globRefs { };
    std::array<std::array<std::atomic<float>*, 4>, 2> lfoRefs { };
    std::vector<std::atomic<float>*> fxRefs;                    // по порядку fxParamTable
    // Для AudioParameterChoice/Bool/Int getRawParameterValue() возвращает
    // nullptr (это API только для параметров, созданных самим APVTS), поэтому
    // перечисления читаются через RangedAudioParameter::convertFrom0to1.
    std::array<juce::RangedAudioParameter*, 14> fxChoiceRefs { };

    // арпеджиатор: 5 перечислений (on, mode, div, octaves, latch) + 2 float
    std::array<juce::RangedAudioParameter*, 5> arpRefs { };
    // мод-матрица: на маршрут — два перечисления и один float
    struct ModRefs { juce::RangedAudioParameter* src = nullptr;
                     juce::RangedAudioParameter* dst = nullptr;
                     std::atomic<float>* amt = nullptr; };
    std::array<ModRefs, kModRoutes> modRefs { };
    std::atomic<float>* arpGateRef = nullptr;
    std::atomic<float>* arpSwingRef = nullptr;

    static float choiceValue (juce::RangedAudioParameter* p) noexcept
    {
        return p != nullptr ? p->convertFrom0to1 (p->getValue()) : 0.0f;
    }

    std::uint64_t lastParamHash = 0;
    std::uint64_t stagedTypeHash = 0;
    std::atomic<int> latencySamples { 0 };
    std::atomic<double> blockSeconds { 0.0 };
    std::atomic<long long> rendered { 0 };
    std::atomic<float> pitchBend { 0.0f };                      // −1…+1, от MIDI

    double currentSampleRate = 48000.0;
    float masterGain = 0.8f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrogenProcessor)
};

} // namespace orogen
