#include "platform/input/SdlInput.h"

#include <SDL3/SDL_events.h>

namespace en::input {

static_assert(
    static_cast<int>(GamepadAxis::LeftX) - 1 == SDL_GAMEPAD_AXIS_LEFTX &&
    static_cast<int>(GamepadAxis::RightTrigger) - 1 == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER &&
    6 == SDL_GAMEPAD_AXIS_COUNT,
    "GamepadAxis enum must match SDL3 gamepad axis layout"
);

void IngestSdlEvent(InputPipeline& pipeline, const SDL_Event& event) noexcept {
    switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (!event.key.repeat) {
                pipeline.FeedButton(Source::Key(static_cast<u16>(event.key.scancode)), event.key.down);
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.which != SDL_TOUCH_MOUSEID) {
                pipeline.FeedButton(Source::Mouse(event.button.button), event.button.down);
            }
            break;

        case SDL_EVENT_MOUSE_MOTION:
            if (event.motion.which != SDL_TOUCH_MOUSEID) {
                pipeline.FeedMouseMotion(
                    Vec2{event.motion.x, event.motion.y},
                    Vec2{event.motion.xrel, event.motion.yrel});
            }
            break;

        case SDL_EVENT_MOUSE_WHEEL:
            if (event.wheel.which != SDL_TOUCH_MOUSEID) {
                const f32 flip = (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) ? -1.0f : 1.0f;
                pipeline.FeedMouseWheel(Vec2{event.wheel.x * flip, event.wheel.y * flip});
            }
            break;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            pipeline.FeedGamepadButton(
                static_cast<u32>(event.gbutton.which),
                event.gbutton.button,
                event.gbutton.down);
            break;

        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            pipeline.FeedGamepadAxis(
                static_cast<u32>(event.gaxis.which),
                event.gaxis.axis,
                event.gaxis.value);
            break;

        case SDL_EVENT_GAMEPAD_REMOVED:
            pipeline.DisconnectGamepad(static_cast<u32>(event.gdevice.which));
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            pipeline.ClearPhysicalState();
            break;

        default:
            break;
    }
}

} // namespace en::input