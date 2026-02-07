#include "NewPlaylistDialog.h"
#include "../../core/ThemeManager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>

NewPlaylistDialog::NewPlaylistDialog(QWidget* parent)
    : QDialog(parent)
    , m_nameEdit(nullptr)
    , m_cancelButton(nullptr)
    , m_okButton(nullptr)
    , m_titleLabel(nullptr)
    , m_nameLabel(nullptr)
{
    setWindowTitle(QStringLiteral("New Playlist"));
    setFixedWidth(360);
    setModal(true);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    setupUI();
    applyTheme();

    connect(ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &NewPlaylistDialog::applyTheme);
}

void NewPlaylistDialog::setupUI()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(UISizes::spacingXL, UISizes::spacingXL,
                                    UISizes::spacingXL, UISizes::spacingXL);
    mainLayout->setSpacing(UISizes::spacingLG);

    // Title
    m_titleLabel = new QLabel(QStringLiteral("New Playlist"), this);
    m_titleLabel->setObjectName(QStringLiteral("dialogTitle"));
    QFont titleFont = m_titleLabel->font();
    titleFont.setPointSize(16);
    titleFont.setWeight(QFont::DemiBold);
    m_titleLabel->setFont(titleFont);
    mainLayout->addWidget(m_titleLabel);

    // Name label
    m_nameLabel = new QLabel(QStringLiteral("Playlist name:"), this);
    m_nameLabel->setObjectName(QStringLiteral("dialogLabel"));
    mainLayout->addWidget(m_nameLabel);

    // Name input
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setObjectName(QStringLiteral("dialogInput"));
    m_nameEdit->setPlaceholderText(QStringLiteral("Enter playlist name"));
    m_nameEdit->setMinimumHeight(UISizes::thumbnailSize);
    mainLayout->addWidget(m_nameEdit);

    // Buttons
    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(UISizes::spacingMD);

    m_cancelButton = new QPushButton(QStringLiteral("Cancel"), this);
    m_cancelButton->setObjectName(QStringLiteral("dialogCancelButton"));
    m_cancelButton->setMinimumHeight(UISizes::buttonHeight);
    m_cancelButton->setCursor(Qt::PointingHandCursor);

    m_okButton = new QPushButton(QStringLiteral("OK"), this);
    m_okButton->setObjectName(QStringLiteral("dialogOkButton"));
    m_okButton->setMinimumHeight(UISizes::buttonHeight);
    m_okButton->setDefault(true);
    m_okButton->setEnabled(false);
    m_okButton->setCursor(Qt::PointingHandCursor);

    buttonLayout->addWidget(m_cancelButton);
    buttonLayout->addWidget(m_okButton);
    mainLayout->addLayout(buttonLayout);

    // Connections
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_okButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_nameEdit, &QLineEdit::textChanged, this, &NewPlaylistDialog::onTextChanged);
    connect(m_nameEdit, &QLineEdit::returnPressed, this, &QDialog::accept);
}

void NewPlaylistDialog::applyTheme()
{
    auto c = ThemeManager::instance()->colors();

    // Dialog background
    setStyleSheet(QStringLiteral(
        "NewPlaylistDialog {"
        "  background-color: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 12px;"
        "}")
        .arg(c.backgroundElevated, c.border));

    // Title
    m_titleLabel->setStyleSheet(QStringLiteral(
        "color: %1; background: transparent;")
        .arg(c.foreground));

    // Label
    m_nameLabel->setStyleSheet(QStringLiteral(
        "color: %1; background: transparent;")
        .arg(c.foregroundSecondary));

    // Input
    m_nameEdit->setStyleSheet(ThemeManager::instance()->inputStyle());

    // Cancel button
    m_cancelButton->setStyleSheet(ThemeManager::instance()->buttonStyle(ButtonVariant::Secondary));

    // OK button
    m_okButton->setStyleSheet(ThemeManager::instance()->buttonStyle(ButtonVariant::Primary));
}

QString NewPlaylistDialog::playlistName() const
{
    return m_nameEdit->text().trimmed();
}

void NewPlaylistDialog::onTextChanged(const QString& text)
{
    m_okButton->setEnabled(!text.trimmed().isEmpty());
}
