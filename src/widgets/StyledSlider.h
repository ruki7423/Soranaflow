#pragma once

#include <QSlider>

class StyledSlider : public QSlider
{
    Q_OBJECT
    Q_PROPERTY(bool showHandle READ showHandle WRITE setShowHandle)

public:
    explicit StyledSlider(QWidget* parent = nullptr);

    bool showHandle() const;
    void setShowHandle(bool show);

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    bool m_showHandle;
};
