#pragma once

#include "core/Types.h"

namespace EngineN {

class Platform;
class Window;
class InputSystem;
class TimeManager;
class AssetManager;
class FrameArenaSlot;
class RenderQueue;
class Batcher;
class RenderBackend;
class Presenter;
class SimEvents;

/**
 * EngineContext: Central coordinator references passed to the game loop.
 * Eliminates global variables and enforces single-point access.
 */
struct EngineContext {
    Platform*       platform   = nullptr;
    Window*         window     = nullptr;
    InputSystem*    input      = nullptr;
    TimeManager*    time       = nullptr;
    AssetManager*   assets     = nullptr;
    FrameArenaSlot* arena      = nullptr;
    RenderQueue*    queue      = nullptr;
    Batcher*        batcher    = nullptr;
    RenderBackend*  backend    = nullptr;
    Presenter*      presenter  = nullptr;
    SimEvents*      events     = nullptr;
};

} // namespace EngineN
