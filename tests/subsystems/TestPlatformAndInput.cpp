#include "tests/TestRunner.h"
#include "input/InputPipeline.h"
#include "platform/Platform.h"

namespace en::tests {

void RunSuitePlatformAndInput(SuiteStats& stats) noexcept {
    // 1. Platform headless lifecycle, sandbox path safety, and atomic Save/Load
    Platform plat{};
    EN_CHECK(stats, plat.Init("EngineNTest", "EngineN", true).has_value());
    EN_CHECK(stats, !plat.ResolveAsset("../outside_escape.bin").has_value());
    auto validPath = plat.ResolveUser("test_slot.sav");
    EN_CHECK(stats, validPath.has_value());

    const std::array<u8, 8> savePayload{10, 20, 30, 40, 50, 60, 70, 80};
    EN_CHECK(stats, Platform::WriteFileAtomic(*validPath, savePayload).has_value());
    std::array<u8, 16> readBuf{};
    auto readBytes = Platform::ReadFileBuffer(*validPath, readBuf);
    EN_CHECK(stats, readBytes.has_value() && *readBytes == 8U && readBuf[7] == 80U);

    // 2. Window headless state & audit hash
    Window win{};
    WindowConfig wcfg{};
    wcfg.size = {1280, 720};
    EN_CHECK(stats, win.Create(wcfg).has_value());
    EN_CHECK(stats, win.IsValid() && win.State().logicalSize.w == 1280);
    EN_CHECK(stats, win.ComputeAuditHash() != 0ULL);
    win.Destroy();
    plat.Shutdown();

    // 3. InputPipeline, ActionBuffer (hold/buffer/coyote), and InputReplayStream RLE round-trip
    InputPipeline input{};
    input.Reset();
    ActionBuffer actBuf{};
    actBuf.Clear();

    input.InjectSyntheticAction(2U, true);
    input.InjectSyntheticAxis(0U, 0.75f);
    input.InjectSyntheticAxis(1U, -0.25f);
    const TickInput t1 = input.PollTick(1);
    EN_CHECK(stats, t1.Pressed(2U) && t1.Held(2U));
    EN_CHECK(stats, std::abs(t1.Axis(0U) - 0.75f) < 1e-3f);

    actBuf.Feed(t1, false);
    EN_CHECK(stats, actBuf.Peek(2U));
    EN_CHECK(stats, actBuf.Consume(2U));
    EN_CHECK(stats, !actBuf.Peek(2U));

    InputReplayStream replay{};
    replay.RecordTick(t1);
    replay.RewindPlayback();
    TickInput played{};
    u16 prevHeld = 0;
    EN_CHECK(stats, replay.SamplePlaybackTick(1, played, prevHeld) && played.Held(2U));

    // 4. DpiScaleHandler monitor migration callback, FileDropQueue, and Clipboard
    f32 observedDpi = 1.0f;
    win.SetDpiChangeCallback([](f32 newDpi, Extent2i, Extent2i, void* ctx) noexcept {
        *static_cast<f32*>(ctx) = newDpi;
    }, &observedDpi);
    win.SimulateDpiMigration(1.5f, {1280, 720});
    EN_CHECK(stats, win.DpiMigrationCount() == 1U && std::abs(observedDpi - 1.5f) < 1e-3f);
    EN_CHECK(stats, win.State().pixelSize.w == 1920 && win.State().pixelSize.h == 1080);

    EN_CHECK(stats, plat.PushDroppedFile("mods/custom_pack.enpak"));
    std::array<FilePath, 4> dropped{};
    EN_CHECK(stats, plat.PollDroppedFiles(dropped) == 1U && dropped[0].View() == "mods/custom_pack.enpak");

    EN_CHECK(stats, Platform::SetClipboardText("set player.speed 12.0"));
    std::array<char, 64> clipOut{};
    EN_CHECK(stats, Platform::GetClipboardText(clipOut) > 0U && std::string_view(clipOut.data()) == "set player.speed 12.0");

    // 5. InputRemapTable binary serialization, GestureDetector (Double-Tap & Hold-to-Charge), and Radial/Axial Deadzones
    input.Map().Bind(3U, {InputSource::Key(44)});
    std::array<u8, 512> remapBytes{};
    const usize serBytes = input.Map().SerializeBindings(remapBytes);
    EN_CHECK(stats, serBytes > 0U);
    InputMap restoredMap{};
    EN_CHECK(stats, restoredMap.DeserializeBindings(std::span<const u8>(remapBytes.data(), serBytes)));
    EN_CHECK(stats, restoredMap.ActionBindings(3U)[0].code == 44U);

    GestureDetector gestures{};
    gestures.Clear();
    TickInput tap1{};
    tap1.pressed = (1U << 2U);
    tap1.held = (1U << 2U);
    gestures.Feed(tap1, 12);
    TickInput release1{};
    release1.released = (1U << 2U);
    gestures.Feed(release1, 12);
    gestures.Feed(tap1, 12);
    EN_CHECK(stats, gestures.WasDoubleTapped(2U));

    for (u32 i = 0; i < 25U; ++i) {
        TickInput holdTick{};
        holdTick.held = (1U << 4U);
        gestures.Feed(holdTick);
    }
    EN_CHECK(stats, gestures.HoldChargeRatio(4U, 25) >= 0.99f);
    TickInput relCharge{};
    relCharge.released = (1U << 4U);
    gestures.Feed(relCharge);
    EN_CHECK(stats, gestures.ConsumeChargedRelease(4U, 20));

    const Vec2 radDz = TickInput::ApplyRadialDeadzone({0.10f, 0.05f}, 0.20f, 0.95f);
    EN_CHECK(stats, radDz.LengthSq() == 0.0f);
    const Vec2 axDz = TickInput::ApplyAxialDeadzone({0.10f, 0.60f}, 0.15f, 0.15f);
    EN_CHECK(stats, axDz.x == 0.0f && axDz.y > 0.4f);
}

} // namespace en::tests
