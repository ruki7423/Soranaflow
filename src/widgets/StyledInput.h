#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QLabel>
#include <QHBoxLayout>

class StyledInput : public QWidget
{
    Q_OBJECT

public:
    explicit StyledInput(const QString& placeholder = "",
                         const QString& iconPath = "",
                         QWidget* parent = nullptr);

    QString text() const;
    void setText(const QString& text);
    QLineEdit* lineEdit() const;

public slots:
    void refreshTheme();

private:
    QLineEdit* m_lineEdit;
    QLabel*    m_iconLabel;
    QString    m_iconPath;
};
