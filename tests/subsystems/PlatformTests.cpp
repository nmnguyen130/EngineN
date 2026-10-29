#include "tests/subsystems/CtsHarness.h"

#include "platform/Platform.h"

namespace {

using namespace en;
using namespace en::cts;

bool CtsSandboxedPathSecurity(CtsRunner& r) noexcept {
    FilePath base{};
    FilePath invalidBase{};
    CTS_CHECK(r, NormalizeDir("C:\\Games\\\\Veil//Data", base) && base == "C:/Games/Veil/Data/");
    CTS_CHECK(r, !NormalizeDir("", invalidBase) && !NormalizeDir({"bad\0dir", 7U}, invalidBase));

    FilePath out{};
    CTS_CHECK(r, ResolvePath(base, "sprites\\kaelen/./idle.png", out));
    CTS_CHECK(r, out == "C:/Games/Veil/Data/sprites/kaelen/idle.png");

    // Exhaustive adversarial path traversal & escape vectors.
    constexpr std::string_view kAttacks[]{
        "", ".", "..", "../secret.bin", "sprites/../../secret.bin",
        "/etc/passwd", "\\Windows\\System32", "C:drive_relative.bin", "D:\\abs.bin",
    };
    for (const std::string_view atk : kAttacks) {
        CTS_CHECK(r, !ResolvePath(base, atk, out));
    }
    CTS_CHECK(r, !ResolvePath(base, {"null\0attack.bin", 15U}, out));
    return true;
}

bool CtsViewportScalingAndCoordinates(CtsRunner& r) noexcept {
    WindowState ws{};
    ws.pixelSize = {1920, 1080};
    const WindowSize virt{640, 360}; // 3x exact integer multiple

    CTS_CHECK(r, ws.ComputeViewport(virt, ScaleMode::IntegerScale) == Rect{0.0f, 0.0f, 1920.0f, 1080.0f});

    // Ultrawide 2560x1080 with 640x360 virtual canvas:
    // Letterbox scales by 3.0x -> width 1920 centered at x = (2560 - 1920)/2 = 320.
    ws.pixelSize = {2560, 1080};
    const Rect lb = ws.ComputeViewport(virt, ScaleMode::Letterbox);
    CTS_CHECK(r, lb == Rect{320.0f, 0.0f, 1920.0f, 1080.0f});

    // PixelToVirtual clamping inside letterbox bars.
    const Vec2 leftBar = ws.PixelToVirtual({100.0f, 540.0f}, virt, ScaleMode::Letterbox, true);
    CTS_CHECK(r, Near(leftBar.x, 0.0f) && Near(leftBar.y, 180.0f));
    const Vec2 center = ws.PixelToVirtual({1280.0f, 540.0f}, virt, ScaleMode::Letterbox, true);
    CTS_CHECK(r, Near(center.x, 320.0f) && Near(center.y, 180.0f));

    // Lifecycle net-flag cancellation (Minimized -> Restored cancels Minimized; FocusLost -> FocusGained cancels FocusLost).
    WindowFlags net = WindowFlags::None;
    bool refresh = false;
    ws.ApplyLifecycleEvent(WindowFlags::Minimized, net, refresh);
    CTS_CHECK(r, ws.minimized && !ws.focused && HasFlag(net, WindowFlags::Minimized));
    ws.ApplyLifecycleEvent(WindowFlags::Restored, net, refresh);
    CTS_CHECK(r, !ws.minimized && !HasFlag(net, WindowFlags::Minimized) && HasFlag(net, WindowFlags::Restored));
    return true;
}

bool DstEventQueueReleaseProtectionAndUtf8(CtsRunner& r) noexcept {
    FrameEvents batch{};
    batch.ClearFrameDeltas();

    // 1. Fill all 49 slots with distinct key presses [1..49].
    for (u16 k = 1U; k <= FrameEvents::kMaxEvents; ++k) {
        batch.IngestKey(k, true, false, true, EventCapture::None);
    }
    CTS_CHECK(r, batch.eventCount == FrameEvents::kMaxEvents);

    // Release keys 1, 25, and 49 while queue is full: each must update its own slot in-place
    // without clobbering any other pressed key in the queue!
    batch.IngestKey(1U, false, false, true, EventCapture::None);
    batch.IngestKey(25U, false, false, true, EventCapture::None);
    batch.IngestKey(49U, false, false, true, EventCapture::None);

    for (u16 i = 0U; i < FrameEvents::kMaxEvents; ++i) {
        const u16 expectedKey = static_cast<u16>(i + 1U);
        const bool expectedDown = (expectedKey != 1U && expectedKey != 25U && expectedKey != 49U);
        CTS_CHECK(r, batch.Events()[i].Code() == expectedKey);
        CTS_CHECK(r, batch.Events()[i].Down() == expectedDown);
    }

    // 2. Gamepad axis -32768 clamping and in-place coalescing.
    batch.ClearFrameDeltas();
    batch.IngestGamepadAxis(2U, -32768, true, EventCapture::None);
    batch.IngestGamepadAxis(2U, 14500, true, EventCapture::None);
    CTS_CHECK(r, batch.eventCount == 1U && batch.Events()[0].value == 14500);

    // 3. Multi-byte UTF-8 boundary truncation check (3-byte Vietnamese 'ệ' = E1 BB 87).
    batch.ClearFrameDeltas();
    batch.IngestTextInput("123456789012345678901234567890", true);
    CTS_CHECK(r, batch.textLen == 30U);
    batch.IngestTextInput("\xE1\xBB\x87", true);
    CTS_CHECK(r, batch.textLen == 30U && batch.TextView() == "123456789012345678901234567890");

    r.RecordFuzzOps(200U);
    return true;
}

bool CtsAtomicFileIoOverwriteAndReadBounds(CtsRunner& r) noexcept {
    Platform plat;
    CTS_CHECK(r, plat.Init("VeilCTS", "EngineN", true).has_value());

    Window win;
    CTS_CHECK(r, win.Create({.title = "CTS", .size = {1280, 720}}, true).has_value());

    const auto pathRes = plat.ResolveUser("cts_atomic_save.bin");
    CTS_CHECK(r, pathRes.has_value());

    // Stress 20 consecutive atomic overwrites on the exact same path with varying sizes.
    Rng rng(0x5A7EC75ULL);
    for (u32 iter = 0U; iter < 20U; ++iter) {
        std::array<u8, 256> writeData{};
        const usize len = static_cast<usize>(rng.Range(16, 256));
        for (usize i = 0U; i < len; ++i) writeData[i] = static_cast<u8>(rng.NextU32() & 0xFFU);

        CTS_CHECK(r, Platform::WriteFileAtomic(*pathRes, {writeData.data(), len}).has_value());

        // Reading into a buffer smaller than the file size must fail cleanly.
        std::array<u8, 8> tinyBuf{};
        CTS_CHECK(r, !Platform::ReadFile(*pathRes, tinyBuf).has_value());

        std::array<u8, 256> readBuf{};
        const auto readRes = Platform::ReadFile(*pathRes, readBuf);
        CTS_CHECK(r, readRes.has_value() && *readRes == len);
        CTS_CHECK(r, std::memcmp(writeData.data(), readBuf.data(), len) == 0);
    }
    (void)std::remove(pathRes->CStr());
    r.RecordFuzzOps(20U);

    win.Destroy();
    plat.Shutdown();
    return true;
}

} // namespace

int main() {
    CtsRunner runner{.suiteName = "PlatformTests (CTS+DST)"};
    runner.RunCase("Sandboxed Path Security & Traversal Guards", CtsSandboxedPathSecurity);
    runner.RunCase("Viewport Scaling & Lifecycle Cancellation", CtsViewportScalingAndCoordinates);
    runner.RunCase("DST: Full-Queue Release & UTF-8 Truncation", DstEventQueueReleaseProtectionAndUtf8);
    runner.RunCase("Atomic Save Overwrite & Read Bounds Stress", CtsAtomicFileIoOverwriteAndReadBounds);
    return runner.Finish();
}
