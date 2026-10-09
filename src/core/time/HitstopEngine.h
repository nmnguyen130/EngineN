#pragma once

#include <cstdint>
#include <algorithm>
#include <array>

namespace EngineN {

enum class HitstopChannel : uint8_t {
    World = 0,
    Player = 1,
    Enemy = 2,
    Count = 3
};

struct HitstopRequest {
    HitstopChannel channel = HitstopChannel::World;
    double freezeDurationSec = 0.0;
};

class HitstopEngine {
public:
    void Request(const HitstopRequest& req);
    void Update(double realDtSec);
    [[nodiscard]] double GetModifier(HitstopChannel ch) const;
    [[nodiscard]] bool IsFrozen(HitstopChannel ch) const;

private:
    std::array<double, static_cast<size_t>(HitstopChannel::Count)> m_remainingFreezeSec{};
};

} // namespace EngineN
