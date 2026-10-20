#include "core/JobSystem.h"
#include <algorithm>

namespace en {

bool JobSystem::Init(u32 workerThreads) noexcept {
    Shutdown();
    if (workerThreads == 0) return true;
    m_stop.store(false, std::memory_order_relaxed);
    m_epoch.store(0, std::memory_order_relaxed);
    const u32 clamped = std::min(workerThreads, 16U);
    m_workers.reserve(clamped);
    for (u32 i = 0; i < clamped; ++i) {
        m_workers.emplace_back([this, i]() noexcept { WorkerLoop(i + 1U); });
    }
    return true;
}

void JobSystem::Shutdown() noexcept {
    if (m_workers.empty()) return;
    {
        std::lock_guard lock(m_mutex);
        m_stop.store(true, std::memory_order_release);
    }
    m_cvWork.notify_all();
    for (auto& w : m_workers) {
        if (w.joinable()) w.join();
    }
    m_workers.clear();
}

void JobSystem::ParallelFor(u32 itemCount, u32 minBatchSize, ParallelTaskFn fn, void* userCtx) noexcept {
    if (itemCount == 0 || fn == nullptr) return;
    const u32 batchSize = std::max(1U, minBatchSize);
    const u32 totalBatches = (itemCount + batchSize - 1U) / batchSize;

    if (m_workers.empty() || totalBatches == 1U) {
        fn(0, itemCount, 0, userCtx);
        return;
    }

    {
        std::lock_guard lock(m_mutex);
        m_activeDispatch = BatchDispatch{fn, userCtx, itemCount, batchSize, totalBatches};
        m_nextBatch.store(0, std::memory_order_relaxed);
        m_remainingBatches.store(totalBatches, std::memory_order_release);
        m_epoch.fetch_add(1ULL, std::memory_order_release);
    }
    m_cvWork.notify_all();

    for (;;) {
        const u32 b = m_nextBatch.fetch_add(1U, std::memory_order_relaxed);
        if (b >= totalBatches) break;
        const u32 start = b * batchSize;
        const u32 end = std::min(start + batchSize, itemCount);
        fn(start, end, 0, userCtx);
        if (m_remainingBatches.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
            std::lock_guard lock(m_mutex);
            m_cvDone.notify_all();
            return;
        }
    }

    std::unique_lock lock(m_mutex);
    m_cvDone.wait(lock, [this]() noexcept {
        return m_remainingBatches.load(std::memory_order_acquire) == 0U;
    });
}

void JobSystem::WorkerLoop(u32 workerIdx) noexcept {
    u64 seenEpoch = 0;
    for (;;) {
        {
            std::unique_lock lock(m_mutex);
            m_cvWork.wait(lock, [this, &seenEpoch]() noexcept {
                return m_stop.load(std::memory_order_acquire) ||
                       m_epoch.load(std::memory_order_acquire) != seenEpoch;
            });
            if (m_stop.load(std::memory_order_acquire)) return;
            seenEpoch = m_epoch.load(std::memory_order_acquire);
        }

        const u32 totalBatches = m_activeDispatch.totalBatches;
        const u32 batchSize = m_activeDispatch.batchSize;
        const u32 itemCount = m_activeDispatch.itemCount;
        const ParallelTaskFn fn = m_activeDispatch.fn;
        void* userCtx = m_activeDispatch.userCtx;

        for (;;) {
            const u32 b = m_nextBatch.fetch_add(1U, std::memory_order_relaxed);
            if (b >= totalBatches) break;
            const u32 start = b * batchSize;
            const u32 end = std::min(start + batchSize, itemCount);
            fn(start, end, workerIdx, userCtx);
            if (m_remainingBatches.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
                std::lock_guard lock(m_mutex);
                m_cvDone.notify_all();
                break;
            }
        }
    }
}

} // namespace en
