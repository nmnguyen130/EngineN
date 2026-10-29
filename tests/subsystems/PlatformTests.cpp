#include "platform/Platform.h"

#include <cmath>
#include <cstdio>

namespace {

using namespace en;

bool TestPathResolutionAndViewport() noexcept {
    FilePath base{};
    if (!NormalizeDir("C:\\Games\\Veil", base) || base != "C:/Games/Veil/") return false;

    FilePath resolved{};
    if (!ResolvePath(base, "data\\sprites/hero.png", resolved) ||
        resolved != "C:/Games/Veil/data/sprites/hero.png") {
        return false;
    }
    // Reject traversal, absolute paths, and drive colons.
    if (ResolvePath(base, "../secret.txt", resolved) ||
        ResolvePath(base, "/etc/passwd", resolved) ||
        ResolvePath(base, "C:foo", resolved)) {
        return false;
    }

    WindowState ws{};
    ws.pixelSize = {1920, 1080};
    const Rect vp = ws.ComputeViewport({640, 360}, ScaleMode::IntegerScale);
    if (vp != Rect{0.0f, 0.0f, 1920.0f, 1080.0f}) return false;

    const Vec2 virt = ws.PixelToVirtual({960.0f, 540.0f}, {640, 360});
    if (std::abs(virt.x - 320.0f) > 1e-3f || std::abs(virt.y - 180.0f) > 1e-3f) return false;
    return true;
}

bool TestEventBatchingAndReleaseProtection() noexcept {
    FrameEvents batch{};
    batch.ClearFrameDeltas();

    // Fill event queue with presses, then ensure a release is never dropped.
    for (u16 i = 1U; i <= FrameEvents::kMaxEvents; ++i) {
        batch.IngestKey(i, true, false, true, EventCapture::None);
    }
    batch.IngestKey(1U, false, false, true, EventCapture::None);
    bool foundRelease = false;
    for (const InputEvent& ev : batch.Events()) {
        if (ev.Code() == 1U && !ev.Down()) foundRelease = true;
    }
    return foundRelease;
}

bool TestPlatformAndAtomicFileIo() noexcept {
    Platform plat;
    if (!plat.Init("VeilTest", "EngineN", true)) return false;

    Window win;
    if (!win.Create({.title = "Test", .size = {1280, 720}}, true) || !win.IsValid()) return false;

    const auto pathRes = plat.ResolveUser("test_save.bin");
    if (!pathRes) return false;

    const u8 payload[]{10, 20, 30, 40, 50};
    if (!Platform::WriteFileAtomic(*pathRes, payload)) return false;

    std::array<u8, 16> readBuf{};
    const auto readRes = Platform::ReadFile(*pathRes, readBuf);
    if (!readRes || *readRes != sizeof(payload) || readBuf[4] != 50) return false;
    (void)std::remove(pathRes->CStr());

    win.Destroy();
    plat.Shutdown();
    return true;
}

} // namespace

int main() {
    if (!TestPathResolutionAndViewport()) return 1;
    if (!TestEventBatchingAndReleaseProtection()) return 2;
    if (!TestPlatformAndAtomicFileIo()) return 3;
    std::printf("PlatformTests: ALL PASSED\n");
    return 0;
}
