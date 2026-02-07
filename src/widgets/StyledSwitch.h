#pragma once

#include <QAbstractButton>

class StyledSwitch : public QAbstractButton
{
    Q_OBJECT

public:
    explicit StyledSwitch(QWidget* parent = nullptr);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
};
