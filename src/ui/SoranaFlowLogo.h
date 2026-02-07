#pragma once
#include <QWidget>
#include <QPainter>
#include <QSvgRenderer>

class SoranaFlowLogo : public QWidget {
    Q_OBJECT
public:
    explicit SoranaFlowLogo(int size = 32, QWidget* parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    int m_size;
    QSvgRenderer m_renderer;
};
