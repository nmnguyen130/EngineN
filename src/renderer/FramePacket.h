#pragma once

#include "core/Types.h"
#include "renderer/RendererTypes.h"
#include "renderer/QuadInstance.h"
#include <span>

namespace EngineN {

/**
 * DrawBatch: Coalesced command for the GPU backend.
 * Represents a contiguous range of QuadInstances with identical GPU pipeline state.
 */
struct DrawBatch {
    u32 instanceOffset{0};  // Offset into the final instance buffer
    u32 instanceCount{0};   // Number of instances to draw
    u16 textureSlot{0};     // Generational texture index
    u16 pipelineId{0};      // PSO / Material identifier
    u8  blendMode{0};       // Blend mode index
    u8  passId{0};          // RenderPass index
    u8  viewSlot{0};        // View slot uniform index
    u8  reserved{0};

    constexpr auto operator<=>(const DrawBatch&) const = default;
};

/**
 * ViewSlotDesc: Camera / layer view uniform pushed when viewSlot changes.
 */
struct ViewSlotDesc {
    f32 viewProjection[16]{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    f32 viewport[4]{0.0f, 0.0f, 1280.0f, 720.0f};
};

/**
 * Static pass description in the static pass table.
 */
struct RenderPassDesc {
    RenderPass pass{RenderPass::Scene};
    bool       enabled{true};
    u8         reserved[2]{0, 0};
    Color      clearColor{Colors::Black};
};

/**
 * Per-frame telemetry and audit metrics.
 */
struct FrameStats {
    u32  instanceCount{0};
    u32  batchCount{0};
    u32  sortEntryCount{0};
    u32  viewSlotCount{0};
    u32  droppedInstances{0};
    bool wasSorted{false};
};

/**
 * FramePacket: Immutable frame submission passed across the presentation seam.
 * Backend executes this packet in a single Execute(const FramePacket&) call.
 */
struct FramePacket {
    std::span<const DrawBatch>    batches{};
    std::span<const QuadInstance> instances{};
    std::span<const ViewSlotDesc> viewSlots{};
    std::span<const RenderPassDesc> passes{};
    FrameStats                    stats{};
};

} // namespace EngineN
