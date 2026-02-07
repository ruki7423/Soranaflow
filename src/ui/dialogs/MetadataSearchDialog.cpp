#include "MetadataSearchDialog.h"
#include "../../core/ThemeManager.h"

#include <QHeaderView>
#include <QGridLayout>
#include <QFrame>

MetadataSearchDialog::MetadataSearchDialog(const Track& track, QWidget* parent)
    : QDialog(parent)
    , m_track(track)
{
    setupUI();

    // Pre-fill fields
    m_titleEdit->setText(track.title);
    m_artistEdit->setText(track.artist);
    m_albumEdit->setText(track.album);

    connect(m_searchBtn, &QPushButton::clicked, this, &MetadataSearchDialog::onSearch);
    connect(m_applyBtn, &QPushButton::clicked, this, &MetadataSearchDialog::onApply);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
}

// ── Confidence helpers ─────────────────────────────────────────────

QString MetadataSearchDialog::confidenceColor(double score) const
{
    if (score > 95.0) return QStringLiteral("#4CAF50");      // green
    if (score > 70.0) return QStringLiteral("#FFC107");      // yellow/amber
    if (score > 40.0) return QStringLiteral("#FF9800");      // orange
    return QStringLiteral("#F44336");                         // red
}

QString MetadataSearchDialog::confidenceText(double score) const
{
    if (score > 95.0) return QStringLiteral("Excellent match");
    if (score > 70.0) return QStringLiteral("Good match");
    if (score > 40.0) return QStringLiteral("Uncertain");
    return QStringLiteral("Poor match");
}

// ── UI Setup ───────────────────────────────────────────────────────

void MetadataSearchDialog::setupUI()
{
    setWindowTitle(QStringLiteral("Fix Metadata"));
    setMinimumSize(780, 600);
    resize(820, 680);

    auto* tm = ThemeManager::instance();
    auto c = tm->colors();

    setStyleSheet(QStringLiteral(
        "QDialog { background: %1; color: %2; }")
        .arg(c.backgroundElevated, c.foreground));

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(UISizes::spacingMD);
    mainLayout->setContentsMargins(UISizes::spacingLG, UISizes::spacingLG,
                                    UISizes::spacingLG, UISizes::spacingLG);

    // Title
    auto* titleLabel = new QLabel(QStringLiteral("Search MusicBrainz"), this);
    titleLabel->setStyleSheet(QStringLiteral(
        "font-size: 18px; font-weight: bold; color: %1;")
        .arg(c.foreground));
    mainLayout->addWidget(titleLabel);

    // Search fields row
    auto* fieldsLayout = new QHBoxLayout();
    fieldsLayout->setSpacing(UISizes::spacingSM);

    auto fieldStyle = tm->inputStyle();

    m_titleEdit = new QLineEdit(this);
    m_titleEdit->setPlaceholderText(QStringLiteral("Title"));
    m_titleEdit->setStyleSheet(fieldStyle);
    fieldsLayout->addWidget(m_titleEdit, 2);

    m_artistEdit = new QLineEdit(this);
    m_artistEdit->setPlaceholderText(QStringLiteral("Artist"));
    m_artistEdit->setStyleSheet(fieldStyle);
    fieldsLayout->addWidget(m_artistEdit, 2);

    m_albumEdit = new QLineEdit(this);
    m_albumEdit->setPlaceholderText(QStringLiteral("Album"));
    m_albumEdit->setStyleSheet(fieldStyle);
    fieldsLayout->addWidget(m_albumEdit, 2);

    m_searchBtn = new QPushButton(QStringLiteral("Search"), this);
    m_searchBtn->setFixedHeight(UISizes::buttonHeight);
    m_searchBtn->setCursor(Qt::PointingHandCursor);
    m_searchBtn->setStyleSheet(tm->buttonStyle(ButtonVariant::Primary));
    fieldsLayout->addWidget(m_searchBtn);

    mainLayout->addLayout(fieldsLayout);

    // Status
    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 12px;").arg(c.foregroundMuted));
    mainLayout->addWidget(m_statusLabel);

    // Results table
    m_resultsTable = new QTableWidget(this);
    m_resultsTable->setColumnCount(5);
    m_resultsTable->setHorizontalHeaderLabels({
        QStringLiteral("Match"), QStringLiteral("Title"),
        QStringLiteral("Artist"), QStringLiteral("Album"),
        QStringLiteral("Year")
    });
    m_resultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultsTable->verticalHeader()->setVisible(false);
    m_resultsTable->horizontalHeader()->setStretchLastSection(true);
    m_resultsTable->setAlternatingRowColors(false);
    m_resultsTable->setShowGrid(false);

    // Column widths
    m_resultsTable->horizontalHeader()->resizeSection(0, 80);
    m_resultsTable->horizontalHeader()->resizeSection(1, 190);
    m_resultsTable->horizontalHeader()->resizeSection(2, 150);
    m_resultsTable->horizontalHeader()->resizeSection(3, 170);
    m_resultsTable->horizontalHeader()->resizeSection(4, 50);

    m_resultsTable->setStyleSheet(QStringLiteral(
        "QTableWidget { background: %1; border: 1px solid %3; border-radius: 4px; }"
        "QTableWidget::item { color: %2; padding: 4px 8px; }"
        "QTableWidget::item:selected { background: %5; }"
        "QHeaderView::section { background: %1; color: %4; border: none; "
        "  border-bottom: 1px solid %3; padding: 6px 8px; font-size: 11px; font-weight: bold; }")
        .arg(c.backgroundSecondary, c.foreground, c.border, c.foregroundMuted, c.selected));

    mainLayout->addWidget(m_resultsTable, 1);

    // Preview panel (hidden initially)
    setupPreviewPanel();
    mainLayout->addWidget(m_previewPanel);

    // Bottom buttons
    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    m_cancelBtn = new QPushButton(QStringLiteral("Cancel"), this);
    m_cancelBtn->setFixedHeight(UISizes::buttonHeight);
    m_cancelBtn->setCursor(Qt::PointingHandCursor);
    m_cancelBtn->setStyleSheet(tm->buttonStyle(ButtonVariant::Secondary));
    buttonLayout->addWidget(m_cancelBtn);

    m_applyBtn = new QPushButton(QStringLiteral("Apply Selected Fields"), this);
    m_applyBtn->setFixedHeight(UISizes::buttonHeight);
    m_applyBtn->setCursor(Qt::PointingHandCursor);
    m_applyBtn->setEnabled(false);
    m_applyBtn->setStyleSheet(tm->buttonStyle(ButtonVariant::Primary));
    buttonLayout->addWidget(m_applyBtn);

    mainLayout->addLayout(buttonLayout);

    // Update preview when a row is selected
    connect(m_resultsTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        int row = m_resultsTable->currentRow();
        if (row >= 0 && row < m_results.size()) {
            updatePreview(row);
            m_previewPanel->setVisible(true);
            m_applyBtn->setEnabled(true);
        } else {
            m_previewPanel->setVisible(false);
            m_applyBtn->setEnabled(false);
        }
    });
}

// ── Preview Panel ──────────────────────────────────────────────────

void MetadataSearchDialog::setupPreviewPanel()
{
    auto c = ThemeManager::instance()->colors();

    m_previewPanel = new QWidget(this);
    m_previewPanel->setVisible(false);
    m_previewPanel->setStyleSheet(QStringLiteral(
        "QWidget#previewPanel { background: %1; border: 1px solid %2; border-radius: 6px; }")
        .arg(c.backgroundSecondary, c.border));
    m_previewPanel->setObjectName(QStringLiteral("previewPanel"));

    auto* previewLayout = new QVBoxLayout(m_previewPanel);
    previewLayout->setSpacing(8);
    previewLayout->setContentsMargins(12, 10, 12, 10);

    // Confidence header
    m_previewConfidence = new QLabel(m_previewPanel);
    m_previewConfidence->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: bold; padding: 0;"));
    previewLayout->addWidget(m_previewConfidence);

    // Separator
    auto* sep = new QFrame(m_previewPanel);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet(QStringLiteral("background: %1; max-height: 1px;").arg(c.border));
    previewLayout->addWidget(sep);

    // Field comparison grid
    auto* grid = new QGridLayout();
    grid->setSpacing(4);
    grid->setColumnMinimumWidth(0, 24);   // checkbox
    grid->setColumnMinimumWidth(1, 60);   // field name
    grid->setColumnMinimumWidth(2, 200);  // current
    grid->setColumnMinimumWidth(3, 20);   // arrow
    grid->setColumnMinimumWidth(4, 200);  // new

    auto headerStyle = QStringLiteral("color: %1; font-size: 11px; font-weight: bold;")
        .arg(c.foregroundMuted);
    auto valStyle = QStringLiteral("color: %1; font-size: 12px;").arg(c.foreground);
    auto mutedStyle = QStringLiteral("color: %1; font-size: 12px;").arg(c.foregroundMuted);

    // Header row
    auto* hField = new QLabel(QStringLiteral("Field"), m_previewPanel);
    hField->setStyleSheet(headerStyle);
    auto* hCur = new QLabel(QStringLiteral("Current"), m_previewPanel);
    hCur->setStyleSheet(headerStyle);
    auto* hNew = new QLabel(QStringLiteral("New"), m_previewPanel);
    hNew->setStyleSheet(headerStyle);
    grid->addWidget(hField, 0, 1);
    grid->addWidget(hCur, 0, 2);
    grid->addWidget(hNew, 0, 4);

    // Title row
    m_chkTitle = new QCheckBox(m_previewPanel);
    m_chkTitle->setChecked(true);
    auto* lblTitle = new QLabel(QStringLiteral("Title"), m_previewPanel);
    lblTitle->setStyleSheet(mutedStyle);
    m_lblCurTitle = new QLabel(m_previewPanel);
    m_lblCurTitle->setStyleSheet(valStyle);
    auto* arrowT = new QLabel(QStringLiteral("\xe2\x86\x92"), m_previewPanel);
    arrowT->setStyleSheet(mutedStyle);
    m_lblNewTitle = new QLabel(m_previewPanel);
    grid->addWidget(m_chkTitle, 1, 0);
    grid->addWidget(lblTitle, 1, 1);
    grid->addWidget(m_lblCurTitle, 1, 2);
    grid->addWidget(arrowT, 1, 3, Qt::AlignCenter);
    grid->addWidget(m_lblNewTitle, 1, 4);

    // Artist row
    m_chkArtist = new QCheckBox(m_previewPanel);
    m_chkArtist->setChecked(true);
    auto* lblArtist = new QLabel(QStringLiteral("Artist"), m_previewPanel);
    lblArtist->setStyleSheet(mutedStyle);
    m_lblCurArtist = new QLabel(m_previewPanel);
    m_lblCurArtist->setStyleSheet(valStyle);
    auto* arrowA = new QLabel(QStringLiteral("\xe2\x86\x92"), m_previewPanel);
    arrowA->setStyleSheet(mutedStyle);
    m_lblNewArtist = new QLabel(m_previewPanel);
    grid->addWidget(m_chkArtist, 2, 0);
    grid->addWidget(lblArtist, 2, 1);
    grid->addWidget(m_lblCurArtist, 2, 2);
    grid->addWidget(arrowA, 2, 3, Qt::AlignCenter);
    grid->addWidget(m_lblNewArtist, 2, 4);

    // Album row
    m_chkAlbum = new QCheckBox(m_previewPanel);
    m_chkAlbum->setChecked(true);
    auto* lblAlbum = new QLabel(QStringLiteral("Album"), m_previewPanel);
    lblAlbum->setStyleSheet(mutedStyle);
    m_lblCurAlbum = new QLabel(m_previewPanel);
    m_lblCurAlbum->setStyleSheet(valStyle);
    auto* arrowAl = new QLabel(QStringLiteral("\xe2\x86\x92"), m_previewPanel);
    arrowAl->setStyleSheet(mutedStyle);
    m_lblNewAlbum = new QLabel(m_previewPanel);
    grid->addWidget(m_chkAlbum, 3, 0);
    grid->addWidget(lblAlbum, 3, 1);
    grid->addWidget(m_lblCurAlbum, 3, 2);
    grid->addWidget(arrowAl, 3, 3, Qt::AlignCenter);
    grid->addWidget(m_lblNewAlbum, 3, 4);

    // Year row
    m_chkYear = new QCheckBox(m_previewPanel);
    m_chkYear->setChecked(true);
    auto* lblYear = new QLabel(QStringLiteral("Year"), m_previewPanel);
    lblYear->setStyleSheet(mutedStyle);
    m_lblCurYear = new QLabel(m_previewPanel);
    m_lblCurYear->setStyleSheet(valStyle);
    auto* arrowY = new QLabel(QStringLiteral("\xe2\x86\x92"), m_previewPanel);
    arrowY->setStyleSheet(mutedStyle);
    m_lblNewYear = new QLabel(m_previewPanel);
    grid->addWidget(m_chkYear, 4, 0);
    grid->addWidget(lblYear, 4, 1);
    grid->addWidget(m_lblCurYear, 4, 2);
    grid->addWidget(arrowY, 4, 3, Qt::AlignCenter);
    grid->addWidget(m_lblNewYear, 4, 4);

    previewLayout->addLayout(grid);
}

void MetadataSearchDialog::updatePreview(int row)
{
    if (row < 0 || row >= m_results.size()) return;
    const auto& r = m_results[row];
    auto c = ThemeManager::instance()->colors();

    // Confidence
    QString color = confidenceColor(r.score);
    QString text = confidenceText(r.score);
    m_previewConfidence->setText(
        QStringLiteral("%1% \xe2\x80\x94 %2").arg(qRound(r.score)).arg(text));
    m_previewConfidence->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; font-weight: bold; padding: 0;").arg(color));

    auto sameStyle = QStringLiteral("color: %1; font-size: 12px;").arg(c.foreground);
    auto diffStyle = QStringLiteral("color: #FF9800; font-size: 12px; font-weight: bold;");

    // Title
    m_lblCurTitle->setText(m_track.title);
    m_lblNewTitle->setText(r.title.isEmpty() ? m_track.title : r.title);
    bool titleSame = r.title.isEmpty() || r.title == m_track.title;
    m_lblNewTitle->setStyleSheet(titleSame ? sameStyle : diffStyle);
    m_chkTitle->setChecked(!titleSame && !r.title.isEmpty());

    // Artist
    m_lblCurArtist->setText(m_track.artist);
    m_lblNewArtist->setText(r.artist.isEmpty() ? m_track.artist : r.artist);
    bool artistSame = r.artist.isEmpty() || r.artist == m_track.artist;
    m_lblNewArtist->setStyleSheet(artistSame ? sameStyle : diffStyle);
    m_chkArtist->setChecked(!artistSame && !r.artist.isEmpty());

    // Album
    m_lblCurAlbum->setText(m_track.album);
    m_lblNewAlbum->setText(r.album.isEmpty() ? m_track.album : r.album);
    bool albumSame = r.album.isEmpty() || r.album == m_track.album;
    m_lblNewAlbum->setStyleSheet(albumSame ? sameStyle : diffStyle);
    m_chkAlbum->setChecked(!albumSame && !r.album.isEmpty());

    // Year (Track struct has no year field, show from result only)
    m_lblCurYear->setText(QStringLiteral("-"));
    QString newYear = r.year > 0 ? QString::number(r.year) : QStringLiteral("-");
    m_lblNewYear->setText(newYear);
    m_lblNewYear->setStyleSheet(r.year > 0 ? diffStyle : sameStyle);
    m_chkYear->setChecked(false);
    m_chkYear->setEnabled(false);

    // Low confidence warning
    if (r.score < 40.0) {
        m_statusLabel->setText(QStringLiteral("Warning: Low confidence match. Review carefully before applying."));
        m_statusLabel->setStyleSheet(QStringLiteral("color: #F44336; font-size: 12px; font-weight: bold;"));
    } else {
        m_statusLabel->setText(QStringLiteral("%1 results found").arg(m_results.size()));
        m_statusLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
            .arg(ThemeManager::instance()->colors().foregroundMuted));
    }
}

// ── Search ─────────────────────────────────────────────────────────

void MetadataSearchDialog::onSearch()
{
    m_searchBtn->setEnabled(false);
    m_statusLabel->setText(QStringLiteral("Searching MusicBrainz..."));
    m_statusLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
        .arg(ThemeManager::instance()->colors().foregroundMuted));
    m_resultsTable->setRowCount(0);
    m_applyBtn->setEnabled(false);
    m_previewPanel->setVisible(false);

    auto* mb = MusicBrainzProvider::instance();

    connect(mb, &MusicBrainzProvider::multipleTracksFound,
            this, &MetadataSearchDialog::onResultsReceived,
            Qt::SingleShotConnection);

    connect(mb, &MusicBrainzProvider::noResultsFound,
            this, [this]() {
        m_statusLabel->setText(QStringLiteral("No results found."));
        m_searchBtn->setEnabled(true);
    }, Qt::SingleShotConnection);

    connect(mb, &MusicBrainzProvider::searchError,
            this, [this](const QString& error) {
        m_statusLabel->setText(QStringLiteral("Error: %1").arg(error));
        m_searchBtn->setEnabled(true);
    }, Qt::SingleShotConnection);

    mb->searchTrackMultiple(
        m_titleEdit->text().trimmed(),
        m_artistEdit->text().trimmed(),
        m_albumEdit->text().trimmed());
}

void MetadataSearchDialog::onResultsReceived(const QVector<MusicBrainzResult>& results)
{
    m_results = results;
    m_searchBtn->setEnabled(true);
    m_statusLabel->setText(QStringLiteral("%1 results found").arg(results.size()));

    m_resultsTable->setRowCount(results.size());
    for (int i = 0; i < results.size(); ++i) {
        const auto& r = results[i];

        // Color-coded confidence cell
        QString color = confidenceColor(r.score);
        auto* scoreItem = new QTableWidgetItem(
            QStringLiteral("%1%").arg(qRound(r.score)));
        scoreItem->setTextAlignment(Qt::AlignCenter);
        scoreItem->setForeground(QColor(color));
        m_resultsTable->setItem(i, 0, scoreItem);

        m_resultsTable->setItem(i, 1, new QTableWidgetItem(r.title));
        m_resultsTable->setItem(i, 2, new QTableWidgetItem(r.artist));
        m_resultsTable->setItem(i, 3, new QTableWidgetItem(r.album));
        m_resultsTable->setItem(i, 4, new QTableWidgetItem(
            r.year > 0 ? QString::number(r.year) : QString()));
    }
}

// ── Apply ──────────────────────────────────────────────────────────

void MetadataSearchDialog::onApply()
{
    int row = m_resultsTable->currentRow();
    if (row < 0 || row >= m_results.size()) return;

    const auto& r = m_results[row];

    // Build result with only checked fields
    MusicBrainzResult filtered;
    // Always pass through MBIDs (they're internal, not user-visible)
    filtered.mbid = r.mbid;
    filtered.artistMbid = r.artistMbid;
    filtered.albumMbid = r.albumMbid;
    filtered.releaseGroupMbid = r.releaseGroupMbid;
    filtered.trackNumber = r.trackNumber;
    filtered.discNumber = r.discNumber;
    filtered.score = r.score;

    if (m_chkTitle->isChecked())  filtered.title  = r.title;
    if (m_chkArtist->isChecked()) filtered.artist = r.artist;
    if (m_chkAlbum->isChecked())  filtered.album  = r.album;
    // Year checkbox is disabled (Track has no year field)

    // Count how many visible fields are changing
    int changing = 0;
    if (m_chkTitle->isChecked() && !r.title.isEmpty() && r.title != m_track.title) ++changing;
    if (m_chkArtist->isChecked() && !r.artist.isEmpty() && r.artist != m_track.artist) ++changing;
    if (m_chkAlbum->isChecked() && !r.album.isEmpty() && r.album != m_track.album) ++changing;

    if (changing == 0) {
        m_statusLabel->setText(QStringLiteral("No fields selected for update."));
        m_statusLabel->setStyleSheet(QStringLiteral("color: #FF9800; font-size: 12px;"));
        return;
    }

    m_selectedResult = filtered;
    qDebug() << "[MetadataSearch] Applying" << changing << "field(s) with score:" << r.score;
    accept();
}
