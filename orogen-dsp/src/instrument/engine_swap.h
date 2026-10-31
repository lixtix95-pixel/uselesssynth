// Обмен движков между потоками без блокировок.
//
// Задача. Смена типа движка в слоте (A–H) означает создание нового объекта
// (make_unique + setSampleRate): это аллокация, а аллокация в аудио-потоке —
// недопустима (может ждать мьютекс аллокатора, может отдать страницы ядра,
// задержка блока вырастает непредсказуемо).
//
// Схема: готовый объект публикуется атомарным обменом указателя в каждом
// слоте. Аудио-поток забирает его другим atomic exchange, а заменённый
// работающий объект отдаёт в SPSC retire bin для удаления вне аудио-потока.
// Обновление непрочитанного pending-объекта и подхват на аудио-потоке не
// могут владеть одним и тем же указателем. Производители сериализуются вне
// аудио-потока. Если retire bin заполнен, обмен откладывается до следующего
// блока, старые движки продолжают звучать.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace orogen
{

// Счётчики жизни движков. Нужны тестам: «создано == удалено» доказывает, что
// ни один движок не потерялся и не удалён в аудио-потоке.
struct EngineStats
{
    static inline std::atomic<long long> created { 0 };
    static inline std::atomic<long long> retired { 0 };
    static inline std::atomic<long long> deleted { 0 };

    static void reset() { created = 0; retired = 0; deleted = 0; }
    static long long live() { return created.load() - deleted.load(); }
};

// Очередь «на удаление»: producers — аудио-поток (один), consumer — message
// thread (один). Фиксированный размер: 1024 указателя = 8 полных обменов по
// 128 движков (16 голосов × 8 слотов), потом всё равно нечего менять.
class EngineRetireBin
{
public:
    static constexpr int kCapacity = 1024;

    // Сколько ещё поместится.
    int room() const noexcept
    {
        const int tail = tailIdx.load (std::memory_order_relaxed);
        const int head = headIdx.load (std::memory_order_acquire);
        return kCapacity - (tail - head);
    }

    // Только аудио-поток. false — места нет, указатель НЕ принят (владелец
    // остаётся за вызывающим).
    bool push (void* owner) noexcept
    {
        const int tail = tailIdx.load (std::memory_order_relaxed);
        const int head = headIdx.load (std::memory_order_acquire);
        if (tail - head >= kCapacity) return false;
        slots[(size_t) (tail % kCapacity)].store (owner, std::memory_order_relaxed);
        tailIdx.store (tail + 1, std::memory_order_release);
        EngineStats::retired.fetch_add (1, std::memory_order_relaxed);
        return true;
    }

    // Только message thread. Забирает до maxN указателей в out, возвращает
    // их число. Удаляет их вызывающий — очередь не знает типа объектов.
    int drain (void** out, int maxN) noexcept
    {
        const int tail = tailIdx.load (std::memory_order_acquire);
        int head = headIdx.load (std::memory_order_relaxed);
        int n = 0;
        while (head < tail && n < maxN)
        {
            out[n++] = slots[(size_t) (head % kCapacity)].load (std::memory_order_relaxed);
            ++head;
        }
        headIdx.store (head, std::memory_order_release);
        return n;
    }

    int size() const noexcept
    {
        return tailIdx.load (std::memory_order_acquire) - headIdx.load (std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<void*>, kCapacity> slots {};
    alignas (64) std::atomic<int> headIdx { 0 };
    std::atomic<int> tailIdx { 0 };
};

} // namespace orogen
