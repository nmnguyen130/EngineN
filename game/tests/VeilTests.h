#pragma once

namespace en::veil {

// Runs full regression test suite verifying GDD mechanics on native EngineN ECS.
// Isolated from runtime application logic for clean architecture.
[[nodiscard]] bool RunVeilTestSuite() noexcept;

} // namespace en::veil
