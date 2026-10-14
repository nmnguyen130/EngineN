#pragma once

#include "platform/input/InputPipeline.h"

union SDL_Event;

namespace en::input {

using InputSystem = InputPipeline;

void IngestSdlEvent(InputPipeline& pipeline, const SDL_Event& event) noexcept;

} // namespace en::input