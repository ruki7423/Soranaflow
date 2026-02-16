#include "DSPSettingsWidget.h"
#include "ProcessingSettingsWidget.h"
#include "SpatialSettingsWidget.h"
#include "EQSettingsWidget.h"

#include <QVBoxLayout>

DSPSettingsWidget::DSPSettingsWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    layout->addWidget(new ProcessingSettingsWidget(this));
    layout->addWidget(new SpatialSettingsWidget(this));
    layout->addWidget(new EQSettingsWidget(this));
}
