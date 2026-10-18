#include "platform/Platform.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>
#include <cassert>
#include <cstdio>

namespace {

using namespace en;
using namespace en::platform;

// 1. Compile-time Struct Size & Cacheline Alignment Invariants
static_assert(sizeof(Extent2i) == 8 && sizeof(WindowState) == 32 && alignof(WindowState) == 32);
static_assert(sizeof(Window) == 64 && alignof(Window) == 64);
static_assert(sizeof(PlatformInputEvent) == 4 && sizeof(PlatformFrameEvents) == 256);
static_assert(sizeof(Platform) == 2368 && alignof(Platform) == 64);
static_assert(sizeof(FilePath) == 1024 && alignof(FilePath) == 64);

// 2. Compile-time Sandboxing & Frame Event Coalescing
consteval bool VerifyConstexprLayer1() noexcept {
    FilePath root{};
    if (!root.AssignNormalizedDir("GameRoot\\\\bin\\") || root.View() != "GameRoot/bin/") return false;

    // Valid sandboxed resolution
    const auto hit = root.Resolve("assets\\audio/hit.eaud");
    if (!hit || hit->View() != "GameRoot/bin/assets/audio/hit.eaud") return false;

    // Strict rejection: traversal, escaping absolute paths, empty, stream injection
    if (root.Resolve("").has_value()) return false;
    if (root.Resolve("../secret.txt").has_value()) return false;
    if (root.Resolve("/abs/file.txt").has_value()) return false;
    if (root.Resolve("C:/file.txt").has_value()) return false;
    if (root.Resolve("file.txt:$DATA").has_value()) return false;

    // Frame events: gamepad axis deduplication (same pad slot + axis overrides value)
    PlatformFrameEvents fe{};
    fe.PushEvent(InputEventKind::GamepadAxis, 0, 0, 100);
    fe.PushEvent(InputEventKind::GamepadAxis, 0, 0, 200);
    return (fe.eventCount == 1 && fe.events[0].value == 200);
}
static_assert(VerifyConstexprLayer1());

// Mock SDL state for deterministic headless simulation
struct {
    u32 pollIdx{0};
    u32 pollCount{0};
    SDL_Event events[32]{};
} g_mock{};

struct MockSink {
    u32 frames{0};
    PlatformFrameEvents last{};
    constexpr void IngestPlatformFrame(const PlatformFrameEvents& f) noexcept { ++frames; last = f; }
};

} // namespace

// Minimal headless SDL stubs (only the 7 calls required for Window + Pump simulation)
extern "C" {
bool SDL_Init(SDL_InitFlags) noexcept { return true; }
void SDL_Quit() noexcept {}
const char* SDL_GetBasePath() noexcept { return "GameRoot/bin/"; }
char* SDL_GetPrefPath(const char*, const char*) noexcept { static char s[] = "GameRoot/save/"; return s; }
void SDL_free(void*) noexcept {}
SDL_Window* SDL_CreateWindow(const char*, int, int, SDL_WindowFlags) noexcept { static int h = 1; return reinterpret_cast<SDL_Window*>(&h); }
void SDL_DestroyWindow(SDL_Window*) noexcept {}
SDL_WindowID SDL_GetWindowID(SDL_Window*) noexcept { return 1u; }
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window*) noexcept { return SDL_WINDOW_INPUT_FOCUS; }
bool SDL_GetWindowRelativeMouseMode(SDL_Window*) noexcept { return false; }
bool SDL_GetWindowSize(SDL_Window*, int* w, int* h) noexcept { if (w) *w = 1920; if (h) *h = 1080; return true; }
bool SDL_GetWindowSizeInPixels(SDL_Window*, int* w, int* h) noexcept { if (w) *w = 1920; if (h) *h = 1080; return true; }
float SDL_GetWindowDisplayScale(SDL_Window*) noexcept { return 1.0f; }
float SDL_GetWindowPixelDensity(SDL_Window*) noexcept { return 1.0f; }
const SDL_DisplayMode* SDL_GetCurrentDisplayMode(SDL_DisplayID) noexcept { return nullptr; }
SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window*) noexcept { return 1u; }
bool SDL_PollEvent(SDL_Event* ev) noexcept {
    if (g_mock.pollIdx < g_mock.pollCount) { if (ev) *ev = g_mock.events[g_mock.pollIdx++]; return true; }
    return false;
}
} // extern "C"

int main() {
    Platform platform{};
    assert(platform.Init("EngineN", "Org").has_value());
    assert(platform.BasePath() == "GameRoot/bin/");
    assert(platform.PrefPath() == "GameRoot/save/");

    Window window{};
    assert(window.Create({.title = "TestWindow", .size = {1280, 720}}).has_value());

    // 1. High-Frequency Mouse Motion & Wheel Coalescing
    // Simulate 5 mouse motions (10, 5) -> (50, 25), with dx=2, dy=1 each
    for (int i = 1; i <= 5; ++i) {
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.windowID = 1u;
        e.motion.x = static_cast<float>(i * 10);
        e.motion.y = static_cast<float>(i * 5);
        e.motion.xrel = 2.0f;
        e.motion.yrel = 1.0f;
        g_mock.events[g_mock.pollCount++] = e;
    }
    // Simulate 2 wheel ticks
    for (int i = 0; i < 2; ++i) {
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.windowID = 1u;
        e.wheel.y = 1.5f;
        g_mock.events[g_mock.pollCount++] = e;
    }

    MockSink sink{};
    platform.Pump(window, sink);

    assert(sink.frames == 1);
    assert(sink.last.hasMouseMotion);
    assert(sink.last.mouseX == 50.0f && sink.last.mouseY == 25.0f); // Final position
    assert(sink.last.mouseDx == 10.0f && sink.last.mouseDy == 5.0f); // Accumulated delta
    assert(sink.last.wheelY == 3.0f);                               // Accumulated wheel

    // 2. Focus Loss Anti-Stuck Key Safety
    g_mock.pollIdx = 0;
    g_mock.pollCount = 0;
    SDL_Event focusLoss{};
    focusLoss.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    focusLoss.window.windowID = 1u;
    g_mock.events[g_mock.pollCount++] = focusLoss;

    const WindowFlags flags = platform.Pump(window, sink);
    assert(HasFlag(flags, WindowFlags::FocusLost));
    assert(sink.last.focusCleared);
    assert(sink.last.eventCount == 0);

    window.Destroy();
    platform.Shutdown();

    std::printf("All simplified Platform tests passed successfully.\n");
    return 0;
}
