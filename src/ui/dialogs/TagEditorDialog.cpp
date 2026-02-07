#include "TagEditorDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QFileDialog>
#include "StyledMessageBox.h"
#include <QClipboard>
#include <QApplication>
#include <QMimeData>
#include <QPixmap>
#include <QDebug>

#include "../../core/ThemeManager.h"

// ═══════════════════════════════════════════════════════════════════════
//  Constructors
// ═══════════════════════════════════════════════════════════════════════

TagEditorDialog::TagEditorDialog(const QString& filePath, QWidget* parent)
    : QDialog(parent)
    , m_filePaths({filePath})
    , m_batchMode(false)
{
    setupUi();
    loadTags();
}

TagEditorDialog::TagEditorDialog(const QStringList& filePaths, QWidget* parent)
    : QDialog(parent)
    , m_filePaths(filePaths)
    , m_batchMode(filePaths.size() > 1)
{
    setupUi();
    loadTags();
}

// ═══════════════════════════════════════════════════════════════════════
//  setupUi
// ═══════════════════════════════════════════════════════════════════════

void TagEditorDialog::setupUi()
{
    setWindowTitle(m_batchMode
        ? tr("Edit Tags (%1 files)").arg(m_filePaths.size())
        : tr("Edit Tags"));
    setMinimumSize(520, 600);
    resize(560, 680);

    auto* tm = ThemeManager::instance();
    auto c = tm->colors();
    setStyleSheet(QStringLiteral("QDialog { background-color: %1; }"
                          "QLabel { color: %2; }"
                          "QLineEdit, QSpinBox { background-color: %3; color: %2;"
                          " border: 1px solid %4; border-radius: 4px; padding: 6px; }"
                          "QGroupBox { color: %2; border: 1px solid %4;"
                          " border-radius: 6px; margin-top: 12px; padding-top: 16px; }"
                          "QGroupBox::title { subcontrol-origin: margin;"
                          " left: 12px; padding: 0 4px; }")
        .arg(c.backgroundElevated, c.foreground, c.backgroundTertiary, c.border));

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(UISizes::spacingMD);
    mainLayout->setContentsMargins(20, 20,
                                    20, 20);

    // ── Album Art section ──────────────────────────────────────────
    auto* artGroup = new QGroupBox(tr("Album Art"), this);
    auto* artLayout = new QHBoxLayout(artGroup);

    m_artLabel = new QLabel(this);
    m_artLabel->setFixedSize(120, 120);
    m_artLabel->setAlignment(Qt::AlignCenter);
    m_artLabel->setStyleSheet(QStringLiteral(
        "QLabel { background-color: %1; border: 1px solid %2; border-radius: 4px; }")
        .arg(c.backgroundTertiary, c.borderSubtle));
    artLayout->addWidget(m_artLabel);

    auto* artBtnLayout = new QVBoxLayout();
    m_changeArtBtn = new QPushButton(tr("Change Art..."), this);
    m_removeArtBtn = new QPushButton(tr("Remove Art"), this);

    QString btnStyle = tm->buttonStyle(ButtonVariant::Secondary);
    m_changeArtBtn->setStyleSheet(btnStyle);
    m_removeArtBtn->setStyleSheet(btnStyle);

    artBtnLayout->addWidget(m_changeArtBtn);
    artBtnLayout->addWidget(m_removeArtBtn);
    artBtnLayout->addStretch();
    artLayout->addLayout(artBtnLayout);
    artLayout->addStretch();

    mainLayout->addWidget(artGroup);

    connect(m_changeArtBtn, &QPushButton::clicked, this, &TagEditorDialog::onChangeArt);
    connect(m_removeArtBtn, &QPushButton::clicked, this, &TagEditorDialog::onRemoveArt);

    // ── Metadata fields ───────────────────────────────────────────
    auto* fieldsGroup = new QGroupBox(tr("Metadata"), this);
    auto* formLayout = new QFormLayout(fieldsGroup);
    formLayout->setSpacing(UISizes::spacingSM);

    m_titleEdit = new QLineEdit(this);
    m_artistEdit = new QLineEdit(this);
    m_albumEdit = new QLineEdit(this);
    m_albumArtistEdit = new QLineEdit(this);
    m_genreEdit = new QLineEdit(this);
    m_composerEdit = new QLineEdit(this);
    m_commentEdit = new QLineEdit(this);

    m_trackSpin = new QSpinBox(this);
    m_trackSpin->setRange(0, 999);
    m_discSpin = new QSpinBox(this);
    m_discSpin->setRange(0, 99);
    m_yearSpin = new QSpinBox(this);
    m_yearSpin->setRange(0, 9999);

    formLayout->addRow(tr("Title:"), m_titleEdit);
    formLayout->addRow(tr("Artist:"), m_artistEdit);
    formLayout->addRow(tr("Album:"), m_albumEdit);
    formLayout->addRow(tr("Album Artist:"), m_albumArtistEdit);

    auto* numRow = new QHBoxLayout();
    numRow->addWidget(new QLabel(tr("Track:"), this));
    numRow->addWidget(m_trackSpin);
    numRow->addSpacing(UISizes::spacingLG);
    numRow->addWidget(new QLabel(tr("Disc:"), this));
    numRow->addWidget(m_discSpin);
    numRow->addSpacing(UISizes::spacingLG);
    numRow->addWidget(new QLabel(tr("Year:"), this));
    numRow->addWidget(m_yearSpin);
    numRow->addStretch();
    formLayout->addRow(numRow);

    formLayout->addRow(tr("Genre:"), m_genreEdit);
    formLayout->addRow(tr("Composer:"), m_composerEdit);
    formLayout->addRow(tr("Comment:"), m_commentEdit);

    // In batch mode, show placeholder text
    if (m_batchMode) {
        const QString hint = tr("(leave empty to keep existing)");
        m_titleEdit->setPlaceholderText(hint);
        m_artistEdit->setPlaceholderText(hint);
        m_albumEdit->setPlaceholderText(hint);
        m_albumArtistEdit->setPlaceholderText(hint);
        m_genreEdit->setPlaceholderText(hint);
        m_composerEdit->setPlaceholderText(hint);
        m_commentEdit->setPlaceholderText(hint);
    }

    mainLayout->addWidget(fieldsGroup);

    // ── Buttons ───────────────────────────────────────────────────
    auto* buttonLayout = new QHBoxLayout();

    auto* undoBtn = new QPushButton(tr("Undo"), this);
    undoBtn->setStyleSheet(btnStyle);
    connect(undoBtn, &QPushButton::clicked, this, &TagEditorDialog::onUndo);
    buttonLayout->addWidget(undoBtn);

    buttonLayout->addStretch();

    auto* cancelBtn = new QPushButton(tr("Cancel"), this);
    cancelBtn->setStyleSheet(btnStyle);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    buttonLayout->addWidget(cancelBtn);

    auto* saveBtn = new QPushButton(tr("Save"), this);
    saveBtn->setStyleSheet(tm->buttonStyle(ButtonVariant::Primary));
    connect(saveBtn, &QPushButton::clicked, this, &TagEditorDialog::onSave);
    buttonLayout->addWidget(saveBtn);

    mainLayout->addLayout(buttonLayout);
}

// ═══════════════════════════════════════════════════════════════════════
//  loadTags
// ═══════════════════════════════════════════════════════════════════════

void TagEditorDialog::loadTags()
{
    m_originalMeta.clear();

    for (const QString& path : m_filePaths) {
        TrackMetadata meta;
        TagWriter::readTags(path, meta);
        m_originalMeta.append(meta);
    }

    if (m_batchMode) {
        // In batch mode, leave fields empty — user fills in what to change
        updateArtDisplay();
        return;
    }

    // Single file mode — populate fields
    if (!m_originalMeta.isEmpty()) {
        const auto& meta = m_originalMeta.first();
        m_titleEdit->setText(meta.title);
        m_artistEdit->setText(meta.artist);
        m_albumEdit->setText(meta.album);
        m_albumArtistEdit->setText(meta.albumArtist);
        m_trackSpin->setValue(meta.trackNumber);
        m_discSpin->setValue(meta.discNumber);
        m_yearSpin->setValue(meta.year);
        m_genreEdit->setText(meta.genre);
        m_composerEdit->setText(meta.composer);
        m_commentEdit->setText(meta.comment);
        m_albumArt = meta.albumArt;
        updateArtDisplay();
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  saveTags
// ═══════════════════════════════════════════════════════════════════════

void TagEditorDialog::saveTags()
{
    int successCount = 0;

    for (int i = 0; i < m_filePaths.size(); ++i) {
        const QString& path = m_filePaths[i];

        TrackMetadata meta;
        TagWriter::readTags(path, meta);

        // Apply field values (in batch mode, skip empty fields)
        auto applyField = [&](const QString& editText, QString& field) {
            if (!m_batchMode || !editText.isEmpty())
                field = editText;
        };

        applyField(m_titleEdit->text(), meta.title);
        applyField(m_artistEdit->text(), meta.artist);
        applyField(m_albumEdit->text(), meta.album);
        applyField(m_albumArtistEdit->text(), meta.albumArtist);
        applyField(m_genreEdit->text(), meta.genre);
        applyField(m_composerEdit->text(), meta.composer);
        applyField(m_commentEdit->text(), meta.comment);

        if (!m_batchMode || m_trackSpin->value() > 0)
            meta.trackNumber = m_trackSpin->value();
        if (!m_batchMode || m_discSpin->value() > 0)
            meta.discNumber = m_discSpin->value();
        if (!m_batchMode || m_yearSpin->value() > 0)
            meta.year = m_yearSpin->value();

        if (TagWriter::writeTags(path, meta))
            successCount++;
        else
            qWarning() << "TagEditor: Failed to write tags for" << path;

        // Handle album art changes
        if (m_artRemoved) {
            TagWriter::writeAlbumArt(path, QImage()); // empty = remove
        } else if (m_artChanged && !m_albumArt.isNull()) {
            TagWriter::writeAlbumArt(path, m_albumArt);
        }
    }

    qDebug() << "TagEditor: Saved tags for" << successCount << "of" << m_filePaths.size() << "files";
}

// ═══════════════════════════════════════════════════════════════════════
//  Slots
// ═══════════════════════════════════════════════════════════════════════

void TagEditorDialog::onSave()
{
    saveTags();
    emit tagsUpdated();
    accept();
}

void TagEditorDialog::onChangeArt()
{
    // Check clipboard first
    const QClipboard* clipboard = QApplication::clipboard();
    const QMimeData* mimeData = clipboard->mimeData();
    if (mimeData && mimeData->hasImage()) {
        QImage clipImg = qvariant_cast<QImage>(mimeData->imageData());
        if (!clipImg.isNull()) {
            // Ask user: from file or clipboard?
            StyledMessageBox box(this);
            box.setIcon(StyledMessageBox::Icon::Question);
            box.setTitle(tr("Album Art"));
            box.setMessage(tr("An image is available on the clipboard.\nUse clipboard image?"));
            box.addButton(StyledMessageBox::ButtonType::Cancel, false);
            box.addButton(StyledMessageBox::ButtonType::No, false);
            box.addButton(StyledMessageBox::ButtonType::Yes, true);
            box.exec();
            if (box.clickedButton() == StyledMessageBox::ButtonType::Yes) {
                m_albumArt = clipImg;
                m_artChanged = true;
                m_artRemoved = false;
                updateArtDisplay();
                return;
            }
            if (box.clickedButton() == StyledMessageBox::ButtonType::Cancel)
                return;
        }
    }

    // Open file dialog
    QString filePath = QFileDialog::getOpenFileName(this,
        tr("Select Album Art"),
        QString(),
        tr("Images (*.png *.jpg *.jpeg *.bmp *.webp);;All Files (*)"));

    if (filePath.isEmpty())
        return;

    QImage img(filePath);
    if (img.isNull()) {
        StyledMessageBox::error(this, tr("Error"),
            tr("Could not load image file."));
        return;
    }

    m_albumArt = img;
    m_artChanged = true;
    m_artRemoved = false;
    updateArtDisplay();
}

void TagEditorDialog::onRemoveArt()
{
    m_albumArt = QImage();
    m_artChanged = false;
    m_artRemoved = true;
    updateArtDisplay();
}

void TagEditorDialog::onUndo()
{
    if (m_originalMeta.isEmpty())
        return;

    if (m_batchMode) {
        // Clear all fields
        m_titleEdit->clear();
        m_artistEdit->clear();
        m_albumEdit->clear();
        m_albumArtistEdit->clear();
        m_trackSpin->setValue(0);
        m_discSpin->setValue(0);
        m_yearSpin->setValue(0);
        m_genreEdit->clear();
        m_composerEdit->clear();
        m_commentEdit->clear();
        m_albumArt = QImage();
        m_artChanged = false;
        m_artRemoved = false;
    } else {
        // Restore original values
        const auto& meta = m_originalMeta.first();
        m_titleEdit->setText(meta.title);
        m_artistEdit->setText(meta.artist);
        m_albumEdit->setText(meta.album);
        m_albumArtistEdit->setText(meta.albumArtist);
        m_trackSpin->setValue(meta.trackNumber);
        m_discSpin->setValue(meta.discNumber);
        m_yearSpin->setValue(meta.year);
        m_genreEdit->setText(meta.genre);
        m_composerEdit->setText(meta.composer);
        m_commentEdit->setText(meta.comment);
        m_albumArt = meta.albumArt;
        m_artChanged = false;
        m_artRemoved = false;
    }
    updateArtDisplay();
}

// ═══════════════════════════════════════════════════════════════════════
//  updateArtDisplay
// ═══════════════════════════════════════════════════════════════════════

void TagEditorDialog::updateArtDisplay()
{
    if (m_albumArt.isNull()) {
        m_artLabel->setText(tr("No Art"));
        m_artLabel->setPixmap(QPixmap());
    } else {
        QPixmap pix = QPixmap::fromImage(m_albumArt).scaled(
            120, 120,
            Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_artLabel->setPixmap(pix);
        m_artLabel->setText(QString());
    }
}
