#pragma once

#include <core/engine/inputplugin.h>
#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <gui/plugins/pluginconfigguiplugin.h>

namespace Fooyin::TwoSF {
class TwoSFPlugin final : public QObject,
                          public Plugin,
                          public CorePlugin,
                          public InputPlugin,
                          public PluginConfigGuiPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID FOOYIN_PLUGIN_IID FILE "twosf.json")
    Q_INTERFACES(Fooyin::Plugin Fooyin::CorePlugin Fooyin::InputPlugin Fooyin::PluginConfigGuiPlugin)

public:
    void initialise(const CorePluginContext& context) override;
    [[nodiscard]] QString inputName() const override;
    [[nodiscard]] InputCreator inputCreator() const override;
    [[nodiscard]] std::unique_ptr<PluginSettingsProvider> settingsProvider() const override;

private:
    std::shared_ptr<AudioLoader> m_audioLoader;
};
} // namespace Fooyin::TwoSF
