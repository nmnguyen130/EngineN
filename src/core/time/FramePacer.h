#pragma once

#include <cstdint>

namespace EngineN {

class FramePacer {
public:
    void SetTargetFps(int fps);
    [[nodiscard]] int GetTargetFps() const { return m_targetFps; }
    [[nodiscard]] uint64_t GetTargetFrameNs() const { return m_targetFrameNs; }
    [[nodiscard]] uint64_t GetTargetDeadlineNs() const { return m_targetDeadlineNs; }

    void WaitForTarget();
    void Reset();

private:
    int m_targetFps = 0; // 0 = uncapped
    uint64_t m_targetFrameNs = 0;
    uint64_t m_targetDeadlineNs = 0;
};

} // namespace EngineN
