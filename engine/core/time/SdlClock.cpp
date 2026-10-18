#include "core/time/SdlClock.h"

#if __has_include(<SDL3/SDL_timer.h>)
#include <SDL3/SDL_timer.h>
#else
extern "C" en::u64 SDL_GetTicksNS(void) noexcept;
#endif

namespace en::time {

u64 SdlClock::NowNs() const noexcept {
    return SDL_GetTicksNS();
}

} // namespace en::time
