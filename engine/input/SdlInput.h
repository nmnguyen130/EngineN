#pragma once

#include "input/InputPipeline.h"

union SDL_Event;

namespace en::input {

void IngestSdlEvent(InputPipeline& pipeline, const SDL_Event& event) noexcept;

} // namespace en::input