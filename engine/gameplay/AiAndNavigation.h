#pragma once

#include "core/Containers.h"
#include "core/Math.h"
#include "physics/SpatialIndex2D.h"

namespace en {

enum class BtStatus : u8 {
    Success = 0,
    Failure = 1,
    Running = 2,
};

enum class BtNodeKind : u8 {
    Sequence = 0,
    Selector = 1,
    Inverter = 2,
    Condition = 3,
    Action = 4,
};

struct BtBlackboard {
    Vec2 selfPos{};
    Vec2 targetPos{};
    Vec2 homePos{};
    f32 aggroRange{8.0f};
    f32 attackRange{1.4f};
    f32 hpRatio{1.0f};
    bool hasLineOfSight{false};
    u8 desiredAction{0};
    u8 activeNodeIdx{0};
};

// Flat array-based data-oriented Behavior Tree evaluator with zero heap allocation or virtuals.
class BehaviorTree2D {
public:
    using LeafFn = BtStatus (*)(BtBlackboard& bb) noexcept;
    static constexpr usize kMaxNodes = 32;

    struct Node {
        BtNodeKind kind{BtNodeKind::Sequence};
        u8 firstChild{0xFFU};
        u8 nextSibling{0xFFU};
        LeafFn leafFn{nullptr};
    };

    constexpr void Clear() noexcept { m_nodeCount = 0; }

    [[nodiscard]] constexpr u8 AddNode(BtNodeKind kind, LeafFn fn = nullptr) noexcept {
        if (m_nodeCount >= kMaxNodes) return 0xFFU;
        const u8 idx = m_nodeCount++;
        m_nodes[idx] = Node{kind, 0xFFU, 0xFFU, fn};
        return idx;
    }

    constexpr void AddChild(u8 parentIdx, u8 childIdx) noexcept {
        if (parentIdx >= m_nodeCount || childIdx >= m_nodeCount) return;
        if (m_nodes[parentIdx].firstChild == 0xFFU) {
            m_nodes[parentIdx].firstChild = childIdx;
        } else {
            u8 cur = m_nodes[parentIdx].firstChild;
            while (m_nodes[cur].nextSibling != 0xFFU) cur = m_nodes[cur].nextSibling;
            m_nodes[cur].nextSibling = childIdx;
        }
    }

    [[nodiscard]] BtStatus Evaluate(BtBlackboard& bb, u8 rootIdx = 0) const noexcept;

private:
    std::array<Node, kMaxNodes> m_nodes{};
    u8 m_nodeCount{0};
};

// Bounded A* 2D grid pathfinder, LOS path smoother, and steering behavior suite.
class NavGrid2D {
public:
    static constexpr usize kMaxSearchNodes = 512;
    static constexpr usize kMaxPathWaypoints = 32;

    [[nodiscard]] static u32 FindPathAStar(
        const SpatialIndex2D& spatial,
        Vec2 startWorld,
        Vec2 goalWorld,
        std::span<Vec2> outWaypoints,
        bool allowAir = true) noexcept;

    [[nodiscard]] static u32 SmoothPathLos(
        const SpatialIndex2D& spatial,
        std::span<const Vec2> rawPath,
        std::span<Vec2> outSmoothed) noexcept;

    [[nodiscard]] static Vec2 SteerSeek(Vec2 currentPos, Vec2 targetPos, f32 maxSpeed) noexcept {
        return (targetPos - currentPos).Normalized() * maxSpeed;
    }

    [[nodiscard]] static Vec2 SteerFlee(Vec2 currentPos, Vec2 threatPos, f32 maxSpeed) noexcept {
        return (currentPos - threatPos).Normalized() * maxSpeed;
    }

    [[nodiscard]] static Vec2 SteerArrive(
        Vec2 currentPos, Vec2 targetPos, f32 maxSpeed, f32 slowRadius) noexcept {
        const Vec2 toTarget = targetPos - currentPos;
        const f32 dist = toTarget.Length();
        if (dist < 1e-4f) return {0.0f, 0.0f};
        const f32 speed = (dist < slowRadius && slowRadius > 1e-4f)
                              ? maxSpeed * (dist / slowRadius)
                              : maxSpeed;
        return (toTarget / dist) * speed;
    }
};

// Response-curve Utility AI action scorer for tactical decision making.
class UtilityAiSelector2D {
public:
    using ScoreFn = f32 (*)(const BtBlackboard& bb) noexcept;
    struct Option {
        u8 actionId{0};
        ScoreFn scoreFn{nullptr};
    };

    constexpr void Clear() noexcept { m_count = 0; }
    constexpr bool AddOption(u8 actionId, ScoreFn fn) noexcept {
        if (!fn || m_count >= m_options.size()) return false;
        m_options[m_count++] = Option{actionId, fn};
        return true;
    }

    [[nodiscard]] u8 SelectHighest(const BtBlackboard& bb, f32* outBestScore = nullptr) const noexcept {
        f32 bestScore = -1e30f;
        u8 bestAction = 0;
        for (usize i = 0; i < m_count; ++i) {
            const f32 sc = m_options[i].scoreFn(bb);
            if (sc > bestScore) {
                bestScore = sc;
                bestAction = m_options[i].actionId;
            }
        }
        if (outBestScore) *outBestScore = bestScore;
        return bestAction;
    }

private:
    std::array<Option, 16> m_options{};
    usize m_count{0};
};

// Fast flat binary Decision Tree evaluator for deterministic AI branching.
class DecisionTree2D {
public:
    using ConditionFn = bool (*)(const BtBlackboard& bb) noexcept;
    struct Node {
        ConditionFn condFn{nullptr};
        u8 trueChild{0xFFU};
        u8 falseChild{0xFFU};
        u8 actionId{0};
        bool isLeaf{true};
    };

    constexpr void Clear() noexcept { m_count = 0; }

    [[nodiscard]] constexpr u8 AddActionLeaf(u8 actionId) noexcept {
        if (m_count >= m_nodes.size()) return 0xFFU;
        const u8 idx = static_cast<u8>(m_count++);
        m_nodes[idx] = Node{nullptr, 0xFFU, 0xFFU, actionId, true};
        return idx;
    }

    [[nodiscard]] constexpr u8 AddBranchNode(ConditionFn fn, u8 trueChild, u8 falseChild) noexcept {
        if (!fn || m_count >= m_nodes.size()) return 0xFFU;
        const u8 idx = static_cast<u8>(m_count++);
        m_nodes[idx] = Node{fn, trueChild, falseChild, 0, false};
        return idx;
    }

    [[nodiscard]] u8 Evaluate(const BtBlackboard& bb, u8 rootIdx = 0) const noexcept {
        u8 cur = rootIdx;
        for (usize depth = 0; depth < m_count && cur < m_count; ++depth) {
            const Node& n = m_nodes[cur];
            if (n.isLeaf || !n.condFn) return n.actionId;
            cur = n.condFn(bb) ? n.trueChild : n.falseChild;
        }
        return 0;
    }

private:
    std::array<Node, 32> m_nodes{};
    usize m_count{0};
};

} // namespace en
