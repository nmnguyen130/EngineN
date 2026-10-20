#pragma once

#include "core/Types.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace en {

// Directed Acyclic Graph (DAG) task scheduler with topological dependency resolution.
class TaskGraph {
public:
    using TaskFn = void (*)(void* userCtx) noexcept;
    static constexpr usize kMaxTasks = 32;

    struct Node {
        TaskFn fn{nullptr};
        void* userCtx{nullptr};
        u32 dependencyMask{0};
    };

    constexpr void Clear() noexcept { m_count = 0; }

    [[nodiscard]] constexpr u8 AddTask(TaskFn fn, void* userCtx = nullptr, u32 depMask = 0) noexcept {
        if (m_count >= kMaxTasks) return 0xFFU;
        const u8 id = m_count++;
        m_nodes[id] = Node{fn, userCtx, depMask};
        return id;
    }

    constexpr void AddDependency(u8 taskIdx, u8 dependsOnIdx) noexcept {
        if (taskIdx < m_count && dependsOnIdx < m_count && taskIdx != dependsOnIdx) {
            m_nodes[taskIdx].dependencyMask |= (1U << dependsOnIdx);
        }
    }

    u32 ExecuteTopological() noexcept {
        u32 completedMask = 0;
        u32 executedCount = 0;
        const u32 targetMask = (m_count == 32) ? 0xFFFFFFFFU : ((1U << m_count) - 1U);
        for (u32 pass = 0; pass < m_count && completedMask != targetMask; ++pass) {
            for (u8 i = 0; i < m_count; ++i) {
                const u32 bit = (1U << i);
                if ((completedMask & bit) != 0) continue;
                if ((m_nodes[i].dependencyMask & completedMask) == m_nodes[i].dependencyMask) {
                    if (m_nodes[i].fn) m_nodes[i].fn(m_nodes[i].userCtx);
                    completedMask |= bit;
                    ++executedCount;
                }
            }
        }
        return executedCount;
    }

private:
    std::array<Node, kMaxTasks> m_nodes{};
    u8 m_count{0};
};

// Multi-threaded work-stealing batch job dispatcher with deterministic single-thread fallback.
class alignas(64) JobSystem {
public:
    using ParallelTaskFn = void (*)(u32 startIdx, u32 endIdx, u32 threadIdx, void* userCtx) noexcept;

    JobSystem() noexcept = default;
    ~JobSystem() noexcept { Shutdown(); }

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    bool Init(u32 workerThreads = 0) noexcept;
    void Shutdown() noexcept;

    void ParallelFor(u32 itemCount, u32 minBatchSize, ParallelTaskFn fn, void* userCtx = nullptr) noexcept;

    template <typename Callable>
    void ParallelForRange(u32 itemCount, u32 minBatchSize, Callable&& callable) noexcept {
        struct Thunk {
            Callable* fn;
            static void Run(u32 s, u32 e, u32 t, void* ctx) noexcept {
                (*static_cast<Thunk*>(ctx)->fn)(s, e, t);
            }
        } thunk{&callable};
        ParallelFor(itemCount, minBatchSize, &Thunk::Run, &thunk);
    }

    [[nodiscard]] u32 WorkerCount() const noexcept { return static_cast<u32>(m_workers.size()); }
    [[nodiscard]] bool IsDeterministicSingleThread() const noexcept { return m_workers.empty(); }

private:
    void WorkerLoop(u32 workerIdx) noexcept;

    struct BatchDispatch {
        ParallelTaskFn fn{nullptr};
        void* userCtx{nullptr};
        u32 itemCount{0};
        u32 batchSize{1};
        u32 totalBatches{0};
    };

    std::vector<std::thread> m_workers{};
    std::mutex m_mutex{};
    std::condition_variable m_cvWork{};
    std::condition_variable m_cvDone{};
    BatchDispatch m_activeDispatch{};
    std::atomic<u32> m_nextBatch{0};
    std::atomic<u32> m_remainingBatches{0};
    std::atomic<u64> m_epoch{0};
    std::atomic<bool> m_stop{false};
};

} // namespace en
