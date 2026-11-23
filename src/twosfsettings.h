#pragma once

#include <core/coresettings.h>
#include <QDialog>

class QComboBox;

namespace Fooyin::TwoSF {
class TwoSFSettings final : public QDialog {
    Q_OBJECT
public:
    explicit TwoSFSettings(QWidget* parent = nullptr);
    void accept() override;
private:
    FySettings m_settings;
    QComboBox* m_interpolation{};
};
} // namespace Fooyin::TwoSF
