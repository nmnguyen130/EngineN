#include "core/Jobs.h"

#include <algorithm>

namespace en {
namespace {
thread_local u32 g_tlsQueueIdx = 0U;
thread_local const JobSystem* g_tlsOwner = nullptr;
} // namespace

bool JobSystem::Init(u32 workerThreads) noexcept {
    Shutdown();
    m_inFlight.store(0U, std::memory_order_relaxed);
    m_stop.store(false, std::memory_order_relaxed);
    g_tlsOwner = this;
    g_tlsQueueIdx = 0U;

    if (workerThreads == 0U) {
        const u32 hw = std::thread::hardware_concurrency();
        workerThreads = (hw > 1U) ? (hw - 1U) : 1U;
    }
    const u32 target = std::min(workerThreads, kMaxWorkers);
    m_workerCount = 0U;
    for (u32 i = 0U; i < target; ++i) {
        const u32 queueIdx = i + 1U;
#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        try {
            m_workers[i] = std::thread([this, queueIdx]() noexcept { WorkerLoop(queueIdx); });
        } catch (...) {
            Shutdown();
            return false;
        }
#else
        m_workers[i] = std::thread([this, queueIdx]() noexcept { WorkerLoop(queueIdx); });
#endif
        m_workerCount = i + 1U;
    }
    return true;
}

void JobSystem::Shutdown() noexcept {
    if (m_workerCount > 0U) {
        {
            std::lock_guard lock(m_wakeMutex);
            m_stop.store(true, std::memory_order_release);
        }
        m_wakeCv.notify_all();
        for (u32 i = 0U; i < m_workerCount; ++i) {
            if (m_workers[i].joinable()) {
                m_workers[i].join();
            }
        }
        m_workerCount = 0U;
    }
    for (auto& dq : m_deques) {
        dq.Clear();
    }
    m_inFlight.store(0U, std::memory_order_relaxed);
}

u32 JobSystem::CallerQueueIndex() const noexcept {
    return (g_tlsOwner == this && g_tlsQueueIdx <= m_workerCount) ? g_tlsQueueIdx : 0U;
}

void JobSystem::RunItem(const JobItem& item, u32 threadIdx) noexcept {
    if (item.fn) {
        item.fn(item.startIdx, item.endIdx, threadIdx, item.userCtx);
    }
    if (item.counter) {
        item.counter->fetch_sub(1U, std::memory_order_acq_rel);
    }
}

bool JobSystem::ExecuteOneJob(u32 callerQueueIdx) noexcept {
    JobItem item{};
    if (m_deques[callerQueueIdx].PopBottom(item)) {
        m_inFlight.fetch_sub(1U, std::memory_order_acq_rel);
        RunItem(item, callerQueueIdx);
        return true;
    }

    const u32 totalQueues = m_workerCount + 1U;
    for (u32 offset = 1U; offset < totalQueues; ++offset) {
        const u32 victim = (callerQueueIdx + offset) % totalQueues;
        if (m_deques[victim].StealTop(item)) {
            m_inFlight.fetch_sub(1U, std::memory_order_acq_rel);
            RunItem(item, callerQueueIdx);
            return true;
        }
    }
    return false;
}

void JobSystem::ParallelFor(u32 itemCount, u32 minBatchSize, TaskFn fn, void* userCtx) noexcept {
    if (itemCount == 0U || !fn) return;

    const u32 batchSize = std::max(1U, minBatchSize);
    const u32 totalBatches = ((itemCount - 1U) / batchSize) + 1U;
    const u32 callerIdx = CallerQueueIndex();

    if (m_workerCount == 0U || totalBatches == 1U || g_tlsOwner != this) {
        fn(0U, itemCount, callerIdx, userCtx);
        return;
    }

    alignas(64) std::atomic<u32> remaining{totalBatches};
    u32 pushed = 0U;

    for (u32 b = 1U; b < totalBatches; ++b) {
        const u32 start = b * batchSize;
        const u32 end = (itemCount - start > batchSize) ? (start + batchSize) : itemCount;
        const JobItem item{fn, userCtx, &remaining, start, end};

        m_inFlight.fetch_add(1U, std::memory_order_release);
        if (m_deques[callerIdx].PushBottom(item)) {
            ++pushed;
        } else {
            m_inFlight.fetch_sub(1U, std::memory_order_relaxed);
            RunItem(item, callerIdx);
        }
    }

    if (pushed > 0U) {
        std::lock_guard lock(m_wakeMutex);
        m_wakeCv.notify_all();
    }

    RunItem({fn, userCtx, &remaining, 0U, std::min(batchSize, itemCount)}, callerIdx);

    while (remaining.load(std::memory_order_acquire) > 0U) {
        if (!ExecuteOneJob(callerIdx)) {
            std::this_thread::yield();
        }
    }
}

void JobSystem::WorkerLoop(u32 queueIdx) noexcept {
    g_tlsOwner = this;
    g_tlsQueueIdx = queueIdx;

    for (;;) {
        while (ExecuteOneJob(queueIdx)) {}

        std::unique_lock lock(m_wakeMutex);
        m_wakeCv.wait(lock, [this]() noexcept {
            return m_stop.load(std::memory_order_acquire) ||
                   m_inFlight.load(std::memory_order_acquire) > 0U;
        });
        if (m_stop.load(std::memory_order_acquire)) return;
    }
}

} // namespace en
