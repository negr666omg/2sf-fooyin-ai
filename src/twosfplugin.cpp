#include "twosfplugin.h"
#include "twosfinput.h"
#include "twosfsettings.h"

#include <core/engine/audioloader.h>

namespace Fooyin::TwoSF {
namespace {
class TwoSFSettingsProvider final : public PluginSettingsProvider {
protected:
    QDialog* createSettings(QWidget* parent) override {
        return new TwoSFSettings(parent);
    }
};
} // namespace

void TwoSFPlugin::initialise(const CorePluginContext& context) {
    m_audioLoader = context.audioLoader;
}

QString TwoSFPlugin::inputName() const {
    return QStringLiteral("2SF");
}

InputCreator TwoSFPlugin::inputCreator() const {
    InputCreator creator;
    creator.priority = 210;
    creator.decoder = [] { return std::make_unique<TwoSFDecoder>(); };
    creator.reader = [] { return std::make_unique<TwoSFReader>(); };
    return creator;
}

std::unique_ptr<PluginSettingsProvider> TwoSFPlugin::settingsProvider() const {
    return std::make_unique<TwoSFSettingsProvider>();
}
} // namespace Fooyin::TwoSF

#include "moc_twosfplugin.cpp"
