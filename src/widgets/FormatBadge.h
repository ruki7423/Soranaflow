#pragma once

#include <QWidget>
#include <QLabel>
#include <QHBoxLayout>
#include "../core/MusicData.h"

class FormatBadge : public QWidget
{
    Q_OBJECT

public:
    explicit FormatBadge(AudioFormat format,
                         const QString& sampleRate = "",
                         const QString& bitDepth = "",
                         const QString& bitrate = "",
                         QWidget* parent = nullptr);

private:
    QColor resolveFormatColor(AudioFormat format,
                              const QString& sampleRate,
                              const QString& bitDepth) const;

    QLabel* m_formatLabel;
    QLabel* m_specsLabel;
};
