// Обмен движков между потоками без блокировок.
//
// Задача. Смена типа движка в слоте (A–H) означает создание нового объекта
// (make_unique + setSampleRate): это аллокация, а аллокация в аудио-потоке —
// недопустима (может ждать мьютекс аллокатора, может отдать страницы ядра,
// задержка блока вырастает непредсказуемо).
//
// Схема (single producer / single consumer на каждой стороне):
//   1. message thread строит новый движок в «подготовленном» слоте и ставит бит
//      в pendingMask — аудио-поток в этот момент ещё видит старый движок;
//   2. аудио-поток в начале блока меняет местами unique_ptr (только указатели,
//      без аллокации) и отдаёт старый движок в retire bin;
//   3. message thread забирает из retire bin и удаляет — освобождение памяти
//      уже вне аудио-потока.
//
// Инварианты, на которых это держится:
//   * владелец движка в любой момент ровно один (unique_ptr либо в engines,
//     либо в pending, либо сырой указатель в retire bin);
//   * pendingMask меняется только атомарно (release при записи, acquire при
//     обмене), движок не публикуется до полной готовности;
//   * retire bin фиксированного размера и не переполняется: аудио-поток
//     проверяет место до обмена и, если места нет, откладывает обмен на
//     следующий блок (движки продолжают звучать как прежде).
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
        const int head = headIdx.load (std::memory_order_relaxed);
        return kCapacity - (tail - head);
    }

    // Только аудио-поток. false — места нет, указатель НЕ принят (владелец
    // остаётся за вызывающим).
    bool push (void* owner) noexcept
    {
        const int tail = tailIdx.load (std::memory_order_relaxed);
        const int head = headIdx.load (std::memory_order_relaxed);
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
