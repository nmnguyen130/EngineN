#include "core/Jobs.h"

#include <algorithm>

namespace en {

namespace {
thread_local u32 g_tlsQueueIndex = 0U;
thread_local const JobSystem* g_tlsOwnerSystem = nullptr;
} // namespace

bool JobSystem::Init(u32 workerThreads) noexcept {
    Shutdown();
    m_inFlightJobs.store(0U, std::memory_order_relaxed);
    m_nextCounterSlot.store(0U, std::memory_order_relaxed);
    m_stop.store(false, std::memory_order_relaxed);
    g_tlsOwnerSystem = this;
    g_tlsQueueIndex = 0U;

    if (workerThreads == 0U) {
        const u32 hw = std::thread::hardware_concurrency();
        workerThreads = (hw > 1U) ? (hw - 1U) : 1U;
    }
    const u32 targetWorkers = std::min(workerThreads, kMaxWorkers);
    m_workerCount = 0U;
    for (u32 i = 0; i < targetWorkers; ++i) {
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
        for (u32 i = 0; i < m_workerCount; ++i) {
            if (m_workers[i].joinable()) {
                m_workers[i].join();
            }
        }
        m_workerCount = 0U;
    }
    for (auto& dq : m_deques) {
        dq.Clear();
    }
    for (auto& c : m_batchCounters) {
        c.remaining.store(0U, std::memory_order_relaxed);
    }
    m_inFlightJobs.store(0U, std::memory_order_relaxed);
}

u32 JobSystem::ResolveCallerQueueIndex() const noexcept {
    return (g_tlsOwnerSystem == this && g_tlsQueueIndex <= m_workerCount) ? g_tlsQueueIndex : 0U;
}

void JobSystem::ExecuteJobItem(const JobItem& item, u32 threadIdx) noexcept {
    if (item.fn != nullptr) {
        item.fn(item.startIdx, item.endIdx, threadIdx, item.userCtx);
    }
    if (item.batchSlot < m_batchCounters.size()) {
        m_batchCounters[item.batchSlot].remaining.fetch_sub(1U, std::memory_order_acq_rel);
    }
}

bool JobSystem::ExecuteOneJob(u32 callerQueueIdx) noexcept {
    JobItem item{};
    if (m_deques[callerQueueIdx].PopBottom(item)) {
        m_inFlightJobs.fetch_sub(1U, std::memory_order_acq_rel);
        ExecuteJobItem(item, callerQueueIdx);
        return true;
    }

    const u32 totalQueues = m_workerCount + 1U;
    for (u32 offset = 1U; offset < totalQueues; ++offset) {
        const u32 victim = (callerQueueIdx + offset) % totalQueues;
        if (m_deques[victim].StealTop(item)) {
            m_inFlightJobs.fetch_sub(1U, std::memory_order_acq_rel);
            ExecuteJobItem(item, callerQueueIdx);
            return true;
        }
    }
    return false;
}

void JobSystem::ParallelFor(u32 itemCount, u32 minBatchSize, ParallelTaskFn fn, void* userCtx) noexcept {
    if (itemCount == 0U || fn == nullptr) {
        return;
    }
    const u32 batchSize = std::max(1U, minBatchSize);
    const u32 totalBatches = ((itemCount - 1U) / batchSize) + 1U;
    const u32 callerQueueIdx = ResolveCallerQueueIndex();

    if (m_workerCount == 0U || totalBatches == 1U || g_tlsOwnerSystem != this) {
        fn(0U, itemCount, callerQueueIdx, userCtx);
        return;
    }

    const u32 counterCount = static_cast<u32>(m_batchCounters.size());
    const u32 baseSlot = m_nextCounterSlot.fetch_add(1U, std::memory_order_relaxed);
    u32 batchSlot = counterCount;
    for (u32 probe = 0U; probe < counterCount; ++probe) {
        const u32 idx = (baseSlot + probe) % counterCount;
        u32 expected = 0U;
        if (m_batchCounters[idx].remaining.compare_exchange_strong(
                expected, totalBatches, std::memory_order_acq_rel)) {
            batchSlot = idx;
            break;
        }
    }

    if (batchSlot >= counterCount) {
        fn(0U, itemCount, callerQueueIdx, userCtx);
        return;
    }

    u32 pushedCount = 0U;
    for (u32 b = 1U; b < totalBatches; ++b) {
        const u32 start = b * batchSize;
        const u32 end = (itemCount - start > batchSize) ? (start + batchSize) : itemCount;
        const JobItem item{fn, userCtx, start, end, batchSlot, 0U};

        m_inFlightJobs.fetch_add(1U, std::memory_order_release);
        if (m_deques[callerQueueIdx].PushBottom(item)) {
            ++pushedCount;
        } else {
            m_inFlightJobs.fetch_sub(1U, std::memory_order_relaxed);
            ExecuteJobItem(item, callerQueueIdx);
        }
    }

    if (pushedCount > 0U) {
        std::lock_guard lock(m_wakeMutex);
        m_wakeCv.notify_all();
    }

    ExecuteJobItem({fn, userCtx, 0U, std::min(batchSize, itemCount), batchSlot, 0U}, callerQueueIdx);

    while (m_batchCounters[batchSlot].remaining.load(std::memory_order_acquire) > 0U) {
        if (!ExecuteOneJob(callerQueueIdx)) {
            std::this_thread::yield();
        }
    }
}

void JobSystem::WorkerLoop(u32 queueIdx) noexcept {
    g_tlsOwnerSystem = this;
    g_tlsQueueIndex = queueIdx;

    for (;;) {
        while (ExecuteOneJob(queueIdx)) {
        }

        std::unique_lock lock(m_wakeMutex);
        m_wakeCv.wait(lock, [this]() noexcept {
            return m_stop.load(std::memory_order_acquire) ||
                   m_inFlightJobs.load(std::memory_order_acquire) > 0U;
        });
        if (m_stop.load(std::memory_order_acquire)) {
            return;
        }
    }
}

} // namespace en
