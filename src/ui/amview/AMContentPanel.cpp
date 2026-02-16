#include "AMContentPanel.h"
#include "../../core/ThemeManager.h"
#include "../../platform/macos/MacUtils.h"
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFontMetrics>
#include <QPushButton>
#include <QScrollBar>
#include <QNetworkReply>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <QEvent>
#include <QDateTime>
#include <QContextMenuEvent>

// Column width constants for consistent alignment
static const int COL_PLAY_WIDTH   = 36;
static const int COL_ART_WIDTH    = 40;
static const int COL_ARTIST_WIDTH = 150;
static const int COL_ALBUM_WIDTH  = 200;
static const int COL_DUR_WIDTH    = 50;

// ═════════════════════════════════════════════════════════════════════
//  Constructor — scroll area with results container
// ═════════════════════════════════════════════════════════════════════

AMContentPanel::AMContentPanel(QWidget* parent)
    : QWidget(parent)
    , m_networkManager(new QNetworkAccessManager(this))
{
    m_networkManager->setTransferTimeout(15000);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setFocusPolicy(Qt::NoFocus);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setStyleSheet(
        QStringLiteral("QScrollArea { background: transparent; border: none; }") +
        ThemeManager::instance()->scrollbarStyle());

    m_resultsContainer = new QWidget(m_scrollArea);
    m_resultsContainer->setStyleSheet(QStringLiteral("background: transparent;"));
    m_resultsContainer->setFocusPolicy(Qt::NoFocus);
    m_resultsContainer->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_resultsLayout = new QVBoxLayout(m_resultsContainer);
    m_resultsLayout->setContentsMargins(0, 0, 0, 0);
    m_resultsLayout->setSpacing(16);
    m_resultsLayout->addStretch();

    m_scrollArea->setWidget(m_resultsContainer);
    layout->addWidget(m_scrollArea, 1);

    // macOS: allow clicks on inactive window to pass through
    QTimer::singleShot(0, this, [this]() {
        enableAcceptsFirstMouse(m_scrollArea);
    });
}

void AMContentPanel::clear()
{
    QLayoutItem* item;
    while ((item = m_resultsLayout->takeAt(0)) != nullptr) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
}

void AMContentPanel::refreshScrollStyle()
{
    m_scrollArea->setStyleSheet(
        QStringLiteral("QScrollArea { background: transparent; border: none; }") +
        ThemeManager::instance()->scrollbarStyle());
}

// ═════════════════════════════════════════════════════════════════════
//  Section builders
// ═════════════════════════════════════════════════════════════════════

void AMContentPanel::buildSongsSection(const QString& header, const QJsonArray& songs)
{
    m_resultsLayout->addWidget(createSectionHeader(header));
    for (const auto& val : songs)
        m_resultsLayout->addWidget(createSongRow(val.toObject()));
}

void AMContentPanel::buildAlbumsGrid(const QString& header, const QJsonArray& albums)
{
    m_resultsLayout->addWidget(createSectionHeader(header));

    auto* flowContainer = new QWidget(m_resultsContainer);
    auto* flowLayout = new QGridLayout(flowContainer);
    flowLayout->setContentsMargins(0, 0, 0, 0);
    flowLayout->setSpacing(12);

    int cols = qMax(2, (m_scrollArea->viewport()->width() - 24) / 172);
    int cardWidth = 160;

    for (int i = 0; i < albums.size(); ++i) {
        auto obj = albums[i].toObject();
        auto* card = createAlbumCard(obj, cardWidth);
        flowLayout->addWidget(card, i / cols, i % cols);
    }

    m_resultsLayout->addWidget(flowContainer);
}

void AMContentPanel::buildArtistsGrid(const QString& header, const QJsonArray& artists)
{
    m_resultsLayout->addWidget(createSectionHeader(header));

    auto* flowContainer = new QWidget(m_resultsContainer);
    auto* flowLayout = new QGridLayout(flowContainer);
    flowLayout->setContentsMargins(0, 0, 0, 0);
    flowLayout->setSpacing(12);

    int cols = qMax(2, (m_scrollArea->viewport()->width() - 24) / 142);
    int cardWidth = 130;

    for (int i = 0; i < artists.size(); ++i) {
        auto obj = artists[i].toObject();
        auto* card = createArtistCard(obj, cardWidth);
        flowLayout->addWidget(card, i / cols, i % cols);
    }

    m_resultsLayout->addWidget(flowContainer);
}

// ═════════════════════════════════════════════════════════════════════
//  Widget factories
// ═════════════════════════════════════════════════════════════════════

QWidget* AMContentPanel::createSectionHeader(const QString& title)
{
    auto c = ThemeManager::instance()->colors();
    auto* label = new QLabel(title, m_resultsContainer);
    QFont f = label->font();
    f.setPixelSize(16);
    f.setBold(true);
    label->setFont(f);
    label->setStyleSheet(QStringLiteral("color: %1; padding: 4px 0;").arg(c.foreground));
    return label;
}

QWidget* AMContentPanel::createSongRow(const QJsonObject& song)
{
    auto c = ThemeManager::instance()->colors();

    auto* row = new QWidget(m_resultsContainer);
    row->setObjectName(QStringLiteral("songRow"));
    row->setFixedHeight(48);
    row->setFocusPolicy(Qt::NoFocus);
    row->setAttribute(Qt::WA_MacShowFocusRect, false);
    row->setStyleSheet(QStringLiteral(
        "#songRow, #songRow * { border: none; outline: none; }"
        "#songRow { background: transparent; border-radius: 6px; }"
        "#songRow:hover { background: %1; }"
        "#songRow QLabel { background: transparent; }"
        "#songRow QPushButton { background: transparent; }").arg(c.hover));

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(10);

    // Play button
    auto* playBtn = new QPushButton(row);
    playBtn->setIcon(ThemeManager::instance()->cachedIcon(QStringLiteral(":/icons/play.svg")));
    playBtn->setIconSize(QSize(16, 16));
    playBtn->setFixedSize(COL_PLAY_WIDTH, COL_PLAY_WIDTH);
    playBtn->setFlat(true);
    playBtn->setCursor(Qt::PointingHandCursor);
    playBtn->setFocusPolicy(Qt::NoFocus);
    playBtn->setAttribute(Qt::WA_MacShowFocusRect, false);
    playBtn->setAttribute(Qt::WA_NoMousePropagation);
    playBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none; border-radius: %1px; outline: none; }"
        "QPushButton:hover { background: %2; }"
        "QPushButton:focus { outline: none; border: none; }"
        "QPushButton:active { outline: none; border: none; }"
        "QPushButton:pressed { outline: none; border: none; }").arg(COL_PLAY_WIDTH / 2).arg(c.accentMuted));
    layout->addWidget(playBtn);

    // Artwork thumbnail
    auto* artLabel = new QLabel(row);
    artLabel->setFixedSize(COL_ART_WIDTH, COL_ART_WIDTH);
    artLabel->setFocusPolicy(Qt::NoFocus);
    artLabel->setAttribute(Qt::WA_MacShowFocusRect, false);
    artLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    artLabel->setStyleSheet(QStringLiteral(
        "background: %1; border-radius: 4px;").arg(c.backgroundSecondary));
    artLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(artLabel);

    QString artworkUrl = song[QStringLiteral("artworkUrl")].toString();
    if (!artworkUrl.isEmpty())
        loadArtwork(artworkUrl, artLabel, COL_ART_WIDTH);

    // Title — stretches to fill
    auto* titleLabel = new QLabel(row);
    titleLabel->setFocusPolicy(Qt::NoFocus);
    titleLabel->setAttribute(Qt::WA_MacShowFocusRect, false);
    titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    titleLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;").arg(c.foreground));
    titleLabel->setText(song[QStringLiteral("title")].toString());
    titleLabel->setMinimumWidth(100);
    titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    titleLabel->setTextFormat(Qt::PlainText);
    layout->addWidget(titleLabel, 1);

    // Artist — clickable
    auto* artistLabel = new QLabel(row);
    artistLabel->setFocusPolicy(Qt::NoFocus);
    artistLabel->setAttribute(Qt::WA_MacShowFocusRect, false);
    artistLabel->setFixedWidth(COL_ARTIST_WIDTH);
    {
        QString artistName = song[QStringLiteral("artist")].toString();
        QFontMetrics fm(artistLabel->font());
        artistLabel->setText(fm.elidedText(artistName, Qt::ElideRight, COL_ARTIST_WIDTH));
    }
    artistLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: %1; font-size: 12px; }"
        "QLabel:hover { color: %2; text-decoration: underline; }")
        .arg(c.foregroundSecondary, c.accent));
    QString songArtistId = song[QStringLiteral("artistId")].toString();
    if (!songArtistId.isEmpty()) {
        artistLabel->setCursor(Qt::PointingHandCursor);
        artistLabel->setProperty("artistId", songArtistId);
        artistLabel->setProperty("artistName", song[QStringLiteral("artist")].toString());
        artistLabel->installEventFilter(this);
    }
    layout->addWidget(artistLabel);

    // Album — clickable
    auto* albumLabel = new QLabel(row);
    albumLabel->setFocusPolicy(Qt::NoFocus);
    albumLabel->setAttribute(Qt::WA_MacShowFocusRect, false);
    albumLabel->setFixedWidth(COL_ALBUM_WIDTH);
    {
        QString albumName = song[QStringLiteral("album")].toString();
        QFontMetrics fm(albumLabel->font());
        albumLabel->setText(fm.elidedText(albumName, Qt::ElideRight, COL_ALBUM_WIDTH));
    }
    albumLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: %1; font-size: 12px; }"
        "QLabel:hover { color: %2; text-decoration: underline; }")
        .arg(c.foregroundMuted, c.accent));
    QString songAlbumId = song[QStringLiteral("albumId")].toString();
    if (!songAlbumId.isEmpty()) {
        albumLabel->setCursor(Qt::PointingHandCursor);
        albumLabel->setProperty("albumId", songAlbumId);
        albumLabel->setProperty("albumName", song[QStringLiteral("album")].toString());
        albumLabel->setProperty("albumArtist", song[QStringLiteral("artist")].toString());
        albumLabel->installEventFilter(this);
    }
    layout->addWidget(albumLabel);

    // Duration
    int secs = static_cast<int>(song[QStringLiteral("duration")].toDouble());
    auto* durLabel = new QLabel(
        QStringLiteral("%1:%2").arg(secs / 60).arg(secs % 60, 2, 10, QLatin1Char('0')), row);
    durLabel->setFocusPolicy(Qt::NoFocus);
    durLabel->setAttribute(Qt::WA_MacShowFocusRect, false);
    durLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    durLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(c.foregroundMuted));
    durLabel->setFixedWidth(COL_DUR_WIDTH);
    durLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(durLabel);

    // Play button → signal
    QJsonObject songCopy = song;
    connect(playBtn, &QPushButton::clicked, this, [this, songCopy]() {
        emit songPlayRequested(songCopy);
    });

    // Store song data for double-click handling
    row->setProperty("songId", song[QStringLiteral("id")].toString());
    row->setProperty("songTitle", song[QStringLiteral("title")].toString());
    row->setProperty("songArtist", song[QStringLiteral("artist")].toString());
    row->setProperty("songAlbum", song[QStringLiteral("album")].toString());
    row->setProperty("songDuration", song[QStringLiteral("duration")].toDouble());
    row->setProperty("songArtwork", song[QStringLiteral("artworkUrl")].toString());
    row->installEventFilter(this);

    return row;
}

QWidget* AMContentPanel::createAlbumCard(const QJsonObject& album, int cardWidth)
{
    auto c = ThemeManager::instance()->colors();
    int textWidth = cardWidth - 16;

    auto* card = new QWidget(m_resultsContainer);
    card->setObjectName(QStringLiteral("albumCard"));
    card->setFixedWidth(cardWidth);
    card->setCursor(Qt::PointingHandCursor);
    card->setStyleSheet(QStringLiteral(
        "#albumCard { background: transparent; border-radius: 8px; }"
        "#albumCard:hover { background: %1; }").arg(c.hover));

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    // Artwork
    int artSize = cardWidth - 16;
    auto* artLabel = new QLabel(card);
    artLabel->setFixedSize(artSize, artSize);
    artLabel->setStyleSheet(QStringLiteral(
        "background: %1; border-radius: 8px;").arg(c.backgroundSecondary));
    artLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(artLabel, 0, Qt::AlignCenter);

    QString artworkUrl = album[QStringLiteral("artworkUrl")].toString();
    if (!artworkUrl.isEmpty())
        loadArtwork(artworkUrl, artLabel, artSize);

    // Title — max 2 lines
    QString titleText = album[QStringLiteral("title")].toString();
    auto* titleLabel = new QLabel(card);
    titleLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; font-weight: bold;").arg(c.foreground));
    titleLabel->setFixedWidth(textWidth);
    titleLabel->setWordWrap(true);
    {
        QFontMetrics fm(titleLabel->font());
        int lineHeight = fm.height();
        titleLabel->setFixedHeight(lineHeight * 2 + 2);
        QString elided = fm.elidedText(titleText, Qt::ElideRight, textWidth * 2 - fm.averageCharWidth());
        titleLabel->setText(elided);
    }
    layout->addWidget(titleLabel);

    // Artist — single line, elided
    QString artistText = album[QStringLiteral("artist")].toString();
    auto* artistLabel = new QLabel(card);
    artistLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(c.foregroundMuted));
    artistLabel->setFixedWidth(textWidth);
    QFontMetrics fm(artistLabel->font());
    artistLabel->setText(fm.elidedText(artistText, Qt::ElideRight, textWidth));
    layout->addWidget(artistLabel);

    // Click handler via eventFilter
    card->installEventFilter(this);
    card->setProperty("albumId", album[QStringLiteral("id")].toString());
    card->setProperty("albumName", titleText);
    card->setProperty("albumArtist", artistText);

    return card;
}

QWidget* AMContentPanel::createArtistCard(const QJsonObject& artist, int cardWidth)
{
    auto c = ThemeManager::instance()->colors();

    auto* card = new QWidget(m_resultsContainer);
    card->setObjectName(QStringLiteral("artistCard"));
    card->setFixedWidth(cardWidth);
    card->setCursor(Qt::PointingHandCursor);
    card->setStyleSheet(QStringLiteral(
        "#artistCard { background: transparent; border-radius: 8px; }"
        "#artistCard:hover { background: %1; }").arg(c.hover));

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    layout->setAlignment(Qt::AlignHCenter);

    // Circular artwork
    int artSize = cardWidth - 24;
    auto* artLabel = new QLabel(card);
    artLabel->setFixedSize(artSize, artSize);
    artLabel->setStyleSheet(QStringLiteral(
        "background: %1; border-radius: %2px;")
            .arg(c.backgroundSecondary).arg(artSize / 2));
    artLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(artLabel, 0, Qt::AlignCenter);

    QString artworkUrl = artist[QStringLiteral("artworkUrl")].toString();
    if (!artworkUrl.isEmpty())
        loadArtwork(artworkUrl, artLabel, artSize, true);

    // Name
    auto* nameLabel = new QLabel(artist[QStringLiteral("name")].toString(), card);
    nameLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; font-weight: bold;").arg(c.foreground));
    nameLabel->setAlignment(Qt::AlignCenter);
    nameLabel->setWordWrap(true);
    nameLabel->setMaximumHeight(32);
    layout->addWidget(nameLabel);

    // Click to view discography
    card->installEventFilter(this);
    card->setProperty("artistId", artist[QStringLiteral("id")].toString());
    card->setProperty("artistName", artist[QStringLiteral("name")].toString());

    return card;
}

// ═════════════════════════════════════════════════════════════════════
//  loadArtwork — async network fetch
// ═════════════════════════════════════════════════════════════════════

void AMContentPanel::loadArtwork(const QString& url, QLabel* target, int size, bool circular)
{
    QString resolvedUrl = url;
    resolvedUrl.replace(QStringLiteral("{w}"), QString::number(size * 2));
    resolvedUrl.replace(QStringLiteral("{h}"), QString::number(size * 2));

    QNetworkRequest req{QUrl(resolvedUrl)};
    QNetworkReply* reply = m_networkManager->get(req);

    QPointer<QLabel> safeTarget = target;
    connect(reply, &QNetworkReply::finished, this, [reply, safeTarget, size, circular]() {
        reply->deleteLater();
        if (!safeTarget) return;
        if (reply->error() != QNetworkReply::NoError) return;

        QPixmap pm;
        pm.loadFromData(reply->readAll());
        if (pm.isNull()) return;

        pm = pm.scaled(size * 2, size * 2, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

        if (circular) {
            QPixmap circPm(size * 2, size * 2);
            circPm.fill(Qt::transparent);
            QPainter painter(&circPm);
            painter.setRenderHint(QPainter::Antialiasing);
            QPainterPath path;
            path.addEllipse(0, 0, size * 2, size * 2);
            painter.setClipPath(path);
            painter.drawPixmap(0, 0, pm);
            painter.end();
            safeTarget->setPixmap(circPm.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            int radius = 8;
            QPixmap rounded(size * 2, size * 2);
            rounded.fill(Qt::transparent);
            QPainter painter(&rounded);
            painter.setRenderHint(QPainter::Antialiasing);
            QPainterPath path;
            path.addRoundedRect(0, 0, size * 2, size * 2, radius * 2, radius * 2);
            painter.setClipPath(path);
            painter.drawPixmap(0, 0, pm);
            painter.end();
            safeTarget->setPixmap(rounded.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    });
}

// ═════════════════════════════════════════════════════════════════════
//  Event filter — double-click play, artist/album click, context menu
// ═════════════════════════════════════════════════════════════════════

bool AMContentPanel::eventFilter(QObject* obj, QEvent* event)
{
    // Helper: resolve the song row for an object
    auto findSongRow = [](QObject* o) -> QObject* {
        if (!o->property("songId").toString().isEmpty())
            return o;
        QObject* p = o->parent();
        if (p && !p->property("songId").toString().isEmpty())
            return p;
        return nullptr;
    };

    // Helper: reconstruct song JSON from row properties
    auto songDataFromRow = [](QObject* row) -> QJsonObject {
        QJsonObject data;
        data[QStringLiteral("id")] = row->property("songId").toString();
        data[QStringLiteral("title")] = row->property("songTitle").toString();
        data[QStringLiteral("artist")] = row->property("songArtist").toString();
        data[QStringLiteral("album")] = row->property("songAlbum").toString();
        data[QStringLiteral("duration")] = row->property("songDuration").toDouble();
        data[QStringLiteral("artworkUrl")] = row->property("songArtwork").toString();
        return data;
    };

    // Double-click detection (MouseButtonPress)
    if (event->type() == QEvent::MouseButtonPress) {
        QObject* songRow = findSongRow(obj);
        if (songRow) {
            qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (m_lastClickedRow == songRow && (now - m_lastClickTime) < 500) {
                m_lastClickedRow = nullptr;
                m_lastClickTime = 0;
                if (now - m_lastPlayTime > 1000) {
                    m_lastPlayTime = now;
                    emit songPlayRequested(songDataFromRow(songRow));
                    return true;
                }
            } else {
                m_lastClickedRow = songRow;
                m_lastClickTime = now;
            }
        }
    }

    // Artist/album label single-click navigation
    if (event->type() == QEvent::MouseButtonRelease) {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastPlayTime < 500)
            return QWidget::eventFilter(obj, event);

        // Artist card or artist label click
        QString artistId = obj->property("artistId").toString();
        if (!artistId.isEmpty()) {
            QObject* songRow = findSongRow(obj);
            if (songRow && m_lastClickedRow == songRow)
                return QWidget::eventFilter(obj, event);
            emit artistNavigationRequested(artistId, obj->property("artistName").toString());
            return true;
        }

        // Album card or album label click
        QString albumId = obj->property("albumId").toString();
        if (!albumId.isEmpty()) {
            QObject* songRow = findSongRow(obj);
            if (songRow && m_lastClickedRow == songRow)
                return QWidget::eventFilter(obj, event);
            emit albumNavigationRequested(albumId,
                obj->property("albumName").toString(),
                obj->property("albumArtist").toString());
            return true;
        }
    }

    // Native double-click backup
    if (event->type() == QEvent::MouseButtonDblClick) {
        QObject* songRow = findSongRow(obj);
        if (songRow) {
            qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastPlayTime > 1000) {
                m_lastPlayTime = now;
                emit songPlayRequested(songDataFromRow(songRow));
            }
            return true;
        }
    }

    // Right-click context menu
    if (event->type() == QEvent::ContextMenu) {
        QObject* songRow = findSongRow(obj);
        if (songRow) {
            auto* cmEvent = static_cast<QContextMenuEvent*>(event);
            emit songContextMenuRequested(cmEvent->globalPos(), songDataFromRow(songRow));
            return true;
        }
    }

    return QWidget::eventFilter(obj, event);
}
