#include "tests/TestRunner.h"
#include "audio/AudioSystem.h"
#include "renderer/Renderer2D.h"

namespace en::tests {

void RunSuiteRendererAndAudio(SuiteStats& stats) noexcept {
    // 1. Renderer2D headless init, RenderList 64-bit radix sort, LightingAndPostFx, and UiBuilder
    Window win{};
    Renderer2D ren{};
    RendererConfig rcfg{};
    rcfg.headless = true;
    EN_CHECK(stats, ren.Init(win, rcfg).has_value());

    RenderList& list = ren.BeginFrame({1280, 720});
    Camera2D cam{};
    cam.worldCenterX = 0.0f;
    cam.worldCenterY = 0.0f;
    cam.zoom = 1.0f;
    cam.pixelsPerMeter = 48.0f;
    list.SetView(0, cam);

    list.PushRect({2.0f, 3.0f, 1.0f, 1.0f}, Color{1.0f, 1.0f, 1.0f, 1.0f}, 0, 10, 40000, 0, BlendMode::Alpha);
    list.PushRect({-1.0f, 1.0f, 1.0f, 1.0f}, Color{1.0f, 1.0f, 1.0f, 1.0f}, 0, 10, 10000, 0, BlendMode::Alpha);

    LightingAndPostFx::EmitPointLightHalo(list, {0.0f, 0.0f}, 4.0f, Color{1.0f, 0.8f, 0.5f, 1.0f}, 1.0f);

    FrameInput fi{};
    UiBuilder ui{list, fi};
    ui.BeginVerticalPanel({20.0f, 20.0f, 220.0f, 120.0f}, "HUD PANEL");
    ui.ProgressBar("HP", 0.8f, Color{0.2f, 0.9f, 0.4f, 1.0f});
    ui.Label("SOULS: 100");
    ui.EndPanel();

    PostProcessConfig pfx{};
    LightingAndPostFx::EmitPostProcessOverlays(list, pfx);

    list.Compile();
    EN_CHECK(stats, list.Stats().submittedQuads >= 4U);
    EN_CHECK(stats, list.Stats().drawBatches >= 1U);
    ren.Submit();
    ren.Shutdown();

    // 2. AudioMixer & AudioSystem SPSC command queue, spatial panning, Biquad LPF, and soft-clip mixing
    AudioEngine audio{};
    AudioEngineConfig acfg{};
    acfg.headless = true;
    EN_CHECK(stats, audio.Init(acfg).has_value());

    SoundCueDef cue{};
    cue.cueId = AssetId{101ULL};
    cue.baseGain = 0.9f;
    cue.bus = AudioBusId::SfxCombat;
    EN_CHECK(stats, audio.RegisterCueDef(cue).has_value());

    AudioUpdateDesc udesc{};
    udesc.tickIndex = 1;
    AudioTriggerReq trig{AssetId{101ULL}, 2.0f, 0.0f, 1U, 0U};
    udesc.triggers = std::span<const AudioTriggerReq>(&trig, 1);
    audio.Update(udesc);

    // Verify Offscreen RenderTarget2D FBO, ShaderParamBlock Material, OcclusionCulling2D, and Software Framebuffer
    const RenderTargetHandle minimapFbo = ren.CreateRenderTarget(64, 64);
    EN_CHECK(stats, minimapFbo.IsValid() && ren.GetRenderTargetTexture(minimapFbo).IsValid());

    ShaderParamBlock glowMat{};
    glowMat.emissiveBoost = 0.35f;
    list.SetMaterial(1, glowMat);
    EN_CHECK(stats, std::abs(list.Material(1).emissiveBoost - 0.35f) < 1e-4f);

    EN_CHECK(stats, list.AddOccluder2D({10.0f, 10.0f, 8.0f, 8.0f}));
    EN_CHECK(stats, list.IsOccluded2D({11.0f, 11.0f, 2.0f, 2.0f}));
    EN_CHECK(stats, ren.CountNonBlackPixels() > 100U);

    // Verify AudioBusHierarchy (Master -> SfxCombat -> SfxPlayer) and DistanceAttenuationModel curves
    audio.SetBusGain(AudioBusId::SfxCombat, 0.5f);
    audio.SetBusGain(AudioBusId::SfxPlayer, 0.5f);
    EN_CHECK(stats, std::abs(audio.EffectiveBusGain(AudioBusId::SfxPlayer) - 0.25f) < 1e-4f);

    const f32 attLin = EvaluateDistanceAttenuation(AttenuationCurve::Linear, 5.0f, 1.0f, 9.0f);
    const f32 attLog = EvaluateDistanceAttenuation(AttenuationCurve::Logarithmic, 5.0f, 1.0f, 9.0f);
    const f32 attExp = EvaluateDistanceAttenuation(AttenuationCurve::InverseExponential, 5.0f, 1.0f, 9.0f);
    EN_CHECK(stats, std::abs(attLin - 0.5f) < 1e-4f && attLog > 0.0f && attExp > 0.0f);

    std::array<f32, 512> stereoOut{};
    audio.PumpBlocks(1, stereoOut);
    EN_CHECK(stats, audio.Telemetry().mixedBlocks >= 1U);
    audio.Shutdown();
}

} // namespace en::tests
