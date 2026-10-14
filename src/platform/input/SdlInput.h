#pragma once

#include "platform/input/InputPipeline.h"

union SDL_Event;

namespace EngineN {

using InputSystem = InputPipeline;

void IngestSdlEvent(InputPipeline& pipeline, const SDL_Event& event) noexcept;

} // namespace EngineN