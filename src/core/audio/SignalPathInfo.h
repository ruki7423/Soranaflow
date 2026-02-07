#pragma once

#include <QString>
#include <QVector>

struct SignalPathNode {
    enum Quality { BitPerfect, Lossless, HighRes, Enhanced, Lossy, Unknown };

    QString label;
    QString detail;
    QString sublabel;
    Quality quality = Unknown;
};

struct SignalPathInfo {
    QVector<SignalPathNode> nodes;
    bool isAppleMusic = false;

    // Overall quality is the worst (highest enum value) of all nodes
    SignalPathNode::Quality overallQuality() const {
        if (nodes.isEmpty()) return SignalPathNode::Unknown;
        SignalPathNode::Quality worst = SignalPathNode::Lossless;
        for (const auto& n : nodes) {
            if (n.quality > worst) worst = n.quality;
        }
        return worst;
    }

    static QString qualityLabel(SignalPathNode::Quality q) {
        switch (q) {
        case SignalPathNode::BitPerfect: return QStringLiteral("Bit-Perfect");
        case SignalPathNode::Lossless:   return QStringLiteral("Lossless");
        case SignalPathNode::HighRes:    return QStringLiteral("High-Res");
        case SignalPathNode::Enhanced:   return QStringLiteral("Enhanced");
        case SignalPathNode::Lossy:      return QStringLiteral("Lossy");
        default:                         return QStringLiteral("Unknown");
        }
    }
};
