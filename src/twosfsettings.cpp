#include "twosfsettings.h"
#include "twosfdefs.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>

namespace Fooyin::TwoSF {
TwoSFSettings::TwoSFSettings(QWidget* parent)
    : QDialog(parent), m_interpolation(new QComboBox(this)) {
    setWindowTitle(tr("2SF Settings"));
    m_interpolation->addItem(tr("None (fastest)"), 0);
    m_interpolation->addItem(tr("BLEP"), 1);
    m_interpolation->addItem(tr("Linear"), 2);
    m_interpolation->addItem(tr("Cubic"), 3);
    m_interpolation->addItem(tr("Sinc (highest quality)"), 4);

    const int saved = m_settings.value(InterpolationSetting, DefaultInterpolation).toInt();
    const int index = m_interpolation->findData(saved);
    m_interpolation->setCurrentIndex(index >= 0 ? index : 4);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &TwoSFSettings::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &TwoSFSettings::reject);

    auto* layout = new QFormLayout(this);
    layout->addRow(tr("Resampling quality"), m_interpolation);
    layout->addRow(buttons);
}

void TwoSFSettings::accept() {
    m_settings.setValue(InterpolationSetting, m_interpolation->currentData().toInt());
    m_settings.sync();
    QDialog::accept();
}
} // namespace Fooyin::TwoSF
