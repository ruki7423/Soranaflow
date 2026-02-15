#include "AudioSettingsTab.h"
#include "OutputSettingsWidget.h"
#include "DSPSettingsWidget.h"
#include "VSTSettingsWidget.h"
#include "../../../widgets/StyledScrollArea.h"

#include <QVBoxLayout>

// ═══════════════════════════════════════════════════════════════════════
//  AudioSettingsTab — thin shell composing Output, DSP, and VST widgets
// ═══════════════════════════════════════════════════════════════════════

AudioSettingsTab::AudioSettingsTab(QWidget* parent)
    : QWidget(parent)
{
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto* scrollArea = new StyledScrollArea(this);
    scrollArea->setWidgetResizable(true);

    auto* content = new QWidget(scrollArea);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 16, 12, 16);
    layout->setSpacing(0);

    layout->addWidget(new OutputSettingsWidget(content));
    layout->addWidget(new DSPSettingsWidget(content));
    layout->addWidget(new VSTSettingsWidget(content));
    layout->addStretch();

    scrollArea->setWidget(content);
    outerLayout->addWidget(scrollArea);
}
