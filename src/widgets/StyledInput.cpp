#include "StyledInput.h"
#include "../core/ThemeManager.h"
#include <QPixmap>
#include <QDebug>

StyledInput::StyledInput(const QString& placeholder,
                         const QString& iconPath,
                         QWidget* parent)
    : QWidget(parent)
    , m_lineEdit(new QLineEdit(this))
    , m_iconLabel(nullptr)
    , m_iconPath(iconPath)
{
    setObjectName(QStringLiteral("StyledInput"));
    setFixedHeight(UISizes::inputHeight);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 0, 8, 0);
    layout->setSpacing(6);

    // Optional icon — use themedIcon for theme-aware color
    if (!iconPath.isEmpty()) {
        m_iconLabel = new QLabel(this);
        m_iconLabel->setFixedSize(UISizes::smallIconSize, UISizes::smallIconSize);
        m_iconLabel->setPixmap(
            ThemeManager::instance()->themedIcon(iconPath).pixmap(UISizes::smallIconSize, UISizes::smallIconSize));
        layout->addWidget(m_iconLabel);
    }

    // Line edit
    m_lineEdit->setPlaceholderText(placeholder);
    m_lineEdit->setFrame(false);
    m_lineEdit->setObjectName(QStringLiteral("StyledInputField"));
    layout->addWidget(m_lineEdit);

    setLayout(layout);

    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &StyledInput::refreshTheme);
}

QString StyledInput::text() const
{
    return m_lineEdit->text();
}

void StyledInput::setText(const QString& text)
{
    m_lineEdit->setText(text);
}

QLineEdit* StyledInput::lineEdit() const
{
    return m_lineEdit;
}

void StyledInput::refreshTheme()
{
    if (m_iconLabel && !m_iconPath.isEmpty()) {
        m_iconLabel->setPixmap(
            ThemeManager::instance()->themedIcon(m_iconPath).pixmap(UISizes::smallIconSize, UISizes::smallIconSize));
    }
}
