#include "platform/Platform.h"
#include <cmath>
#include <cstdio>
#include <filesystem>

#if __has_include(<SDL3/SDL.h>)
#include <SDL3/SDL.h>
#define EN_TEST_HAS_SDL3 1
#else
#define EN_TEST_HAS_SDL3 0
#endif

namespace en {

namespace {

[[nodiscard]] consteval bool VerifyConstexprLayer1Platform() noexcept {
    // 1. WindowState coordinate space transformations & ApplyMetrics
    WindowState ws{};
    if (!ws.IsPresentable()) return false;
    if (std::abs(ws.AspectRatio() - (1280.0f / 720.0f)) > 1e-4f) return false;

    const WindowFlags f0 = ws.ApplyMetrics({1280, 720}, {1280, 720}, 1.0f, 1.0f, 60.0f);
    if (f0 != WindowFlags::None) return false;

    const WindowFlags f1 = ws.ApplyMetrics({1280, 720}, {2560, 1440}, 2.0f, 2.0f, 120.0f);
    if (!HasFlag(f1, WindowFlags::Resized) || !HasFlag(f1, WindowFlags::DisplayChanged)) return false;

    const Vec2 pix = ws.WindowToPixel({100.0f, 50.0f});
    if (pix.x != 200.0f || pix.y != 100.0f) return false;
    const Vec2 win = ws.PixelToWindow(pix);
    if (win.x != 100.0f || win.y != 50.0f) return false;
    const Vec2 ui = ws.PixelToUiLogical({400.0f, 200.0f});
    if (ui.x != 200.0f || ui.y != 100.0f) return false;
    const Vec2 uiPix = ws.UiLogicalToPixel(ui);
    if (uiPix.x != 400.0f || uiPix.y != 200.0f) return false;

    // 2. 2D Virtual Canvas & Aspect-Ratio Viewport Scaler (Stretch, Letterbox, IntegerScale, Expand)
    WindowState ultrawide{};
    ultrawide.pixelSize = {2560, 1080}; // 21:9 monitor displaying 1920x1080 16:9 canvas
    const Rect pillarVp = ultrawide.ComputeViewport({1920, 1080}, ScaleMode::Letterbox);
    if (pillarVp.x != 320.0f || pillarVp.y != 0.0f || pillarVp.w != 1920.0f || pillarVp.h != 1080.0f) {
        return false;
    }
    // Click inside pillarbox bar (x = 100) clamps to virtual canvas x = 0
    const Vec2 clampedLeft = ultrawide.PixelToVirtual({100.0f, 540.0f}, {1920, 1080}, ScaleMode::Letterbox, true);
    if (clampedLeft.x != 0.0f || std::abs(clampedLeft.y - 540.0f) > 1e-3f) return false;
    // Center of 2560x1080 screen maps to (960, 540) in 1920x1080 virtual canvas
    const Vec2 centerVirt = ultrawide.PixelToVirtual({1280.0f, 540.0f}, {1920, 1080}, ScaleMode::Letterbox, true);
    if (std::abs(centerVirt.x - 960.0f) > 1e-3f || std::abs(centerVirt.y - 540.0f) > 1e-3f) return false;
    const Vec2 backToPix = ultrawide.VirtualToPixel(centerVirt, {1920, 1080}, ScaleMode::Letterbox);
    if (std::abs(backToPix.x - 1280.0f) > 1e-3f || std::abs(backToPix.y - 540.0f) > 1e-3f) return false;

    // Integer scaling: 320x180 pixel-art canvas on 1366x768 display -> 4x integer scale (1280x720)
    WindowState laptop{};
    laptop.pixelSize = {1366, 768};
    const Rect intVp = laptop.ComputeViewport({320, 180}, ScaleMode::IntegerScale);
    if (intVp.w != 1280.0f || intVp.h != 720.0f || intVp.x != 43.0f || intVp.y != 24.0f) return false;

    // 3. WindowState mutually-exclusive net lifecycle transitions
    bool needRefresh = false;
    WindowFlags netFlags = WindowFlags::None;
    ws.ApplyLifecycleEvent(WindowFlags::Minimized | WindowFlags::FocusLost, netFlags, needRefresh);
    if (!ws.suspended || ws.focused || ws.IsPresentable()) return false;
    ws.ApplyLifecycleEvent(WindowFlags::Restored, netFlags, needRefresh);
    ws.ApplyLifecycleEvent(WindowFlags::FocusGained, netFlags, needRefresh);
    if (ws.suspended || !ws.focused || !needRefresh) return false;
    if (HasFlag(netFlags, WindowFlags::Minimized) || HasFlag(netFlags, WindowFlags::FocusLost)) return false;
    if (!HasFlag(netFlags, WindowFlags::Restored) || !HasFlag(netFlags, WindowFlags::FocusGained)) return false;

    // 4. PowerInfo battery threshold logic
    constexpr PowerInfo lowBat{1200, 15, PowerSupplyState::OnBattery};
    constexpr PowerInfo chargingBat{1200, 15, PowerSupplyState::Charging};
    if (!lowBat.IsLowBattery(20) || chargingBat.IsLowBattery(20)) return false;

    // 5. EventTap constexpr bitmask conversion across all 8 3-bit combinations
    if (EventTap{}.Evaluate() != EventCapture::None) return false;
    if (EventTap{EventCapture::Keyboard}.Evaluate() != EventCapture::Keyboard) return false;
    if (EventTap{EventCapture::Mouse}.Evaluate() != EventCapture::Mouse) return false;
    if (EventTap{EventCapture::Gamepad}.Evaluate() != EventCapture::Gamepad) return false;
    if (EventTap{EventCapture::Keyboard | EventCapture::Mouse}.Evaluate() !=
        (EventCapture::Keyboard | EventCapture::Mouse)) {
        return false;
    }
    if (EventTap{EventCapture::Keyboard | EventCapture::Gamepad}.Evaluate() !=
        (EventCapture::Keyboard | EventCapture::Gamepad)) {
        return false;
    }
    if (EventTap{EventCapture::Mouse | EventCapture::Gamepad}.Evaluate() !=
        (EventCapture::Mouse | EventCapture::Gamepad)) {
        return false;
    }
    if (EventTap{EventCapture::All}.Evaluate() != EventCapture::All) return false;

    // 6. PlatformFrameEvents: key repeat filter, duplicate edge suppression & anti-sticking capture filter
    PlatformFrameEvents batch{};
    batch.IngestKey(44U, true, false, true, EventCapture::None);
    batch.IngestKey(44U, true, true, true, EventCapture::None);  // OS repeat ignored
    batch.IngestKey(44U, true, false, true, EventCapture::None); // Duplicate Down without Up ignored
    if (batch.eventCount != 1U) return false;

    // Press while Keyboard is captured must be blocked, but release must pass through!
    batch.IngestKey(22U, true, false, true, EventCapture::Keyboard);
    if (batch.eventCount != 1U) return false;
    batch.IngestKey(44U, false, false, false, EventCapture::Keyboard); // Release while unfocused & captured
    if (batch.eventCount != 2U) return false;
    if (batch.events[1].Code() != 44U || batch.events[1].Down()) return false;

    // 7. Immediate High-DPI 8000 Hz mouse scaling + flipped wheel + UTF-8 text input
    for (int i = 0; i < 8; ++i) {
        batch.IngestMouseMotion(
            10.0f + static_cast<f32>(i), 20.0f, 1.5f, -0.5f, 2.0f, true, EventCapture::None);
    }
    if (batch.mouseX != 34.0f || batch.mouseY != 40.0f || batch.mouseDx != 24.0f || batch.mouseDy != -8.0f) {
        return false;
    }

    batch.IngestMouseWheel(0.0f, 1.0f, false, true, EventCapture::None);
    batch.IngestMouseWheel(0.0f, 2.0f, true, true, EventCapture::None); // Flipped scroll
    if (!batch.HasMouseWheel() || batch.wheelY != -1.0f) return false;

    batch.IngestTextInput("fps_max ", true);
    batch.IngestTextInput("120", true);
    batch.IngestTextInput("ignored", false); // Unfocused text input ignored
    if (batch.TextView() != "fps_max 120") return false;

    // 8. Interleaved 1000 Hz dual-axis (X/Y) gamepad stick coalescing & disconnect purge
    batch.IngestGamepadAxis(1U, 0U, -32768, true, EventCapture::None); // Axis 0 (X) clamped to -32767
    batch.IngestGamepadAxis(1U, 1U, 10000, true, EventCapture::None);  // Axis 1 (Y)
    batch.IngestGamepadAxis(1U, 0U, 16000, true, EventCapture::None);  // Axis 0 (X) interleaved!
    batch.IngestGamepadAxis(1U, 1U, -20000, true, EventCapture::None); // Axis 1 (Y) interleaved!
    if (batch.eventCount != 4U) return false; // 2 keys + 1 Axis0 + 1 Axis1!
    if (batch.events[2].value != 16000 || batch.events[3].value != -20000) return false;

    batch.IngestGamepadButton(1U, 3U, true, true, EventCapture::None);
    batch.IngestGamepadButton(2U, 5U, true, true, EventCapture::None);
    if (batch.eventCount != 6U) return false;

    // Disconnect pad 1 mid-frame: must set disconnectedPadMask bit 1 and strip pad 1's 3 events!
    batch.IngestGamepadDisconnect(1U);
    if (batch.disconnectedPadMask != (1U << 1U)) return false;
    if (batch.eventCount != 3U) return false; // 2 key events + 1 pad-2 button event remain
    if (batch.events[2].Kind() != InputEventKind::GamepadButton || batch.events[2].PadSlot() != 2U) {
        return false;
    }

    // 9. 49-event queue capacity saturation boundary (with distinct alternating Down/Up edges)
    for (u16 i = 0U; i < 100U; ++i) {
        batch.IngestMouseButton(
            static_cast<u16>(((i >> 1U) & 7U) + 1U),
            (i & 1U) == 0U,
            50.0f,
            60.0f,
            1.0f,
            true,
            EventCapture::None);
    }
    if (batch.eventCount != PlatformFrameEvents::kMaxEvents) return false;
    if (batch.Events().size() != PlatformFrameEvents::kMaxEvents) return false;

    if (ws.ComputeAuditHash() == 0ULL || batch.ComputeAuditHash() == 0ULL) return false;
    return true;
}

static_assert(VerifyConstexprLayer1Platform());

struct MockInputSink {
    u32 ingestCalls{0};
    u8 lastEventCount{0};
    u8 lastFocusCleared{0};
    u8 lastDisconnectedMask{0};
    f32 lastMouseX{0.0f};
    f32 lastMouseDx{0.0f};

    constexpr void IngestPlatformFrame(const PlatformFrameEvents& batch) noexcept {
        ++ingestCalls;
        lastEventCount = batch.eventCount;
        lastFocusCleared = batch.focusCleared;
        lastDisconnectedMask = batch.disconnectedPadMask;
        lastMouseX = batch.mouseX;
        lastMouseDx = batch.mouseDx;
    }
};

struct MockOverlayCapture {
    EventCapture activeMask{EventCapture::None};
    [[nodiscard]] EventCapture QueryCapture() const noexcept { return activeMask; }
};

} // namespace

} // namespace en

int main() {
    using namespace en;

    u32 passed = 0U;
    u32 failed = 0U;
    auto check = [&](bool cond, const char* expr, int line) noexcept {
        if (cond) {
            ++passed;
        } else {
            ++failed;
            std::fprintf(stderr, "[FAIL] Line %d: %s\n", line, expr);
        }
    };
#define EN_CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

    // 1. Struct byte sizes & cache-line alignment invariants
    EN_CHECK(sizeof(Extent2i) == 8U && alignof(Extent2i) == 8U);
    EN_CHECK(sizeof(PowerInfo) == 4U && alignof(PowerInfo) == 4U);
    EN_CHECK(sizeof(WindowConfig) == 40U && alignof(WindowConfig) == 8U);
    EN_CHECK(sizeof(WindowState) == 32U && alignof(WindowState) == 32U);
    EN_CHECK(sizeof(EventTap) == 16U && alignof(EventTap) == 16U);
    EN_CHECK(sizeof(PlatformInputEvent) == 4U && alignof(PlatformInputEvent) == 4U);
    EN_CHECK(sizeof(PlatformFrameEvents) == 256U && alignof(PlatformFrameEvents) == 64U);
    EN_CHECK(sizeof(Window) == 64U && alignof(Window) == 64U);
    EN_CHECK(sizeof(FileWatcher::WatchSlot) == 272U && alignof(FileWatcher::WatchSlot) == 8U);
    EN_CHECK(sizeof(Platform) == 1856U && alignof(Platform) == 64U);

    // 2. Platform lifecycle, screen sleep control, power query & sandboxed path resolution
    Platform plat{};
    EN_CHECK(plat.Init("EngineNTest", "EngineN", true).has_value());
    EN_CHECK(plat.IsInitialized() && plat.IsHeadless() && !plat.ShouldQuit());
    EN_CHECK(!plat.BasePath().empty() && !plat.PrefPath().empty());
    EN_CHECK(plat.IsScreenSleepEnabled());
    plat.SetScreenSleepEnabled(false);
    EN_CHECK(!plat.IsScreenSleepEnabled());
    plat.SetScreenSleepEnabled(true);
    EN_CHECK(plat.IsScreenSleepEnabled());

    const PowerInfo pwr = Platform::QueryPowerInfo();
    EN_CHECK(!pwr.IsLowBattery());

    EN_CHECK(!plat.ResolveAsset("").has_value());
    EN_CHECK(!plat.ResolveAsset("../escape.bin").has_value());
    EN_CHECK(!plat.ResolveAsset("sub/../../escape.bin").has_value());
    EN_CHECK(!plat.ResolveAsset("/abs/path.bin").has_value());
    EN_CHECK(!plat.ResolveAsset("C:/drive.bin").has_value());
    EN_CHECK(!plat.ResolveAsset("trailing/slash/").has_value());

    const auto resolvedAsset = plat.ResolveAsset("textures\\hero/idle.png");
    EN_CHECK(resolvedAsset.has_value());
    EN_CHECK(resolvedAsset->View().ends_with("textures/hero/idle.png"));

    const auto resolvedUser = plat.ResolveUser("saves\\slot_01.sav");
    EN_CHECK(resolvedUser.has_value());
    EN_CHECK(resolvedUser->View().ends_with("saves/slot_01.sav"));

    // 3. Atomic file write/read & bitmask-accelerated FileWatcher hot-reload polling
    FilePath tempFile{};
    EN_CHECK(tempFile.Assign("/tmp/enginen_platform_test.bin"));
    std::filesystem::remove(tempFile.View());

    const std::array<u8, 8> payload1{1, 2, 3, 4, 5, 6, 7, 8};
    EN_CHECK(Platform::WriteFileAtomic(tempFile, payload1).has_value());

    std::array<u8, 4> smallBuf{};
    EN_CHECK(!Platform::ReadFileBuffer(tempFile, smallBuf).has_value());

    std::array<u8, 16> readBuf{};
    const auto readRes = Platform::ReadFileBuffer(tempFile, readBuf);
    EN_CHECK(readRes.has_value() && *readRes == 8U && readBuf[0] == 1U && readBuf[7] == 8U);

    FileWatcher watcher{};
    EN_CHECK(watcher.ActiveCount() == 0U);
    const StringId assetSid = "textures/hero/idle.png"_sid;
    EN_CHECK(!watcher.Watch(FilePath{}, assetSid));
    EN_CHECK(!watcher.Watch(tempFile, StringId{}));
    EN_CHECK(watcher.Watch(tempFile, assetSid));
    EN_CHECK(watcher.ActiveCount() == 1U);

    std::array<StringId, 4> changed{};
    EN_CHECK(watcher.PollModified(changed) == 0U);

    const std::array<u8, 8> payload2{9, 10, 11, 12, 13, 14, 15, 16};
    EN_CHECK(Platform::WriteFileAtomic(tempFile, payload2).has_value());
    std::error_code ec{};
    const auto oldTime = std::filesystem::last_write_time(tempFile.CStr(), ec);
    std::filesystem::last_write_time(tempFile.CStr(), oldTime + std::chrono::seconds(2), ec);

    EN_CHECK(watcher.PollModified(changed) == 1U && changed[0] == assetSid);
    EN_CHECK(watcher.PollModified(changed) == 0U);
    EN_CHECK(watcher.Unwatch(assetSid));
    EN_CHECK(watcher.ActiveCount() == 0U);
    EN_CHECK(!watcher.Unwatch(assetSid));
    std::filesystem::remove(tempFile.View(), ec);

    // 4. Window lifecycle, cursor shapes, aspect-ratio lock, safe area, and DPI migration
    Window win{};
    WindowConfig badCfg{};
    badCfg.size = {0, -10};
    EN_CHECK(!win.Create(badCfg, true).has_value());

    WindowConfig wcfg{};
    wcfg.title = "EngineN Platform Spec";
    wcfg.size = {1280, 720};
    wcfg.minSize = {640, 360};
    EN_CHECK(win.Create(wcfg, true).has_value());
    EN_CHECK(win.IsValid() && win.Id() == 1U);
    EN_CHECK(win.State().logicalSize == Extent2i{1280, 720});
    EN_CHECK(win.SafeAreaPixels() == Rect{0.0f, 0.0f, 1280.0f, 720.0f});
    EN_CHECK(win.ComputeAuditHash() != 0ULL);

    EN_CHECK(win.SetTitle("EngineN Updated Title").has_value());
    EN_CHECK(!win.SetAspectRatioLock(2.0f, 1.0f).has_value()); // Invalid min > max rejected
    EN_CHECK(win.SetAspectRatioLock(16.0f / 9.0f, 16.0f / 9.0f).has_value());
    EN_CHECK(win.IsAspectRatioLocked());
    EN_CHECK(win.SetAspectRatioLock(0.0f, 0.0f).has_value());
    EN_CHECK(!win.IsAspectRatioLocked());

    EN_CHECK(win.SetMode(WindowMode::Borderless).has_value());
    EN_CHECK(win.State().mode == WindowMode::Borderless);
    EN_CHECK(win.SetMode(WindowMode::ExclusiveFullscreen, 144.0f).has_value());
    EN_CHECK(win.State().mode == WindowMode::ExclusiveFullscreen && win.State().refreshHz == 144.0f);
    EN_CHECK(win.SetMode(WindowMode::Windowed).has_value());
    EN_CHECK(win.SetRelativeMouse(true).has_value() && win.State().relativeMouse);

    win.SetCursorVisible(false);
    win.SetCursorShape(CursorShape::Crosshair);
    win.SetTextInput(true);
    win.WarpMouse({640.0f, 360.0f});
    win.Flash(WindowFlashOp::Briefly);
    EN_CHECK(!win.IsCursorVisible() && win.GetCursorShape() == CursorShape::Crosshair && win.IsTextInputActive());

    f32 observedDpi = 1.0f;
    Extent2i observedPixel{};
    win.SetDpiChangeCallback(
        [](f32 dpi, Extent2i, Extent2i px, void* ctx) noexcept {
            *static_cast<f32*>(ctx) = dpi;
            (void)px;
        },
        &observedDpi);
    win.SimulateDpiMigration(1.5f, {1280, 720}, 1.5f);
    observedPixel = win.State().pixelSize;
    EN_CHECK(win.DpiMigrationCount() == 1U && std::abs(observedDpi - 1.5f) < 1e-3f);
    EN_CHECK(observedPixel == Extent2i{1920, 1080});

    Window movedWin = std::move(win);
    EN_CHECK(!win.IsValid() && movedWin.IsValid() && movedWin.State().pixelSize == Extent2i{1920, 1080});
    plat.SetLiveResizeWatch(&movedWin);
    plat.SetLiveResizeWatch(nullptr);

    // 5. 4-Slot Bitmask Gamepad SoA hotplug, rumble, and disconnect cleanup
    EN_CHECK(!plat.OpenGamepad(0U));
    EN_CHECK(plat.OpenGamepad(101U));
    EN_CHECK(plat.OpenGamepad(101U)); // Duplicate open is idempotent
    EN_CHECK(plat.OpenGamepad(102U));
    EN_CHECK(plat.OpenGamepad(103U));
    EN_CHECK(plat.OpenGamepad(104U));
    EN_CHECK(!plat.OpenGamepad(105U)); // 5th pad rejected when all 4 slots are occupied
    EN_CHECK(plat.ActiveGamepadMask() == 0x0FU);
    EN_CHECK(plat.FindGamepadSlot(103U) == 2U);

    EN_CHECK(!plat.RumbleGamepad(4U, 30000U, 30000U, 100U)); // Out-of-range slot rejected
    EN_CHECK(plat.RumbleGamepad(2U, 30000U, 60000U, 250U));
    EN_CHECK(plat.RumbleGamepadMask() == (1U << 2U));
    EN_CHECK(plat.RumbleGamepad(2U, 0U, 0U, 0U));
    EN_CHECK(plat.RumbleGamepadMask() == 0U);

    // Queue an event on pad slot 1 (instanceId 102) then disconnect it before FinalizePump
    plat.FrameEvents().IngestGamepadButton(1U, 0U, true, true, EventCapture::None);
    EN_CHECK(plat.FrameEvents().eventCount == 1U);
    plat.CloseGamepad(102U);
    EN_CHECK(plat.ActiveGamepadMask() == 0x0DU); // Slot 1 freed
    EN_CHECK(plat.FrameEvents().disconnectedPadMask == (1U << 1U));
    EN_CHECK(plat.FrameEvents().eventCount == 0U); // Disconnected pad event purged!

    // New gamepad reclaims lowest free slot (slot 1) via std::countr_one
    EN_CHECK(plat.OpenGamepad(202U) && plat.FindGamepadSlot(202U) == 1U);

    // 6. EventTap binding, immediate DPI mouse scaling, and FocusLost immediate purge
    MockOverlayCapture overlay{EventCapture::Keyboard};
    const EventTap boundTap = EventTap::Bind<&MockOverlayCapture::QueryCapture>(&overlay);
    EN_CHECK(boundTap.Evaluate() == EventCapture::Keyboard);

    plat.FrameEvents().ClearFrameDeltas();
    plat.FrameEvents().IngestMouseMotion(
        100.0f, 50.0f, 10.0f, -5.0f, movedWin.State().pixelDensity, true, boundTap.Evaluate());
    const WindowFlags pumpFlags1 = plat.FinalizePump(movedWin, WindowFlags::None, false);
    EN_CHECK(pumpFlags1 == WindowFlags::None);
    EN_CHECK(std::abs(plat.FrameEvents().mouseX - 150.0f) < 1e-3f); // Scaled by 1.5x pixelDensity
    EN_CHECK(std::abs(plat.FrameEvents().mouseDx - 15.0f) < 1e-3f);

    // FocusLost must stop all rumble and purge queued frame inputs with focusCleared = 1
    (void)plat.RumbleGamepad(0U, 10000U, 10000U, 500U);
    plat.FrameEvents().IngestKey(44U, true, false, true, EventCapture::None);
    const WindowFlags pumpFlags2 = plat.FinalizePump(movedWin, WindowFlags::FocusLost, false);
    EN_CHECK(HasFlag(pumpFlags2, WindowFlags::FocusLost));
    EN_CHECK(plat.RumbleGamepadMask() == 0U);
    EN_CHECK(plat.FrameEvents().focusCleared == 1U && plat.FrameEvents().eventCount == 0U);

    MockInputSink sink{};
    (void)plat.Pump(movedWin, sink, boundTap);
    EN_CHECK(sink.ingestCalls == 1U);

    // 7. Drag-and-drop file queue & Clipboard round-trip
    EN_CHECK(!plat.PushDroppedFile(""));
    EN_CHECK(plat.PushDroppedFile("mods/dlc_01.enpak"));
    EN_CHECK(plat.PushDroppedFile("mods/dlc_02.enpak"));
    EN_CHECK(plat.PushDroppedFile("mods/dlc_03.enpak"));
    EN_CHECK(plat.PushDroppedFile("mods/dlc_04.enpak"));
    EN_CHECK(!plat.PushDroppedFile("mods/dlc_05.enpak")); // 5th file rejected at capacity 4
    EN_CHECK(plat.DroppedFileCount() == 4U);
    std::array<FilePath, 4> droppedOut{};
    EN_CHECK(plat.PollDroppedFiles(droppedOut) == 4U);
    EN_CHECK(droppedOut[0] == "mods/dlc_01.enpak" && droppedOut[3] == "mods/dlc_04.enpak");
    EN_CHECK(plat.DroppedFileCount() == 0U);

    EN_CHECK(Platform::SetClipboardText("r_vsync 1; cl_fov 90"));
    std::array<char, 64> clipBuf{};
    const usize clipLen = Platform::GetClipboardText(clipBuf);
    EN_CHECK(clipLen == 20U && std::string_view{clipBuf.data(), clipLen} == "r_vsync 1; cl_fov 90");

    std::array<char, 8> tinyClip{};
    const usize tinyLen = Platform::GetClipboardText(tinyClip);
    EN_CHECK(tinyLen == 7U && tinyClip[7] == '\0' && std::string_view{tinyClip.data(), tinyLen} == "r_vsync");

#if EN_TEST_HAS_SDL3
    // 8. End-to-end SDL_PollEvent pump verification (including interleaved X/Y axis coalescing)
    Platform sdlPlat{};
    EN_CHECK(sdlPlat.Init("EngineNSdl", "EngineN", false).has_value());
    Window sdlWin{};
    EN_CHECK(sdlWin.Create(wcfg, false).has_value());

    SDL_Event evKeyDown{};
    evKeyDown.type = SDL_EVENT_KEY_DOWN;
    evKeyDown.key.windowID = sdlWin.Id();
    evKeyDown.key.scancode = SDL_SCANCODE_SPACE;
    evKeyDown.key.down = true;
    evKeyDown.key.repeat = false;
    SDL_PushEvent(&evKeyDown);

    SDL_Event evKeyRepeat = evKeyDown;
    evKeyRepeat.key.repeat = true;
    SDL_PushEvent(&evKeyRepeat);

    SDL_Event evPadAdd{};
    evPadAdd.type = SDL_EVENT_GAMEPAD_ADDED;
    evPadAdd.gdevice.which = 77U;
    SDL_PushEvent(&evPadAdd);

    SDL_Event evAxisX1{};
    evAxisX1.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    evAxisX1.gaxis.which = 77U;
    evAxisX1.gaxis.axis = 0U;
    evAxisX1.gaxis.value = -32768;
    SDL_PushEvent(&evAxisX1);

    SDL_Event evAxisY1 = evAxisX1;
    evAxisY1.gaxis.axis = 1U;
    evAxisY1.gaxis.value = 12000;
    SDL_PushEvent(&evAxisY1);

    SDL_Event evAxisX2 = evAxisX1;
    evAxisX2.gaxis.value = 24000;
    SDL_PushEvent(&evAxisX2);

    SDL_Event evAxisY2 = evAxisY1;
    evAxisY2.gaxis.value = -18000;
    SDL_PushEvent(&evAxisY2);

    MockInputSink sdlSink{};
    (void)sdlPlat.Pump(sdlWin, sdlSink);
    EN_CHECK(sdlSink.ingestCalls == 1U);
    EN_CHECK(sdlSink.lastEventCount == 3U); // 1 key + 1 coalesced AxisX + 1 coalesced AxisY!
    EN_CHECK(sdlPlat.ActiveGamepadMask() == 0x01U);

    Platform::ShowErrorBox("Test Error", "Headless error box check", &sdlWin);
    sdlWin.Destroy();
    sdlPlat.Shutdown();
#endif

    movedWin.Destroy();
    plat.Shutdown();

    std::printf("PlatformTests: %u passed, %u failed.\n", passed, failed);
    return failed == 0U ? 0 : 1;
}
