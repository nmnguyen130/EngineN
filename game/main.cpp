#include "game/runtime/VeilGame.h"
#include <cstring>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--test") == 0) {
            return en::veil::VeilGame::RunVerificationSuite() ? 0 : 1;
        }
    }

    en::CrashHandler::Install();
    en::Application app{};
    en::veil::VeilGame game{};
    en::ApplicationConfig cfg{};
    cfg.appName = "Veil of the Silent (EngineN 2026 Final Production)";
    if (!app.Init(cfg, game.BuildAppHooks())) return 1;

    app.Input().Map()
        .BindAxis(en::veil::VeilAxis::MoveX, en::AxisBinding{en::InputSource::Key(4), en::InputSource::Key(7)})
        .Bind(en::veil::VeilAction::Jump, {en::InputSource::Key(44), en::InputSource::Key(26)})
        .Bind(en::veil::VeilAction::Dash, {en::InputSource::Key(225)})
        .Bind(en::veil::VeilAction::Attack, {en::InputSource::Key(13), en::InputSource::Mouse(1)})
        .Bind(en::veil::VeilAction::Parry, {en::InputSource::Key(14), en::InputSource::Mouse(3)})
        .Bind(en::veil::VeilAction::ThrowShuriken, {en::InputSource::Key(24)})
        .Bind(en::veil::VeilAction::Grapple, {en::InputSource::Key(12)})
        .Bind(en::veil::VeilAction::Pause, {en::InputSource::Key(41)});

    (void)app.Run();
    return 0;
}
