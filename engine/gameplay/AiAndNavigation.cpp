#include "gameplay/AiAndNavigation.h"
#include <algorithm>
#include <cmath>

namespace en {

BtStatus BehaviorTree2D::Evaluate(BtBlackboard& bb, u8 rootIdx) const noexcept {
    if (rootIdx >= m_nodeCount) return BtStatus::Failure;
    const Node& n = m_nodes[rootIdx];
    switch (n.kind) {
        case BtNodeKind::Condition:
        case BtNodeKind::Action:
            bb.activeNodeIdx = rootIdx;
            return n.leafFn ? n.leafFn(bb) : BtStatus::Failure;
        case BtNodeKind::Inverter: {
            if (n.firstChild == 0xFFU) return BtStatus::Failure;
            const BtStatus s = Evaluate(bb, n.firstChild);
            if (s == BtStatus::Success) return BtStatus::Failure;
            if (s == BtStatus::Failure) return BtStatus::Success;
            return BtStatus::Running;
        }
        case BtNodeKind::Sequence: {
            u8 child = n.firstChild;
            while (child != 0xFFU) {
                const BtStatus s = Evaluate(bb, child);
                if (s != BtStatus::Success) return s;
                child = m_nodes[child].nextSibling;
            }
            return BtStatus::Success;
        }
        case BtNodeKind::Selector: {
            u8 child = n.firstChild;
            while (child != 0xFFU) {
                const BtStatus s = Evaluate(bb, child);
                if (s != BtStatus::Failure) return s;
                child = m_nodes[child].nextSibling;
            }
            return BtStatus::Failure;
        }
    }
    return BtStatus::Failure;
}

u32 NavGrid2D::FindPathAStar(
    const SpatialIndex2D& spatial, Vec2 startWorld, Vec2 goalWorld,
    std::span<Vec2> outWaypoints, bool /*allowAir*/) noexcept {
    if (outWaypoints.empty()) return 0;
    const f32 ts = spatial.TileSize();
    const Vec2 orig = spatial.Origin();
    const auto toCell = [&](Vec2 w) noexcept -> IVec2 {
        return {
            std::clamp(static_cast<i32>(std::floor((w.x - orig.x) / ts)), 0, static_cast<i32>(spatial.MapWidth()) - 1),
            std::clamp(static_cast<i32>(std::floor((w.y - orig.y) / ts)), 0, static_cast<i32>(spatial.MapHeight()) - 1),
        };
    };
    const auto toWorldCenter = [&](IVec2 c) noexcept -> Vec2 {
        return {orig.x + (static_cast<f32>(c.x) + 0.5f) * ts,
                orig.y + (static_cast<f32>(c.y) + 0.5f) * ts};
    };

    const IVec2 startCell = toCell(startWorld);
    const IVec2 goalCell = toCell(goalWorld);
    if (startCell == goalCell) {
        outWaypoints[0] = goalWorld;
        return 1;
    }

    struct SearchNode {
        IVec2 cell{};
        u16 parentIdx{0xFFFFU};
        f32 gCost{0.0f};
        f32 fCost{0.0f};
        bool closed{false};
    };
    std::array<SearchNode, kMaxSearchNodes> nodes{};
    u16 nodeCount = 0;

    const auto heuristic = [](IVec2 a, IVec2 b) noexcept -> f32 {
        return static_cast<f32>(std::abs(a.x - b.x) + std::abs(a.y - b.y));
    };

    nodes[nodeCount++] = SearchNode{startCell, 0xFFFFU, 0.0f, heuristic(startCell, goalCell), false};
    u16 goalNodeIdx = 0xFFFFU;
    u16 bestFallbackIdx = 0;

    for (u32 step = 0; step < 256U; ++step) {
        u16 currentIdx = 0xFFFFU;
        f32 lowestF = 1e30f;
        for (u16 i = 0; i < nodeCount; ++i) {
            if (!nodes[i].closed && nodes[i].fCost < lowestF) {
                lowestF = nodes[i].fCost;
                currentIdx = i;
            }
        }
        if (currentIdx == 0xFFFFU) break;
        nodes[currentIdx].closed = true;

        if (nodes[currentIdx].cell == goalCell) {
            goalNodeIdx = currentIdx;
            break;
        }
        if (heuristic(nodes[currentIdx].cell, goalCell) <
            heuristic(nodes[bestFallbackIdx].cell, goalCell)) {
            bestFallbackIdx = currentIdx;
        }

        constexpr IVec2 kDirs[4] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        const IVec2 curCell = nodes[currentIdx].cell;
        for (const IVec2 d : kDirs) {
            const IVec2 next{curCell.x + d.x, curCell.y + d.y};
            if (next.x < 0 || next.y < 0 ||
                next.x >= spatial.MapWidth() || next.y >= spatial.MapHeight()) {
                continue;
            }
            const TileType tt = spatial.GetTile(next.x, next.y);
            if (tt == TileType::Solid || tt == TileType::SpikeHazard) continue;

            const f32 newG = nodes[currentIdx].gCost + 1.0f;
            u16 existingIdx = 0xFFFFU;
            for (u16 i = 0; i < nodeCount; ++i) {
                if (nodes[i].cell == next) {
                    existingIdx = i;
                    break;
                }
            }
            if (existingIdx != 0xFFFFU) {
                if (!nodes[existingIdx].closed && newG < nodes[existingIdx].gCost) {
                    nodes[existingIdx].gCost = newG;
                    nodes[existingIdx].fCost = newG + heuristic(next, goalCell);
                    nodes[existingIdx].parentIdx = currentIdx;
                }
            } else if (nodeCount < kMaxSearchNodes) {
                nodes[nodeCount++] = SearchNode{
                    next, currentIdx, newG, newG + heuristic(next, goalCell), false};
            }
        }
    }

    u16 traceIdx = (goalNodeIdx != 0xFFFFU) ? goalNodeIdx : bestFallbackIdx;
    std::array<Vec2, kMaxPathWaypoints> reversed{};
    u32 revCount = 0;
    while (traceIdx != 0xFFFFU && revCount < kMaxPathWaypoints) {
        reversed[revCount++] = (traceIdx == goalNodeIdx) ? goalWorld : toWorldCenter(nodes[traceIdx].cell);
        const u16 p = nodes[traceIdx].parentIdx;
        if (p == 0xFFFFU) break;
        traceIdx = p;
    }

    const u32 outCount = std::min<u32>(revCount, static_cast<u32>(outWaypoints.size()));
    for (u32 i = 0; i < outCount; ++i) {
        outWaypoints[i] = reversed[revCount - 1U - i];
    }
    return outCount;
}

u32 NavGrid2D::SmoothPathLos(
    const SpatialIndex2D& spatial,
    std::span<const Vec2> rawPath,
    std::span<Vec2> outSmoothed) noexcept {
    if (rawPath.empty() || outSmoothed.empty()) return 0;
    u32 outCount = 0;
    outSmoothed[outCount++] = rawPath[0];
    usize anchor = 0;
    while (anchor + 1U < rawPath.size() && outCount < outSmoothed.size()) {
        usize furthest = anchor + 1U;
        for (usize test = rawPath.size() - 1U; test > anchor + 1U; --test) {
            const Vec2 delta = rawPath[test] - rawPath[anchor];
            const f32 dist = delta.Length();
            const RayHit2D hit = spatial.Raycast(rawPath[anchor], delta, dist, 1U);
            if (!hit.hit) {
                furthest = test;
                break;
            }
        }
        outSmoothed[outCount++] = rawPath[furthest];
        anchor = furthest;
    }
    return outCount;
}

} // namespace en
