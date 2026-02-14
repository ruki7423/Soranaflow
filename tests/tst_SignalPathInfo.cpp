#include <QtTest/QtTest>
#include "SignalPathInfo.h"

class tst_SignalPathInfo : public QObject {
    Q_OBJECT

private slots:
    // ── overallQuality ───────────────────────────────────────────
    void overallQuality_emptyNodes()
    {
        SignalPathInfo info;
        QCOMPARE(info.overallQuality(), SignalPathNode::Unknown);
    }

    void overallQuality_singleLossless()
    {
        SignalPathInfo info;
        info.nodes.append({QStringLiteral("Source"), {}, {}, SignalPathNode::Lossless});
        QCOMPARE(info.overallQuality(), SignalPathNode::Lossless);
    }

    void overallQuality_worstWins()
    {
        SignalPathInfo info;
        info.nodes.append({QStringLiteral("Source"), {}, {}, SignalPathNode::BitPerfect});
        info.nodes.append({QStringLiteral("DSP"), {}, {}, SignalPathNode::Enhanced});
        info.nodes.append({QStringLiteral("Output"), {}, {}, SignalPathNode::Lossless});
        // Enhanced (3) > Lossless (1) > BitPerfect (0) → worst is Enhanced
        QCOMPARE(info.overallQuality(), SignalPathNode::Enhanced);
    }

    void overallQuality_allBitPerfect()
    {
        SignalPathInfo info;
        info.nodes.append({QStringLiteral("Source"), {}, {}, SignalPathNode::BitPerfect});
        info.nodes.append({QStringLiteral("Output"), {}, {}, SignalPathNode::BitPerfect});
        // BitPerfect (0) > Lossless (1)? No — BitPerfect < Lossless in enum value
        // overallQuality starts with worst=Lossless, only updates if node > worst
        // BitPerfect (0) is NOT > Lossless (1), so worst stays Lossless
        QCOMPARE(info.overallQuality(), SignalPathNode::Lossless);
    }

    void overallQuality_lossy_dominates()
    {
        SignalPathInfo info;
        info.nodes.append({QStringLiteral("Source"), {}, {}, SignalPathNode::HighRes});
        info.nodes.append({QStringLiteral("DSP"), {}, {}, SignalPathNode::Lossy});
        info.nodes.append({QStringLiteral("Output"), {}, {}, SignalPathNode::Lossless});
        QCOMPARE(info.overallQuality(), SignalPathNode::Lossy);
    }

    void overallQuality_unknown_dominates()
    {
        SignalPathInfo info;
        info.nodes.append({QStringLiteral("Source"), {}, {}, SignalPathNode::Lossless});
        info.nodes.append({QStringLiteral("?"), {}, {}, SignalPathNode::Unknown});
        QCOMPARE(info.overallQuality(), SignalPathNode::Unknown);
    }

    // ── qualityLabel ─────────────────────────────────────────────
    void qualityLabel_allValues()
    {
        QCOMPARE(SignalPathInfo::qualityLabel(SignalPathNode::BitPerfect),
                 QStringLiteral("Bit-Perfect"));
        QCOMPARE(SignalPathInfo::qualityLabel(SignalPathNode::Lossless),
                 QStringLiteral("Lossless"));
        QCOMPARE(SignalPathInfo::qualityLabel(SignalPathNode::HighRes),
                 QStringLiteral("High-Res"));
        QCOMPARE(SignalPathInfo::qualityLabel(SignalPathNode::Enhanced),
                 QStringLiteral("Enhanced"));
        QCOMPARE(SignalPathInfo::qualityLabel(SignalPathNode::Lossy),
                 QStringLiteral("Lossy"));
        QCOMPARE(SignalPathInfo::qualityLabel(SignalPathNode::Unknown),
                 QStringLiteral("Unknown"));
    }

    // ── isAppleMusic default ─────────────────────────────────────
    void isAppleMusic_defaultFalse()
    {
        SignalPathInfo info;
        QVERIFY(!info.isAppleMusic);
    }

    // ── SignalPathNode defaults ───────────────────────────────────
    void node_defaultQuality()
    {
        SignalPathNode node;
        QCOMPARE(node.quality, SignalPathNode::Unknown);
        QVERIFY(node.label.isEmpty());
        QVERIFY(node.detail.isEmpty());
        QVERIFY(node.sublabel.isEmpty());
    }
};

QTEST_MAIN(tst_SignalPathInfo)
#include "tst_SignalPathInfo.moc"
