#pragma once

#include <QPushButton>
#include <QIcon>
#include <QString>

class StyledButton : public QPushButton
{
    Q_OBJECT
    Q_PROPERTY(QString variant READ buttonVariant WRITE setButtonVariant)
    Q_PROPERTY(QString buttonSize READ buttonSize WRITE setButtonSize)

public:
    explicit StyledButton(const QString& text = "",
                          const QString& variant = "default",
                          QWidget* parent = nullptr);

    StyledButton(const QIcon& icon,
                 const QString& text,
                 const QString& variant = "default",
                 QWidget* parent = nullptr);

    QString buttonVariant() const;
    void setButtonVariant(const QString& variant);

    QString buttonSize() const;
    void setButtonSize(const QString& size);

private:
    void init(const QString& variant);
    void applySize();
    void applyIconSize();

    QString m_variant;
    QString m_size;
};
