#pragma once

#include <QComboBox>

class StyledComboBox : public QComboBox
{
    Q_OBJECT

public:
    explicit StyledComboBox(QWidget* parent = nullptr);

public slots:
    void refreshTheme();

protected:
    void paintEvent(QPaintEvent* event) override;
};
