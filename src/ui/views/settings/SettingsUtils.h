#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include "../../../core/ThemeManager.h"

namespace SettingsUtils {

inline QWidget* createSettingRow(const QString& label,
                                  const QString& description,
                                  QWidget* control)
{
    auto* row = new QWidget();
    row->setObjectName(QStringLiteral("settingRow"));
    row->setMinimumHeight(UISizes::rowHeight);
    row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    row->setStyleSheet(
        QStringLiteral("#settingRow { border-bottom: 1px solid %1; }")
            .arg(ThemeManager::instance()->colors().borderSubtle));

    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 8, 0, 8);
    rowLayout->setSpacing(16);

    auto* textLayout = new QVBoxLayout();
    textLayout->setSpacing(2);

    auto* labelWidget = new QLabel(label, row);
    labelWidget->setStyleSheet(
        QStringLiteral("color: %1; font-size: 14px; font-weight: bold; border: none;")
            .arg(ThemeManager::instance()->colors().foreground));
    textLayout->addWidget(labelWidget);

    if (!description.isEmpty()) {
        auto* descWidget = new QLabel(description, row);
        descWidget->setStyleSheet(
            QStringLiteral("color: %1; font-size: 12px; border: none;")
                .arg(ThemeManager::instance()->colors().foregroundMuted));
        descWidget->setWordWrap(true);
        textLayout->addWidget(descWidget);
    }

    rowLayout->addLayout(textLayout, 1);

    if (control) {
        rowLayout->addWidget(control, 0, Qt::AlignVCenter);
    }

    return row;
}

inline QWidget* createSectionHeader(const QString& title)
{
    auto* header = new QLabel(title);
    header->setStyleSheet(
        QStringLiteral("color: %1; font-size: 16px; font-weight: bold;"
                       " border: none; padding: 0px;")
            .arg(ThemeManager::instance()->colors().foreground));
    header->setContentsMargins(0, 16, 0, 8);
    return header;
}

} // namespace SettingsUtils
