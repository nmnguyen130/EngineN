#include "input/InputPipeline.h"
#include "platform/Platform.h"
#include "platform/PlatformTypes.h"
#include "platform/Window.h"

#include <SDL3/SDL_events.h>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string_view>

using namespace en;
using namespace en::platform;

static u32 g_failedAssertions = 0;
static u32 g_passedAssertions = 0;

namespace {

void AssertTrue(bool condition, std::string_view expression, std::source_location loc = std::source_location::current()) {
    if (!condition) {
        std::cerr << "[FAIL] " << loc.file_name() << ":" << loc.line()
                  << " Assertion failed: " << expression << "\n";
        ++g_failedAssertions;
    } else {
        ++g_passedAssertions;
    }
}

#define TEST_ASSERT(cond) AssertTrue(static_cast<bool>(cond), #cond)

void TestExtent2iAndWindowState() {
    constexpr Extent2i def{};
    TEST_ASSERT(def.w == 0 && def.h == 0);

    constexpr Extent2i a{1920, 1080};
    constexpr Extent2i b{1920, 1080};
    constexpr Extent2i c{1280, 720};
    TEST_ASSERT(a == b);
    TEST_ASSERT(!(a == c));

    WindowState state{
        .logicalSize  = {1280, 720},
        .pixelSize    = {1920, 1080},
        .pixelDensity = 1.5f,
        .refreshHz    = 144.0f,
        .mode         = WindowMode::Windowed,
        .focused      = true,
        .suspended    = false
    };

    TEST_ASSERT(state.IsPresentable());
    TEST_ASSERT(std::abs(state.AspectRatio() - (16.0f / 9.0f)) < 1e-4f);

    // Suspended window is not presentable
    state.suspended = true;
    TEST_ASSERT(!state.IsPresentable());

    // Zero-sized window is not presentable
    state.suspended = false;
    state.pixelSize = {0, 720};
    TEST_ASSERT(!state.IsPresentable());

    // Degenerate height returns 1.0f fallback aspect ratio
    state.pixelSize = {1920, 0};
    TEST_ASSERT(state.AspectRatio() == 1.0f);
}

void TestFixedString() {
    FixedString<32> str("EngineN Platform");
    TEST_ASSERT(str.Size() == 16);
    TEST_ASSERT(!str.Empty());
    TEST_ASSERT(str.View() == "EngineN Platform");
    TEST_ASSERT(str == "EngineN Platform");
    TEST_ASSERT(str.CStr()[16] == '\0');

    // Capacity overflow protection: FixedString<8> holds at most 7 chars + NUL
    FixedString<8> limited;
    const bool assigned = limited.Assign("0123456789");
    TEST_ASSERT(!assigned); // Truncation detected
    TEST_ASSERT(limited.Size() == 7);
    TEST_ASSERT(limited.View() == "0123456");
    TEST_ASSERT(limited.CStr()[7] == '\0');
}

void TestWindowFlagsAndEventCapture() {
    WindowFlags flags = WindowFlags::Resized | WindowFlags::FocusGained;
    TEST_ASSERT(HasFlag(flags, WindowFlags::Resized));
    TEST_ASSERT(HasFlag(flags, WindowFlags::FocusGained));
    TEST_ASSERT(!HasFlag(flags, WindowFlags::DisplayChanged));
    TEST_ASSERT(!HasFlag(flags, WindowFlags::FocusLost));

    flags |= WindowFlags::DisplayChanged;
    TEST_ASSERT(HasFlag(flags, WindowFlags::DisplayChanged));

    EventCapture cap = EventCapture::Keyboard | EventCapture::Mouse;
    TEST_ASSERT(HasCapture(cap, EventCapture::Keyboard));
    TEST_ASSERT(HasCapture(cap, EventCapture::Mouse));
    TEST_ASSERT(!HasCapture(cap, EventCapture::Gamepad));
}

struct MockDevToolsOverlay {
    EventCapture captureMask{EventCapture::Keyboard};

    EventCapture OnEvent(const SDL_Event& ev) noexcept {
        (void)ev;
        return captureMask;
    }
};

void TestEventTapDelegate() {
    MockDevToolsOverlay mock{.captureMask = EventCapture::Keyboard | EventCapture::Gamepad};
    const EventTap tap = EventTap::Bind<&MockDevToolsOverlay::OnEvent>(mock);

    SDL_Event dummyEvent{};
    const EventCapture result = tap.callback(tap.user, dummyEvent);
    TEST_ASSERT(HasCapture(result, EventCapture::Keyboard));
    TEST_ASSERT(HasCapture(result, EventCapture::Gamepad));
    TEST_ASSERT(!HasCapture(result, EventCapture::Mouse));
}

void TestPlatformInitAndQuit() {
    Platform platform;
    TEST_ASSERT(!platform.ShouldQuit());

    const auto initRes = platform.Init("EngineN_Test", "EngineN");
    TEST_ASSERT(initRes.has_value());

    platform.RequestQuit();
    TEST_ASSERT(platform.ShouldQuit());

    platform.Shutdown();
}

void TestSandboxedPathResolution() {
    Platform platform;
    const auto initRes = platform.Init("EngineN_Test", "EngineN");
    TEST_ASSERT(initRes.has_value());

    // 1. Safe relative paths within asset sandbox
    const auto safeAsset = platform.ResolveAsset("textures/hero.png");
    TEST_ASSERT(safeAsset.has_value());
    TEST_ASSERT(safeAsset->View().ends_with("textures/hero.png"));

    const auto safeUser = platform.ResolveUser("savegame.dat");
    TEST_ASSERT(safeUser.has_value());
    TEST_ASSERT(safeUser->View().ends_with("savegame.dat"));

    // 2. Malicious directory traversal attempts must be strictly rejected
    const auto parentEscape = platform.ResolveAsset("../secret.txt");
    TEST_ASSERT(!parentEscape.has_value());

    const auto nestedEscape = platform.ResolveAsset("assets/../../secret.txt");
    TEST_ASSERT(!nestedEscape.has_value());

    const auto absoluteUnix = platform.ResolveAsset("/etc/passwd");
    TEST_ASSERT(!absoluteUnix.has_value());

    const auto absoluteWin = platform.ResolveAsset("\\Windows\\System32");
    TEST_ASSERT(!absoluteWin.has_value());

    const auto driveLetter = platform.ResolveAsset("C:\\secret.txt");
    TEST_ASSERT(!driveLetter.has_value());

    const auto emptyPath = platform.ResolveAsset("");
    TEST_ASSERT(!emptyPath.has_value());

    platform.Shutdown();
}

void TestClipboardService() {
    Platform platform;
    const auto initRes = platform.Init("EngineN_Test", "EngineN");
    TEST_ASSERT(initRes.has_value());

    constexpr std::string_view testStr = "EngineN_Unit_Test_Clipboard";
    const auto setRes = Platform::SetClipboard(testStr);
    TEST_ASSERT(setRes.has_value());

    char buf[128]{};
    const std::string_view clip = Platform::GetClipboard(buf);
    TEST_ASSERT(clip == testStr);

    platform.Shutdown();
}

} // namespace

int main() {
    std::cout << "Running EngineN Platform Subsystem Tests...\n";

    TestExtent2iAndWindowState();
    TestFixedString();
    TestWindowFlagsAndEventCapture();
    TestEventTapDelegate();
    TestPlatformInitAndQuit();
    TestSandboxedPathResolution();
    TestClipboardService();

    std::cout << "Test Summary: " << g_passedAssertions << " passed, "
              << g_failedAssertions << " failed.\n";

    return (g_failedAssertions == 0) ? 0 : 1;
}
