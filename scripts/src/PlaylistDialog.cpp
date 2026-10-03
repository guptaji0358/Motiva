#include "PlaylistDialog.h"
#include "ImagePlaylist.h"
#include "PlaylistRotation.h"
#include "Theme.h"
#include "VideoPlayer.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr const char* kRowMimeType = "application/x-motiva-playlist-row";
constexpr int kCardWidth = 184;
constexpr int kCardHeight = 158;
constexpr int kCardSpacing = 12;

// Same light/dark icon convention as the Settings gear - see Theme::iconVariant().
QString playlistIconPath() {
    return QStringLiteral(":/playlist/%1/playlist.svg").arg(Theme::iconVariant(Theme::currentTheme()));
}

// The app ships no translation files, so tr("%n item(s)") would show the
// literal "(s)" - spell out the singular/plural forms instead.
QString countText(int n, const QString& one, const QString& many) {
    return (n == 1 ? one : many).arg(n);
}

bool isOnyx(Theme::AppTheme theme) {
    return theme == Theme::AppTheme::DarkOnyx || theme == Theme::AppTheme::LightOnyx;
}

// Corner language follows the active theme family: Aurora is soft, Onyx
// is near-square (matches the radii baked into Theme.cpp's stylesheets).
int cardRadius() {
    return isOnyx(Theme::currentTheme()) ? 2 : 10;
}

QPainterPath roundedPath(const QRectF& rect, qreal radius) {
    QPainterPath path;
    path.addRoundedRect(rect, radius, radius);
    return path;
}

// One playlist entry as a card: thumbnail (cropped to fill, on the same
// fixed-dark media surface the main preview uses), order number, a
// "Now showing" marker on the current image, and an explicit
// "File not found" state for missing files. Colors come from the active
// Motiva Theme at paint time, so a live theme switch restyles it.
class PlaylistCardDelegate : public QStyledItemDelegate {
public:
    explicit PlaylistCardDelegate(ImagePlaylist* playlist, QObject* parent)
        : QStyledItemDelegate(parent), m_playlist(playlist) {}

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(kCardWidth, kCardHeight);
    }

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const Theme::ThemePalette& pal = Theme::themePalette(Theme::currentTheme());
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const bool available = index.data(ImagePlaylist::AvailableRole).toBool();
        const bool isCurrent = index.data(ImagePlaylist::IsCurrentRole).toBool();
        const bool playlistOn = m_playlist->isEnabled();
        const int radius = cardRadius();

        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setRenderHint(QPainter::SmoothPixmapTransform, true);

        const QRectF card = QRectF(option.rect).adjusted(3, 3, -3, -3);
        p->setPen(Qt::NoPen);
        p->setBrush(pal.panelBg);
        p->drawPath(roundedPath(card, radius));

        // Thumbnail area
        const QRectF thumb = card.adjusted(8, 8, -8, -36);
        const QPainterPath thumbPath = roundedPath(thumb, qMax(1, radius - 3));
        p->fillPath(thumbPath, QColor(Theme::kPreviewSurfaceBg));
        if (available) {
            const QImage image = index.data(ImagePlaylist::ThumbnailRole).value<QImage>();
            if (!image.isNull()) {
                p->save();
                p->setClipPath(thumbPath);
                const QSizeF scaled = QSizeF(image.size()).scaled(thumb.size(), Qt::KeepAspectRatioByExpanding);
                const QRectF target(thumb.center().x() - scaled.width() / 2, thumb.center().y() - scaled.height() / 2,
                                    scaled.width(), scaled.height());
                p->drawImage(target, image);
                p->restore();
            } else {
                p->setPen(QColor(Theme::kPreviewText));
                p->drawText(thumb, Qt::AlignCenter, PlaylistDialog::tr("Loading…"));
            }
        } else {
            QPen dashed(QColor(Theme::kStatusWarning), 1.2, Qt::DashLine);
            p->setPen(dashed);
            p->setBrush(Qt::NoBrush);
            p->drawPath(roundedPath(thumb.adjusted(1, 1, -1, -1), qMax(1, radius - 3)));
            QFont f = option.font;
            f.setBold(true);
            p->setFont(f);
            p->setPen(QColor(Theme::kStatusWarning));
            p->drawText(thumb, Qt::AlignCenter, PlaylistDialog::tr("File not found"));
        }

        // Order badge (top-left)
        QFont badgeFont = option.font;
        badgeFont.setBold(true);
        badgeFont.setPointSizeF(qMax(7.0, badgeFont.pointSizeF() - 1));
        p->setFont(badgeFont);
        const QString number = QStringLiteral("%1").arg(index.row() + 1, 2, 10, QLatin1Char('0'));
        const QRectF numberRect(thumb.left() + 6, thumb.top() + 6,
                                p->fontMetrics().horizontalAdvance(number) + 12, p->fontMetrics().height() + 4);
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(0, 0, 0, 165));
        p->drawRoundedRect(numberRect, qMin(4, radius), qMin(4, radius));
        p->setPen(Qt::white);
        p->drawText(numberRect, Qt::AlignCenter, number);

        // Current marker (top-right)
        if (isCurrent) {
            const QString label = playlistOn ? PlaylistDialog::tr("Now showing") : PlaylistDialog::tr("Resumes here");
            const QRectF currentRect(thumb.right() - 6 - (p->fontMetrics().horizontalAdvance(label) + 14),
                                     thumb.top() + 6, p->fontMetrics().horizontalAdvance(label) + 14,
                                     p->fontMetrics().height() + 4);
            p->setPen(Qt::NoPen);
            p->setBrush(playlistOn ? pal.accent : QColor(0, 0, 0, 165));
            p->drawRoundedRect(currentRect, qMin(4, radius), qMin(4, radius));
            p->setPen(Qt::white);
            p->drawText(currentRect, Qt::AlignCenter, label);
        }

        // File name
        p->setFont(option.font);
        const QRectF nameRect(card.left() + 10, thumb.bottom() + 6, card.width() - 20, 22);
        p->setPen(available ? pal.textPrimary : pal.textDisabled);
        p->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                    p->fontMetrics().elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle,
                                                int(nameRect.width())));

        // Card outline: selection > current > hover > resting
        QColor outline = pal.border;
        qreal width = 1.0;
        if (selected) {
            outline = pal.accent;
            width = 2.0;
        } else if (isCurrent && playlistOn) {
            outline = pal.accent;
            width = 1.5;
        } else if (hovered) {
            outline = pal.borderStrong;
        }
        p->setPen(QPen(outline, width));
        p->setBrush(Qt::NoBrush);
        p->drawPath(roundedPath(card.adjusted(width / 2, width / 2, -width / 2, -width / 2), radius));
        p->restore();
    }

private:
    ImagePlaylist* m_playlist;
};

} // namespace

// ---------------------------------------------------------------- PlaylistView

PlaylistView::PlaylistView(ImagePlaylist* playlist, QWidget* parent)
    : QListView(parent), m_playlist(playlist) {
    setModel(playlist);
    setItemDelegate(new PlaylistCardDelegate(playlist, this));
    setViewMode(QListView::IconMode);
    setFlow(QListView::LeftToRight);
    setWrapping(true);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setUniformItemSizes(true);
    setSpacing(kCardSpacing / 2);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragEnabled(true);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDropIndicatorShown(false); // drawn ourselves, between cards
    setMouseTracking(true);
    setFrameShape(QFrame::NoFrame);
    viewport()->setAutoFillBackground(false); // background painted in paintEvent (theme-aware)
    setAccessibleName(tr("Playlist images"));
    setAccessibleDescription(tr("Ordered list of playlist images. Ctrl+Left and Ctrl+Right move the selected "
                                "image, Enter shows it now, Delete removes it from the playlist."));
}

QStringList PlaylistView::supportedLocalFiles(const QMimeData* mime) {
    QStringList files;
    if (!mime || !mime->hasUrls()) {
        return files;
    }
    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile()) {
            continue; // browser thumbnails / web URLs are never treated as images
        }
        const QString path = url.toLocalFile();
        if (QFileInfo(path).isFile() && ImagePlaylist::isSupportedImage(path)) {
            files << path;
        }
    }
    return files;
}

int PlaylistView::insertPositionAt(const QPoint& pos) const {
    const int n = model()->rowCount();
    for (int row = 0; row < n; ++row) {
        const QRect r = visualRect(model()->index(row, 0));
        if (pos.y() < r.top() - spacing()) {
            return row; // above this card's line
        }
        if (pos.y() <= r.bottom() + spacing() && pos.x() < r.center().x()) {
            return row;
        }
    }
    return n;
}

void PlaylistView::startDrag(Qt::DropActions) {
    const QModelIndex idx = currentIndex();
    if (!idx.isValid()) {
        return;
    }
    auto* mime = new QMimeData();
    mime->setData(kRowMimeType, QByteArray::number(idx.row()));
    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    const QImage thumb = idx.data(ImagePlaylist::ThumbnailRole).value<QImage>();
    if (!thumb.isNull()) {
        const QPixmap pm = QPixmap::fromImage(thumb.scaled(128, 84, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        drag->setPixmap(pm);
        drag->setHotSpot(QPoint(pm.width() / 2, pm.height() / 2));
    }
    drag->exec(Qt::MoveAction);
    drag->deleteLater();
    m_dropIndicatorPos = -1;
    viewport()->update();
}

void PlaylistView::dragEnterEvent(QDragEnterEvent* event) {
    const QMimeData* mime = event->mimeData();
    if (mime->hasFormat(kRowMimeType) && event->source() == this) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else if (!supportedLocalFiles(mime).isEmpty()) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else {
        event->ignore();
    }
}

void PlaylistView::dragMoveEvent(QDragMoveEvent* event) {
    const QMimeData* mime = event->mimeData();
    const bool internal = mime->hasFormat(kRowMimeType) && event->source() == this;
    if (!internal && supportedLocalFiles(mime).isEmpty()) {
        event->ignore();
        return;
    }
    const int pos = insertPositionAt(event->position().toPoint());
    if (pos != m_dropIndicatorPos) {
        m_dropIndicatorPos = pos;
        viewport()->update();
    }
    event->setDropAction(internal ? Qt::MoveAction : Qt::CopyAction);
    event->accept();
}

void PlaylistView::dragLeaveEvent(QDragLeaveEvent* event) {
    m_dropIndicatorPos = -1;
    viewport()->update();
    QListView::dragLeaveEvent(event);
}

void PlaylistView::dropEvent(QDropEvent* event) {
    const QMimeData* mime = event->mimeData();
    const int pos = insertPositionAt(event->position().toPoint());
    m_dropIndicatorPos = -1;
    viewport()->update();
    if (mime->hasFormat(kRowMimeType) && event->source() == this) {
        const int from = mime->data(kRowMimeType).toInt();
        const int to = (pos > from) ? pos - 1 : pos;
        event->setDropAction(Qt::MoveAction);
        event->accept();
        if (to != from) {
            emit moveRequested(from, to);
        }
        return;
    }
    const QStringList files = supportedLocalFiles(mime);
    if (files.isEmpty()) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
    emit filesDropped(files, pos);
}

void PlaylistView::keyPressEvent(QKeyEvent* event) {
    const QModelIndexList selected = selectionModel()->selectedIndexes();
    const int row = selected.isEmpty() ? -1 : selected.first().row();
    if (row >= 0 && (event->modifiers() & Qt::ControlModifier)) {
        if (event->key() == Qt::Key_Left && row > 0) {
            emit moveRequested(row, row - 1);
            return;
        }
        if (event->key() == Qt::Key_Right && row < model()->rowCount() - 1) {
            emit moveRequested(row, row + 1);
            return;
        }
    }
    if (row >= 0 && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        emit showNowRequested(row);
        return;
    }
    if (row >= 0 && event->key() == Qt::Key_Delete) {
        emit removeRequested(row);
        return;
    }
    QListView::keyPressEvent(event);
}

void PlaylistView::changeEvent(QEvent* event) {
    QListView::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        viewport()->update(); // delegate re-reads the active theme's colors
    }
}

void PlaylistView::paintEvent(QPaintEvent* event) {
    const Theme::ThemePalette& pal = Theme::themePalette(Theme::currentTheme());
    {
        QPainter p(viewport());
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(pal.border, 1));
        p.setBrush(pal.altBg);
        p.drawRoundedRect(QRectF(viewport()->rect()).adjusted(0.5, 0.5, -0.5, -0.5), cardRadius(), cardRadius());
        if (model()->rowCount() == 0) {
            QFont title = font();
            title.setBold(true);
            title.setPointSizeF(title.pointSizeF() + 2);
            p.setFont(title);
            p.setPen(pal.textPrimary);
            const QRect r = viewport()->rect();
            p.drawText(r.adjusted(0, 0, 0, -28), Qt::AlignCenter, tr("Your playlist is empty"));
            p.setFont(font());
            p.setPen(pal.textSecondary);
            p.drawText(r.adjusted(0, 28, 0, 0), Qt::AlignCenter,
                       tr("Click \"Add Images\" or drop image files here."));
        }
    }
    QListView::paintEvent(event);
    if (m_dropIndicatorPos >= 0 && model()->rowCount() > 0) {
        QPainter p(viewport());
        const int n = model()->rowCount();
        QRect r;
        int x = 0;
        if (m_dropIndicatorPos < n) {
            r = visualRect(model()->index(m_dropIndicatorPos, 0));
            x = r.left() - 1;
        } else {
            r = visualRect(model()->index(n - 1, 0));
            x = r.right() + 1;
        }
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(pal.accent);
        p.drawRoundedRect(QRectF(x - 2, r.top() + 6, 4, r.height() - 12), 2, 2);
    }
}

// -------------------------------------------------------------- PlaylistDialog

PlaylistDialog::PlaylistDialog(ImagePlaylist* playlist, PlaylistRotation* rotation, QWidget* parent)
    : QDialog(parent), m_playlist(playlist), m_rotation(rotation) {
    setWindowTitle(tr("Image Playlist"));
    setWindowIcon(QIcon(playlistIconPath()));
    buildUi();
    setMinimumSize(720, 600);
    resize(900, 700);

    connect(m_playlist, &ImagePlaylist::contentsChanged, this, &PlaylistDialog::updateUi);
    connect(m_playlist, &ImagePlaylist::currentChanged, this, &PlaylistDialog::updateUi);
    connect(m_playlist, &ImagePlaylist::enabledChanged, this, [this](bool on) {
        m_enabledCheck->blockSignals(true);
        m_enabledCheck->setChecked(on);
        m_enabledCheck->blockSignals(false);
        m_view->viewport()->update();
        updateUi();
    });
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &PlaylistDialog::updateUi);
    updateUi();
}

void PlaylistDialog::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(12);

    // --- Header: title + live summary, and the on/off switch ---
    auto* header = new QHBoxLayout();
    header->setSpacing(10);
    auto* titleBlock = new QVBoxLayout();
    titleBlock->setSpacing(2);
    auto* title = new QLabel(tr("Image Playlist"), this);
    title->setObjectName(QStringLiteral("appTitle"));
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 3);
    title->setFont(titleFont);
    titleBlock->addWidget(title);
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setObjectName(QStringLiteral("secondaryText"));
    titleBlock->addWidget(m_summaryLabel);
    header->addLayout(titleBlock, 1);

    m_enabledCheck = new QCheckBox(tr("Use image playlist"), this);
    QFont toggleFont = m_enabledCheck->font();
    toggleFont.setBold(true);
    m_enabledCheck->setFont(toggleFont);
    m_enabledCheck->setChecked(m_playlist->isEnabled());
    m_enabledCheck->setToolTip(tr("When on, the playlist decides which image is shown and changes it "
                                  "automatically. Turning it off keeps the current image and your playlist."));
    connect(m_enabledCheck, &QCheckBox::toggled, this, [this](bool checked) {
        const bool result = m_playlist->setEnabled(checked);
        if (result != checked) {
            m_enabledCheck->blockSignals(true);
            m_enabledCheck->setChecked(result);
            m_enabledCheck->blockSignals(false);
            showNotice(tr("Add at least one image that can be found on disk first."));
        }
    });
    header->addWidget(m_enabledCheck, 0, Qt::AlignVCenter);
    root->addLayout(header);

    auto* rule = new QFrame(this);
    rule->setObjectName(QStringLiteral("headerRule"));
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Plain);
    root->addWidget(rule);

    // --- Cards ---
    m_view = new PlaylistView(m_playlist, this);
    // One full row of cards at minimum; the view takes all extra height
    // (stretch 1) and scrolls beyond that. A taller minimum made the
    // layout's total minimum exceed the window and overlap the hint below.
    m_view->setMinimumHeight(kCardHeight + 3 * kCardSpacing);
    root->addWidget(m_view, 1);
    connect(m_view, &PlaylistView::filesDropped, this, [this](const QStringList& files, int row) { addFiles(files, row); });
    connect(m_view, &PlaylistView::moveRequested, this, [this](int from, int to) {
        if (m_playlist->move(from, to)) {
            selectRow(to);
        }
    });
    connect(m_view, &PlaylistView::showNowRequested, this, [this](int row) {
        selectRow(row);
        onShowNow();
    });
    connect(m_view, &PlaylistView::removeRequested, this, [this](int) { onRemove(); });
    connect(m_view, &QListView::doubleClicked, this, [this](const QModelIndex& idx) {
        selectRow(idx.row());
        onShowNow();
    });

    auto* hint = new QLabel(tr("Drag images to set the order  •  Double-click an image to show it now  •  "
                               "Ctrl+← / Ctrl+→ moves the selected image"), this);
    hint->setObjectName(QStringLiteral("secondaryText"));
    hint->setWordWrap(true);
    root->addWidget(hint);

    // --- Actions ---
    auto* actions = new QHBoxLayout();
    actions->setSpacing(8);
    m_addButton = new QPushButton(tr("+  Add Images"), this);
    m_addButton->setToolTip(tr("Add one or more image files to the end of the playlist"));
    connect(m_addButton, &QPushButton::clicked, this, &PlaylistDialog::onAddImages);
    actions->addWidget(m_addButton);
    m_moveLeftButton = new QPushButton(tr("◀  Move Left"), this);
    m_moveLeftButton->setToolTip(tr("Move the selected image earlier in the order (Ctrl+Left)"));
    connect(m_moveLeftButton, &QPushButton::clicked, this, [this] { onMove(-1); });
    actions->addWidget(m_moveLeftButton);
    m_moveRightButton = new QPushButton(tr("Move Right  ▶"), this);
    m_moveRightButton->setToolTip(tr("Move the selected image later in the order (Ctrl+Right)"));
    connect(m_moveRightButton, &QPushButton::clicked, this, [this] { onMove(+1); });
    actions->addWidget(m_moveRightButton);
    m_showNowButton = new QPushButton(tr("Show Now"), this);
    m_showNowButton->setToolTip(tr("Make the selected image the current one (Enter)"));
    connect(m_showNowButton, &QPushButton::clicked, this, &PlaylistDialog::onShowNow);
    actions->addWidget(m_showNowButton);
    m_removeButton = new QPushButton(tr("Remove"), this);
    m_removeButton->setToolTip(tr("Remove the selected image from the playlist (Delete). The file itself is not deleted."));
    connect(m_removeButton, &QPushButton::clicked, this, &PlaylistDialog::onRemove);
    actions->addWidget(m_removeButton);
    actions->addStretch();
    m_clearButton = new QPushButton(tr("Clear Playlist"), this);
    m_clearButton->setToolTip(tr("Remove every image from the playlist. No files are deleted."));
    connect(m_clearButton, &QPushButton::clicked, this, &PlaylistDialog::onClear);
    actions->addWidget(m_clearButton);
    root->addLayout(actions);

    m_noticeLabel = new QLabel(this);
    m_noticeLabel->setObjectName(QStringLiteral("secondaryText"));
    m_noticeLabel->setVisible(false);
    root->addWidget(m_noticeLabel);

    // --- Rotation settings ---
    auto* rotationSection = new QLabel(tr("Change image automatically"), this);
    rotationSection->setObjectName(QStringLiteral("sectionLabel"));
    QFont sectionFont = rotationSection->font();
    sectionFont.setBold(true);
    rotationSection->setFont(sectionFont);
    root->addWidget(rotationSection);

    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(24);
    grid->setVerticalSpacing(10);
    m_unlockCheck = new QCheckBox(tr("After Lock → Unlock"), this);
    m_unlockCheck->setToolTip(tr("Show the next image each time you unlock Windows after it was locked."));
    m_unlockCheck->setChecked(m_rotation->rotateOnUnlock());
    connect(m_unlockCheck, &QCheckBox::toggled, m_rotation, &PlaylistRotation::setRotateOnUnlock);
    grid->addWidget(m_unlockCheck, 0, 0);

    m_startCheck = new QCheckBox(tr("After Windows restarts or you sign in"), this);
    m_startCheck->setToolTip(tr("Show the next image when Motiva starts in a new Windows session. Restarting "
                                "Explorer or Motiva itself does not count."));
    m_startCheck->setChecked(m_rotation->rotateOnWindowsStart());
    connect(m_startCheck, &QCheckBox::toggled, m_rotation, &PlaylistRotation::setRotateOnWindowsStart);
    grid->addWidget(m_startCheck, 0, 1);

    auto* intervalRow = new QHBoxLayout();
    intervalRow->setSpacing(8);
    m_intervalCheck = new QCheckBox(tr("Every"), this);
    m_intervalCheck->setChecked(m_rotation->rotateOnInterval());
    connect(m_intervalCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_rotation->setRotateOnInterval(on);
        m_intervalCombo->setEnabled(on);
    });
    intervalRow->addWidget(m_intervalCheck);
    m_intervalCombo = new QComboBox(this);
    m_intervalCombo->setAccessibleName(tr("Image change interval"));
    const QList<int> minutes = {1, 5, 10, 15, 30, 60, 120, 180, 360, 720, 1440};
    for (int m : minutes) {
        const QString label = (m < 60) ? countText(m, tr("%1 minute"), tr("%1 minutes"))
                                       : countText(m / 60, tr("%1 hour"), tr("%1 hours"));
        m_intervalCombo->addItem(label, m);
    }
    int comboIndex = m_intervalCombo->findData(m_rotation->intervalMinutes());
    if (comboIndex < 0) { // a value saved outside the preset list
        m_intervalCombo->addItem(countText(m_rotation->intervalMinutes(), tr("%1 minute"), tr("%1 minutes")),
                                 m_rotation->intervalMinutes());
        comboIndex = m_intervalCombo->count() - 1;
    }
    m_intervalCombo->setCurrentIndex(comboIndex);
    m_intervalCombo->setEnabled(m_rotation->rotateOnInterval());
    connect(m_intervalCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        m_rotation->setIntervalMinutes(m_intervalCombo->currentData().toInt());
    });
    intervalRow->addWidget(m_intervalCombo);
    intervalRow->addStretch();
    grid->addLayout(intervalRow, 1, 0, 1, 2);
    root->addLayout(grid);

    auto* rotationNote = new QLabel(tr("Images change in playlist order and wrap around to the first after the last. "
                                       "Automatic changes only happen while the playlist is on."), this);
    rotationNote->setObjectName(QStringLiteral("secondaryText"));
    rotationNote->setWordWrap(true);
    root->addWidget(rotationNote);

    // --- Footer ---
    auto* footer = new QHBoxLayout();
    footer->addStretch();
    m_applyButton = new QPushButton(tr("Set Playlist as Wallpaper"), this);
    m_applyButton->setObjectName(QStringLiteral("primaryButton"));
    m_applyButton->setMinimumHeight(38);
    connect(m_applyButton, &QPushButton::clicked, this, &PlaylistDialog::applyToDesktopRequested);
    footer->addWidget(m_applyButton);
    auto* closeButton = new QPushButton(tr("Close"), this);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    footer->addWidget(closeButton);
    root->addLayout(footer);
}

void PlaylistDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    setWindowIcon(QIcon(playlistIconPath()));
    m_playlist->refreshAvailability(); // a drive may have been reconnected/removed
    updateUi();
}

int PlaylistDialog::selectedRow() const {
    // The selection itself, not the view's "current index": accessibility
    // clients (UI Automation SelectionItem.Select, i.e. screen readers)
    // select an item without moving the current index.
    const QModelIndexList selected = m_view->selectionModel()->selectedIndexes();
    return selected.isEmpty() ? -1 : selected.first().row();
}

void PlaylistDialog::selectRow(int row) {
    if (row < 0 || row >= m_playlist->count()) {
        return;
    }
    const QModelIndex idx = m_playlist->index(row);
    m_view->setCurrentIndex(idx);
    m_view->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect);
    m_view->scrollTo(idx);
    updateUi();
}

void PlaylistDialog::showNotice(const QString& text) {
    m_noticeLabel->setText(text);
    m_noticeLabel->setVisible(!text.isEmpty());
}

void PlaylistDialog::addFiles(const QStringList& paths, int insertRow) {
    const ImagePlaylist::AddResult result = m_playlist->addImages(paths, insertRow);
    QStringList parts;
    if (result.added > 0) {
        parts << countText(result.added, tr("Added %1 image."), tr("Added %1 images."));
    }
    if (result.duplicates > 0) {
        parts << countText(result.duplicates, tr("%1 was already in the playlist."),
                           tr("%1 were already in the playlist."));
    }
    if (result.unsupported > 0) {
        parts << countText(result.unsupported, tr("%1 file skipped - not a supported image."),
                           tr("%1 files skipped - not supported images."));
    }
    showNotice(parts.join(QLatin1Char(' ')));
    if (result.added > 0) {
        const int first = (insertRow < 0 || insertRow > m_playlist->count() - result.added)
            ? m_playlist->count() - result.added : insertRow;
        selectRow(first);
    }
    updateUi();
}

void PlaylistDialog::onAddImages() {
    QStringList patterns;
    for (const QString& ext : VideoPlayer::supportedStaticImageExtensions()) {
        patterns << QStringLiteral("*.%1").arg(ext);
    }
    patterns.sort(Qt::CaseInsensitive);
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Add Images to Playlist"), QString(),
        tr("Images (%1);;All Files (*)").arg(patterns.join(QLatin1Char(' '))));
    if (!files.isEmpty()) {
        addFiles(files);
    }
}

void PlaylistDialog::onRemove() {
    const int row = selectedRow();
    if (row < 0) {
        return;
    }
    const QString name = QFileInfo(m_playlist->pathAt(row)).fileName();
    m_playlist->removeAt(row);
    showNotice(tr("Removed \"%1\" from the playlist. The file itself was not touched.").arg(name));
    selectRow(qMin(row, m_playlist->count() - 1));
    updateUi();
}

void PlaylistDialog::onClear() {
    if (m_playlist->count() == 0) {
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Clear Playlist"),
        countText(m_playlist->count(), tr("Remove the %1 image from the playlist?\n\nThe image file is not deleted."),
                  tr("Remove all %1 images from the playlist?\n\nYour image files are not deleted.")),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }
    m_playlist->clear();
    showNotice(tr("Playlist cleared. No files were deleted."));
    updateUi();
}

void PlaylistDialog::onMove(int delta) {
    const int row = selectedRow();
    const int to = row + delta;
    if (row < 0 || to < 0 || to >= m_playlist->count()) {
        return;
    }
    if (m_playlist->move(row, to)) {
        selectRow(to);
    }
}

void PlaylistDialog::onShowNow() {
    const int row = selectedRow();
    if (row < 0) {
        return;
    }
    // setCurrentIndex re-checks the file and refuses a missing one.
    if (!m_playlist->setCurrentIndex(row)) {
        showNotice(tr("That image can't be found on disk. Reconnect its drive or remove it from the playlist."));
        return;
    }
    // Choosing an image to show is using the playlist - turn it on so the
    // image is applied (and rotation continues from it).
    if (!m_playlist->isEnabled()) {
        m_playlist->setEnabled(true);
    }
    showNotice(QString());
}

void PlaylistDialog::setPlaylistOnDesktop(bool onDesktop) {
    m_onDesktop = onDesktop;
    updateUi();
}

void PlaylistDialog::updateUi() {
    if (!m_summaryLabel) {
        return;
    }
    const int count = m_playlist->count();
    const int available = m_playlist->availableCount();
    const int missing = count - available;
    const int row = selectedRow();
    const int current = m_playlist->currentIndex();

    QString summary;
    if (count == 0) {
        summary = tr("No images yet - add some to start a rotating wallpaper.");
    } else if (m_playlist->isEnabled()) {
        summary = countText(count, tr("%1 image"), tr("%1 images")) + QStringLiteral("  •  ")
            + tr("Showing image %1 of %2").arg(current + 1).arg(count);
        summary += m_onDesktop ? tr("  •  On the desktop") : tr("  •  Not on the desktop yet");
    } else {
        summary = countText(count, tr("%1 image"), tr("%1 images")) + QStringLiteral("  •  ") + tr("Playlist is off");
        if (current >= 0) {
            summary += tr(" - resumes at image %1").arg(current + 1);
        }
    }
    if (missing > 0) {
        summary += QStringLiteral("  •  ") + countText(missing, tr("%1 file not found"), tr("%1 files not found"));
    }
    m_summaryLabel->setText(summary);

    m_moveLeftButton->setEnabled(row > 0);
    m_moveRightButton->setEnabled(row >= 0 && row < count - 1);
    m_showNowButton->setEnabled(row >= 0 && (row != current || !m_playlist->isEnabled()));
    m_removeButton->setEnabled(row >= 0);
    m_clearButton->setEnabled(count > 0);

    const bool playlistOnDesktop = m_onDesktop && m_playlist->isEnabled();
    m_applyButton->setText(playlistOnDesktop ? tr("Playlist is on the desktop") : tr("Set Playlist as Wallpaper"));
    m_applyButton->setEnabled(!playlistOnDesktop && available > 0);
}
