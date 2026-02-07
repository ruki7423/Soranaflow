#include "StyledButton.h"
#include "../core/ThemeManager.h"
#include <QCursor>
#include <QStyle>

StyledButton::StyledButton(const QString& text,
                           const QString& variant,
                           QWidget* parent)
    : QPushButton(text, parent)
    , m_variant(variant)
    , m_size(QStringLiteral("default"))
{
    init(variant);
}

StyledButton::StyledButton(const QIcon& icon,
                           const QString& text,
                           const QString& variant,
                           QWidget* parent)
    : QPushButton(icon, text, parent)
    , m_variant(variant)
    , m_size(QStringLiteral("default"))
{
    init(variant);
}

void StyledButton::init(const QString& /*variant*/)
{
    setObjectName(QStringLiteral("StyledButton"));
    setCursor(Qt::PointingHandCursor);
    // m_variant and m_size are already set in the constructor initializer list.
    // Do NOT call setProperty() here — it triggers the Q_PROPERTY WRITE setter
    // which calls setProperty() again, causing infinite recursion via qt_metacall.
    applySize();
    applyIconSize();
}

QString StyledButton::buttonVariant() const
{
    return m_variant;
}

void StyledButton::setButtonVariant(const QString& variant)
{
    if (m_variant == variant)
        return;
    m_variant = variant;
    // Do NOT call setProperty("variant", ...) — this function IS the Q_PROPERTY
    // setter, so setProperty would re-enter this function via qt_metacall.
    style()->unpolish(this);
    style()->polish(this);
    update();
}

QString StyledButton::buttonSize() const
{
    return m_size;
}

void StyledButton::setButtonSize(const QString& size)
{
    if (m_size == size)
        return;
    m_size = size;
    // Do NOT call setProperty("buttonSize", ...) — same recursion issue.
    applySize();
    applyIconSize();
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void StyledButton::applySize()
{
    if (m_size == QStringLiteral("sm")) {
        setFixedHeight(UISizes::buttonHeight);
    } else if (m_size == QStringLiteral("lg")) {
        setFixedHeight(44);
    } else if (m_size == QStringLiteral("icon")) {
        setFixedSize(UISizes::buttonHeight, UISizes::buttonHeight);
    } else {
        // "default"
        setFixedHeight(UISizes::buttonHeight);
    }
}

void StyledButton::applyIconSize()
{
    if (m_size == QStringLiteral("sm")) {
        setIconSize(QSize(UISizes::buttonIconSize, UISizes::buttonIconSize));
    } else if (m_size == QStringLiteral("lg")) {
        setIconSize(QSize(24, 24));
    } else {
        // "default" and "icon"
        setIconSize(QSize(20, 20));
    }
}
