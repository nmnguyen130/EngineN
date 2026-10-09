#include "HitstopEngine.h"

namespace EngineN {

void HitstopEngine::Request(const HitstopRequest& req) {
    size_t idx = static_cast<size_t>(req.channel);
    if (idx < m_remainingFreezeSec.size() && req.freezeDurationSec > 0.0) {
        // Merge by max, do not stack to avoid infinite freeze
        m_remainingFreezeSec[idx] = std::max(m_remainingFreezeSec[idx], req.freezeDurationSec);
    }
}

void HitstopEngine::Update(double realDtSec) {
    for (double& sec : m_remainingFreezeSec) {
        if (sec > 0.0) {
            sec = std::max(0.0, sec - realDtSec);
        }
    }
}

bool HitstopEngine::IsFrozen(HitstopChannel ch) const {
    size_t idx = static_cast<size_t>(ch);
    return (idx < m_remainingFreezeSec.size()) && (m_remainingFreezeSec[idx] > 1e-9);
}

double HitstopEngine::GetModifier(HitstopChannel ch) const {
    return IsFrozen(ch) ? 0.0 : 1.0;
}

} // namespace EngineN
