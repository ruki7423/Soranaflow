#pragma once
#include <QWidget>

#ifdef Q_OS_MAC
void enableAcceptsFirstMouse(QWidget* widget);
#else
inline void enableAcceptsFirstMouse(QWidget*) {}
#endif
