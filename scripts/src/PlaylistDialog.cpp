#include "PlaylistDialog.h"
#include "MediaRecovery.h"
#include "PlaylistLibrary.h"
#include "PlaylistModel.h"
#include "Theme.h"
#include "VideoPlayer.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace {

constexpr const char* kRowMimeType = "application/x-motiva-playlist-row";
constexpr int kCardWidth = 184;
constexpr int kCardHeight = 158;
constexpr int kCardSpacing = 12;

// The app ships no translation files, so tr("%n item(s)") would show the
// literal "(s)" - spell out the singular/plural forms instead.
QString countText(int n, const QString& one, const QString& many) {
    return (n == 1 ? one : many).arg(n);
}

QString playlistIconPath() {
    return QStringLiteral(":/playlist/%1/playlist.svg").arg(Theme::iconVariant(Theme::currentTheme()));
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

void drawPill(QPainter* p, const QRectF& rect, const QColor& fill, const QString& text, int radius) {
    p->setPen(Qt::NoPen);
    p->setBrush(fill);
    p->drawRoundedRect(rect, qMin(4, radius), qMin(4, radius));
    p->setPen(Qt::white);
    p->drawText(rect, Qt::AlignCenter, text);
}

// One playlist item as a card: thumbnail (cropped to fill, on the same
// fixed-dark media surface the main preview uses), order number, a video
// marker for video items, "Now showing" on the active playlist's current
// item, and an explicit "File not found" state. Colors come from the active
// Motiva Theme at paint time, so a live theme switch restyles it.
class PlaylistCardDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(kCardWidth, kCardHeight);
    }

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const Theme::ThemePalette& pal = Theme::themePalette(Theme::currentTheme());
        const auto* model = qobject_cast<const PlaylistModel*>(index.model());
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const bool available = index.data(PlaylistModel::AvailableRole).toBool();
        const bool isCurrent = index.data(PlaylistModel::IsCurrentRole).toBool();
        const bool isVideo = index.data(PlaylistModel::IsVideoRole).toBool();
        const bool playlistActive = model && model->isActive();
        const int radius = cardRadius();

        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setRenderHint(QPainter::SmoothPixmapTransform, true);

        const QRectF card = QRectF(option.rect).adjusted(3, 3, -3, -3);
        p->setPen(Qt::NoPen);
        p->setBrush(pal.panelBg);
        p->drawPath(roundedPath(card, radius));

        const QRectF thumb = card.adjusted(8, 8, -8, -36);
        const QPainterPath thumbPath = roundedPath(thumb, qMax(1, radius - 3));
        p->fillPath(thumbPath, QColor(Theme::kPreviewSurfaceBg));
        if (available) {
            const QImage image = index.data(PlaylistModel::ThumbnailRole).value<QImage>();
            if (!image.isNull()) {
                p->save();
                p->setClipPath(thumbPath);
                const QSizeF scaled = QSizeF(image.size()).scaled(thumb.size(), Qt::KeepAspectRatioByExpanding);
                p->drawImage(QRectF(thumb.center().x() - scaled.width() / 2, thumb.center().y() - scaled.height() / 2,
                                    scaled.width(), scaled.height()),
                             image);
                p->restore();
            } else {
                p->setPen(QColor(Theme::kPreviewText));
                const bool pending = index.data(PlaylistModel::ThumbnailPendingRole).toBool();
                p->drawText(thumb, Qt::AlignCenter,
                            pending ? PlaylistDialog::tr("Loading…")
                                    : (isVideo ? PlaylistDialog::tr("▶  No preview") : PlaylistDialog::tr("No preview")));
            }
        } else {
            p->setPen(QPen(QColor(Theme::kStatusWarning), 1.2, Qt::DashLine));
            p->setBrush(Qt::NoBrush);
            p->drawPath(roundedPath(thumb.adjusted(1, 1, -1, -1), qMax(1, radius - 3)));
            QFont f = option.font;
            f.setBold(true);
            p->setFont(f);
            p->setPen(QColor(Theme::kStatusWarning));
            p->drawText(thumb, Qt::AlignCenter, PlaylistDialog::tr("File not found"));
        }

        QFont badgeFont = option.font;
        badgeFont.setBold(true);
        badgeFont.setPointSizeF(qMax(7.0, badgeFont.pointSizeF() - 1));
        p->setFont(badgeFont);
        const QFontMetrics fm = p->fontMetrics();
        const QString number = QStringLiteral("%1").arg(index.row() + 1, 2, 10, QLatin1Char('0'));
        drawPill(p, QRectF(thumb.left() + 6, thumb.top() + 6, fm.horizontalAdvance(number) + 12, fm.height() + 4),
                 QColor(0, 0, 0, 165), number, radius);
        if (isVideo && available) {
            const QString tag = PlaylistDialog::tr("▶ VIDEO");
            drawPill(p, QRectF(thumb.left() + 6, thumb.bottom() - fm.height() - 10, fm.horizontalAdvance(tag) + 12,
                               fm.height() + 4),
                     QColor(0, 0, 0, 165), tag, radius);
        }
        if (isCurrent) {
            const QString label = playlistActive ? PlaylistDialog::tr("Now showing") : PlaylistDialog::tr("Resumes here");
            const qreal w = fm.horizontalAdvance(label) + 14;
            drawPill(p, QRectF(thumb.right() - 6 - w, thumb.top() + 6, w, fm.height() + 4),
                     playlistActive ? pal.accent : QColor(0, 0, 0, 165), label, radius);
        }

        p->setFont(option.font);
        const QRectF nameRect(card.left() + 10, thumb.bottom() + 6, card.width() - 20, 22);
        p->setPen(available ? pal.textPrimary : pal.textDisabled);
        p->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                    p->fontMetrics().elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle,
                                                int(nameRect.width())));

        QColor outline = pal.border;
        qreal width = 1.0;
        if (selected) {
            outline = pal.accent;
            width = 2.0;
        } else if (isCurrent && playlistActive) {
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
};

// A "My Playlists" row: name, kind + item count, and an "Active" marker on
// the playlist that controls the wallpaper.
class PlaylistRowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return QSize(200, 52); }

    // Inline rename: a plain line edit over the name line of the row.
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex&) const override {
        const QRect row = option.rect.adjusted(8, 4, -8, -4);
        editor->setGeometry(QRect(row.left(), row.top(), row.width(), qMin(row.height(), 28)));
    }

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const Theme::ThemePalette& pal = Theme::themePalette(Theme::currentTheme());
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const bool active = index.data(PlaylistListModel::IsActiveRole).toBool();
        const bool video = index.data(PlaylistListModel::TypeRole).toInt() == static_cast<int>(PlaylistType::Video);
        const int count = index.data(PlaylistListModel::CountRole).toInt();
        const int radius = cardRadius();

        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        const QRectF row = QRectF(option.rect).adjusted(4, 2, -4, -2);
        if (selected || hovered) {
            p->setPen(Qt::NoPen);
            p->setBrush(selected ? pal.accentSoft : pal.buttonHoverBg);
            p->drawRoundedRect(row, qMin(6, radius), qMin(6, radius));
        }
        if (selected) {
            p->setBrush(pal.accent);
            p->drawRoundedRect(QRectF(row.left(), row.top() + 8, 3, row.height() - 16), 1.5, 1.5);
        }

        QFont bold = option.font;
        bold.setBold(true);
        const QFontMetrics boldFm(bold);
        const QFontMetrics fm(option.font);
        const QString activeLabel = PlaylistDialog::tr("Active");
        const qreal pillW = active ? fm.horizontalAdvance(activeLabel) + 14 : 0;
        const QRectF nameRect(row.left() + 12, row.top() + 6, row.width() - 24 - pillW, boldFm.height());
        p->setFont(bold);
        p->setPen(pal.textPrimary);
        p->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                    boldFm.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, int(nameRect.width())));

        p->setFont(option.font);
        p->setPen(pal.textSecondary);
        const QString subtitle = (video ? PlaylistDialog::tr("Videos") : PlaylistDialog::tr("Images")) +
            QStringLiteral("  •  ") + QString::number(count);
        p->drawText(QRectF(row.left() + 12, nameRect.bottom() + 2, row.width() - 24, fm.height()),
                    Qt::AlignLeft | Qt::AlignVCenter, subtitle);

        if (active) {
            drawPill(p, QRectF(row.right() - 8 - pillW, row.top() + 6, pillW, fm.height() + 4), pal.accent,
                     activeLabel, radius);
        }
        p->restore();
    }
};

} // namespace

// ---------------------------------------------------------------- PlaylistView

PlaylistView::PlaylistView(QWidget* parent) : QListView(parent) {
    setItemDelegate(new PlaylistCardDelegate(this));
    setViewMode(QListView::IconMode);
    setFlow(QListView::LeftToRight);
    setWrapping(true);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setUniformItemSizes(true); // constant-time layout even for thousands of items
    setLayoutMode(QListView::Batched);
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
    setContextMenuPolicy(Qt::CustomContextMenu); // item actions live in the right-click menu (PlaylistDialog)
    setAccessibleName(tr("Playlist items"));
    setAccessibleDescription(tr("Ordered playlist items. Right-click or press the Menu key for item actions. "
                                "Ctrl+Left and Ctrl+Right move the selected item, Enter shows it now, Delete "
                                "removes it from the playlist."));
}

void PlaylistView::setPlaylist(PlaylistModel* playlist) {
    m_playlist = playlist;
    setModel(playlist);
    viewport()->update();
}

QStringList PlaylistView::acceptedLocalFiles(const QMimeData* mime) const {
    QStringList files;
    if (!m_playlist || !mime || !mime->hasUrls()) {
        return files;
    }
    for (const QUrl& url : mime->urls()) {
        if (url.isLocalFile() && m_playlist->acceptsFile(url.toLocalFile())) {
            files << url.toLocalFile(); // browser thumbnails / web URLs are never accepted
        }
    }
    return files;
}

int PlaylistView::insertPositionAt(const QPoint& pos) const {
    const int n = model() ? model()->rowCount() : 0;
    for (int row = 0; row < n; ++row) {
        const QRect r = visualRect(model()->index(row, 0));
        if (pos.y() < r.top() - spacing()) {
            return row;
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
    const QImage thumb = idx.data(PlaylistModel::ThumbnailRole).value<QImage>();
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
    } else if (!acceptedLocalFiles(mime).isEmpty()) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else {
        event->ignore();
    }
}

void PlaylistView::dragMoveEvent(QDragMoveEvent* event) {
    const QMimeData* mime = event->mimeData();
    const bool internal = mime->hasFormat(kRowMimeType) && event->source() == this;
    if (!internal && acceptedLocalFiles(mime).isEmpty()) {
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
    const QStringList files = acceptedLocalFiles(mime);
    if (files.isEmpty()) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
    emit filesDropped(files, pos);
}

void PlaylistView::keyPressEvent(QKeyEvent* event) {
    const QModelIndexList selected = selectionModel() ? selectionModel()->selectedIndexes() : QModelIndexList();
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
    const int n = model() ? model()->rowCount() : 0;
    {
        QPainter p(viewport());
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(pal.border, 1));
        p.setBrush(pal.altBg);
        p.drawRoundedRect(QRectF(viewport()->rect()).adjusted(0.5, 0.5, -0.5, -0.5), cardRadius(), cardRadius());
        if (n == 0) {
            QFont title = font();
            title.setBold(true);
            title.setPointSizeF(title.pointSizeF() + 2);
            p.setFont(title);
            p.setPen(pal.textPrimary);
            const QRect r = viewport()->rect();
            p.drawText(r.adjusted(0, 0, 0, -28), Qt::AlignCenter, tr("This playlist is empty"));
            p.setFont(font());
            p.setPen(pal.textSecondary);
            const bool video = m_playlist && m_playlist->isVideo();
            p.drawText(r.adjusted(0, 28, 0, 0), Qt::AlignCenter,
                       video ? tr("Click \"Add Videos\" or drop video files here.")
                             : tr("Click \"Add Images\" or drop image files here."));
        }
    }
    QListView::paintEvent(event);
    if (m_dropIndicatorPos >= 0 && n > 0) {
        QPainter p(viewport());
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

PlaylistDialog::PlaylistDialog(PlaylistLibrary* library, QWidget* parent)
    : QDialog(parent), m_library(library) {
    setWindowTitle(tr("Playlists"));
    setWindowIcon(QIcon(playlistIconPath()));
    buildUi();
    setMinimumSize(900, 620);
    resize(1080, 720);

    // Ctrl+N: same flow as "+ New" (choose image/video, then name it).
    // Window-scoped like MainWindow's Ctrl+V shortcut; ignored while a text
    // field has focus so typing is never hijacked.
    auto* newShortcut = new QShortcut(QKeySequence::New, this);
    connect(newShortcut, &QShortcut::activated, this, [this] {
        if (!qobject_cast<QLineEdit*>(QApplication::focusWidget())) {
            m_newButton->showMenu();
        }
    });

    connect(m_library, &PlaylistLibrary::playlistsChanged, this, &PlaylistDialog::updateUi);
    // Cleanup & Reset closes the library and destroys every playlist model;
    // let go of ours first, then bind to whatever the reopened library has.
    connect(m_library, &PlaylistLibrary::aboutToReset, this, [this] { m_view->setPlaylist(nullptr); });
    connect(m_library, &PlaylistLibrary::resetFinished, this, [this] { selectPlaylist(m_library->selectedId()); });
    connect(m_library, &PlaylistLibrary::activeChanged, this, [this] { bindSelectedPlaylist(); });
    connect(m_library, &PlaylistLibrary::activeCurrentChanged, this, &PlaylistDialog::updateUi);
    connect(m_library, &PlaylistLibrary::errorOccurred, this, [this](const QString& message) {
        if (isVisible()) {
            QMessageBox::warning(this, tr("Playlist library"),
                                 tr("The change could not be saved to the playlist library.\n\n%1").arg(message));
        }
    });
    selectPlaylist(m_library->selectedId());
}

void PlaylistDialog::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(12);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel(tr("Playlists"), this);
    title->setObjectName(QStringLiteral("appTitle"));
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 3);
    title->setFont(titleFont);
    header->addWidget(title);
    header->addStretch();
    auto* importButton = new QPushButton(tr("Import…"), this);
    importButton->setToolTip(tr("Add the playlists from another Motiva library (.mtv) to yours"));
    connect(importButton, &QPushButton::clicked, this, &PlaylistDialog::onImport);
    header->addWidget(importButton);
    auto* exportButton = new QPushButton(tr("Export…"), this);
    exportButton->setToolTip(tr("Save a copy of your playlist library as a .mtv file. Media files are "
                                "referenced, not copied."));
    connect(exportButton, &QPushButton::clicked, this, &PlaylistDialog::onExport);
    header->addWidget(exportButton);
    root->addLayout(header);

    auto* rule = new QFrame(this);
    rule->setObjectName(QStringLiteral("headerRule"));
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Plain);
    root->addWidget(rule);

    auto* columns = new QHBoxLayout();
    columns->setSpacing(18);
    columns->addWidget(buildSidebar());
    columns->addWidget(buildEditor(), 1);
    root->addLayout(columns, 1);

    auto* footer = new QHBoxLayout();
    auto* libraryLabel = new QLabel(this);
    libraryLabel->setObjectName(QStringLiteral("secondaryText"));
    libraryLabel->setText(m_library->isPersistent()
                              ? tr("Library: %1").arg(QFileInfo(m_library->libraryPath()).fileName())
                              : tr("Library not saved this session"));
    libraryLabel->setToolTip(QDir::toNativeSeparators(m_library->libraryPath()));
    footer->addWidget(libraryLabel);
    footer->addStretch();
    m_applyButton = new QPushButton(tr("Set as Wallpaper"), this);
    m_applyButton->setObjectName(QStringLiteral("primaryButton"));
    m_applyButton->setMinimumHeight(38);
    connect(m_applyButton, &QPushButton::clicked, this, [this] {
        if (PlaylistModel* m = current()) {
            emit applyToDesktopRequested(m->id());
        }
    });
    footer->addWidget(m_applyButton);
    auto* closeButton = new QPushButton(tr("Close"), this);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    footer->addWidget(closeButton);
    root->addLayout(footer);
}

QWidget* PlaylistDialog::buildSidebar() {
    auto* panel = new QWidget(this);
    panel->setFixedWidth(250);
    auto* col = new QVBoxLayout(panel);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(10);

    auto* label = new QLabel(tr("My Playlists"), panel);
    label->setObjectName(QStringLiteral("sectionLabel"));
    QFont f = label->font();
    f.setBold(true);
    label->setFont(f);
    col->addWidget(label);

    m_playlistList = new QListView(panel);
    m_playlistList->setModel(m_library->listModel());
    m_playlistList->setItemDelegate(new PlaylistRowDelegate(m_playlistList));
    m_playlistList->setUniformItemSizes(true);
    m_playlistList->setMouseTracking(true);
    m_playlistList->setFrameShape(QFrame::NoFrame);
    m_playlistList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_playlistList->setAccessibleName(tr("My Playlists"));
    m_playlistList->setStyleSheet(QStringLiteral("QListView { background: transparent; }"));
    m_playlistList->setAccessibleDescription(tr("Right-click for Rename and Delete. F2 or double-click renames, "
                                                "Delete or D deletes, Ctrl+N creates a playlist."));
    // Double-click and F2 (EditKeyPressed) open the inline name editor;
    // Enter commits, Escape cancels (standard item-view editing).
    m_playlistList->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_playlistList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_playlistList, &QWidget::customContextMenuRequested, this, &PlaylistDialog::showPlaylistMenu);
    m_playlistList->installEventFilter(this);
    connect(m_playlistList->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &PlaylistDialog::onSelectionInSidebar);
    col->addWidget(m_playlistList, 1);

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(6);
    m_newButton = new QPushButton(tr("+  New"), panel);
    auto* newMenu = new QMenu(m_newButton);
    newMenu->addAction(tr("Image Playlist"), this, [this] { onCreatePlaylist(static_cast<int>(PlaylistType::Image)); });
    newMenu->addAction(tr("Video Playlist"), this, [this] { onCreatePlaylist(static_cast<int>(PlaylistType::Video)); });
    m_newButton->setMenu(newMenu);
    m_newButton->setToolTip(tr("Create a new image or video playlist (Ctrl+N)"));
    buttons->addWidget(m_newButton);
    buttons->addStretch();
    col->addLayout(buttons);
    return panel;
}

QWidget* PlaylistDialog::buildEditor() {
    m_editorStack = new QStackedWidget(this);

    m_emptyLabel = new QLabel(m_editorStack);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setObjectName(QStringLiteral("secondaryText"));
    m_editorStack->addWidget(m_emptyLabel);

    auto* editor = new QWidget(m_editorStack);
    auto* col = new QVBoxLayout(editor);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(10);

    auto* head = new QHBoxLayout();
    auto* nameBlock = new QVBoxLayout();
    nameBlock->setSpacing(2);
    m_nameLabel = new QLabel(editor);
    QFont nf = m_nameLabel->font();
    nf.setBold(true);
    nf.setPointSize(nf.pointSize() + 2);
    m_nameLabel->setFont(nf);
    nameBlock->addWidget(m_nameLabel);
    m_summaryLabel = new QLabel(editor);
    m_summaryLabel->setObjectName(QStringLiteral("secondaryText"));
    nameBlock->addWidget(m_summaryLabel);
    head->addLayout(nameBlock, 1);
    m_activeCheck = new QCheckBox(tr("Use this playlist"), editor);
    QFont af = m_activeCheck->font();
    af.setBold(true);
    m_activeCheck->setFont(af);
    m_activeCheck->setToolTip(tr("The active playlist decides what is shown and changes it automatically. "
                                 "Only one playlist can be active. Turning it off keeps the current item."));
    connect(m_activeCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_binding || !current()) {
            return;
        }
        const bool ok = m_library->setActive(checked ? current()->id() : 0);
        if (!ok && checked) {
            m_binding = true;
            m_activeCheck->setChecked(false);
            m_binding = false;
            showNotice(tr("Add at least one item that can be found on disk first."));
        }
    });
    head->addWidget(m_activeCheck, 0, Qt::AlignVCenter);
    col->addLayout(head);

    m_view = new PlaylistView(editor);
    m_view->setMinimumHeight(kCardHeight + 3 * kCardSpacing);
    col->addWidget(m_view, 1);
    connect(m_view, &PlaylistView::filesDropped, this, [this](const QStringList& files, int row) {
        if (current()) {
            addFilesTo(current()->id(), files, row);
        }
    });
    connect(m_view, &PlaylistView::moveRequested, this, [this](int from, int to) {
        if (current() && current()->move(from, to)) {
            selectRow(to);
        }
    });
    connect(m_view, &PlaylistView::showNowRequested, this, [this](int row) {
        selectRow(row);
        if (current() && !current()->isAvailable(row)) {
            onFindFile(row);
        } else {
            onShowNow();
        }
    });
    connect(m_view, &PlaylistView::removeRequested, this, [this](int) { onRemove(); });
    connect(m_view, &QListView::doubleClicked, this, [this](const QModelIndex& idx) {
        selectRow(idx.row());
        if (current() && !current()->isAvailable(idx.row())) {
            onFindFile(idx.row());
        } else {
            onShowNow();
        }
    });

    m_hintLabel = new QLabel(tr("Right-click an item for more actions  •  Drag items to set the order  •  "
                                "Double-click an item to show it now"),
                             editor);
    m_hintLabel->setObjectName(QStringLiteral("secondaryText"));
    m_hintLabel->setWordWrap(true);
    col->addWidget(m_hintLabel);

    auto* actions = new QHBoxLayout();
    actions->setSpacing(8);
    m_addButton = new QPushButton(editor);
    connect(m_addButton, &QPushButton::clicked, this, &PlaylistDialog::onAddMedia);
    actions->addWidget(m_addButton);
    m_showNowButton = new QPushButton(tr("Show Now"), editor);
    m_showNowButton->setToolTip(tr("Make the selected item the current one (Enter)"));
    connect(m_showNowButton, &QPushButton::clicked, this, &PlaylistDialog::onShowNow);
    actions->addWidget(m_showNowButton);
    actions->addStretch();
    m_clearButton = new QPushButton(tr("Clear"), editor);
    m_clearButton->setToolTip(tr("Remove every item from this playlist. No files are deleted."));
    connect(m_clearButton, &QPushButton::clicked, this, &PlaylistDialog::onClear);
    actions->addWidget(m_clearButton);
    col->addLayout(actions);

    // Missing media: explains the state and offers Find File (search the
    // drives) or removing the entry. Styled from the active theme in
    // updateMissingBanner().
    m_missingBanner = new QFrame(editor);
    m_missingBanner->setObjectName(QStringLiteral("missingBanner"));
    auto* bannerRow = new QHBoxLayout(m_missingBanner);
    bannerRow->setContentsMargins(14, 10, 10, 10);
    bannerRow->setSpacing(10);
    auto* bannerText = new QVBoxLayout();
    bannerText->setSpacing(2);
    m_missingTitle = new QLabel(tr("File not found"), m_missingBanner);
    QFont mf = m_missingTitle->font();
    mf.setBold(true);
    m_missingTitle->setFont(mf);
    bannerText->addWidget(m_missingTitle);
    m_missingText = new QLabel(m_missingBanner);
    m_missingText->setObjectName(QStringLiteral("secondaryText"));
    m_missingText->setWordWrap(true);
    bannerText->addWidget(m_missingText);
    bannerRow->addLayout(bannerText, 1);
    m_findFileButton = new QPushButton(tr("Find File"), m_missingBanner);
    m_findFileButton->setObjectName(QStringLiteral("primaryButton"));
    m_findFileButton->setToolTip(tr("Search your drives for this file and repair its location"));
    connect(m_findFileButton, &QPushButton::clicked, this, [this] {
        PlaylistModel* m = current();
        if (!m) {
            return;
        }
        int row = selectedRow();
        if (row < 0 || m->isAvailable(row)) {
            for (row = 0; row < m->count() && m->isAvailable(row); ++row) {
            }
            selectRow(row);
        }
        onFindFile(row);
    });
    bannerRow->addWidget(m_findFileButton, 0, Qt::AlignVCenter);
    m_missingRemoveButton = new QPushButton(tr("Remove"), m_missingBanner);
    m_missingRemoveButton->setToolTip(tr("Remove this entry from the playlist. No file is deleted."));
    connect(m_missingRemoveButton, &QPushButton::clicked, this, &PlaylistDialog::onRemove);
    bannerRow->addWidget(m_missingRemoveButton, 0, Qt::AlignVCenter);
    m_missingBanner->setVisible(false);
    col->addWidget(m_missingBanner);

    connect(m_view, &QWidget::customContextMenuRequested, this, &PlaylistDialog::showItemMenu);

    m_noticeLabel = new QLabel(editor);
    m_noticeLabel->setObjectName(QStringLiteral("secondaryText"));
    m_noticeLabel->setWordWrap(true);
    m_noticeLabel->setVisible(false);
    col->addWidget(m_noticeLabel);

    // Settings differ by kind: image playlists have change triggers; video
    // playlists advance when a video ends.
    m_settingsStack = new QStackedWidget(editor);
    auto* imageSettings = new QWidget(m_settingsStack);
    auto* imageCol = new QVBoxLayout(imageSettings);
    imageCol->setContentsMargins(0, 0, 0, 0);
    imageCol->setSpacing(8);
    auto* rotationLabel = new QLabel(tr("Change image automatically"), imageSettings);
    rotationLabel->setObjectName(QStringLiteral("sectionLabel"));
    QFont rf = rotationLabel->font();
    rf.setBold(true);
    rotationLabel->setFont(rf);
    imageCol->addWidget(rotationLabel);
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(24);
    grid->setVerticalSpacing(8);
    m_unlockCheck = new QCheckBox(tr("After Lock → Unlock"), imageSettings);
    m_unlockCheck->setToolTip(tr("Show the next image each time you unlock Windows after it was locked."));
    grid->addWidget(m_unlockCheck, 0, 0);
    m_startCheck = new QCheckBox(tr("After Windows restarts or you sign in"), imageSettings);
    m_startCheck->setToolTip(tr("Show the next image when Motiva starts in a new Windows session. Restarting "
                                "Explorer or Motiva itself does not count."));
    grid->addWidget(m_startCheck, 0, 1);
    auto* intervalRow = new QHBoxLayout();
    m_intervalCheck = new QCheckBox(tr("Every"), imageSettings);
    intervalRow->addWidget(m_intervalCheck);
    m_intervalCombo = new QComboBox(imageSettings);
    m_intervalCombo->setAccessibleName(tr("Image change interval"));
    for (int m : {1, 5, 10, 15, 30, 60, 120, 180, 360, 720, 1440}) {
        m_intervalCombo->addItem(m < 60 ? countText(m, tr("%1 minute"), tr("%1 minutes"))
                                        : countText(m / 60, tr("%1 hour"), tr("%1 hours")),
                                 m);
    }
    intervalRow->addWidget(m_intervalCombo);
    intervalRow->addStretch();
    grid->addLayout(intervalRow, 1, 0, 1, 2);
    imageCol->addLayout(grid);
    auto* imageNote = new QLabel(tr("Images change in playlist order and wrap around after the last. Automatic "
                                    "changes only happen while this playlist is the active one."),
                                 imageSettings);
    imageNote->setObjectName(QStringLiteral("secondaryText"));
    imageNote->setWordWrap(true);
    imageCol->addWidget(imageNote);
    for (QCheckBox* c : {m_unlockCheck, m_startCheck, m_intervalCheck}) {
        connect(c, &QCheckBox::toggled, this, &PlaylistDialog::onRotationEdited);
    }
    connect(m_intervalCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &PlaylistDialog::onRotationEdited);
    m_settingsStack->addWidget(imageSettings);

    auto* videoSettings = new QWidget(m_settingsStack);
    auto* videoCol = new QVBoxLayout(videoSettings);
    videoCol->setContentsMargins(0, 0, 0, 0);
    videoCol->setSpacing(8);
    auto* videoLabel = new QLabel(tr("Playback"), videoSettings);
    videoLabel->setObjectName(QStringLiteral("sectionLabel"));
    videoLabel->setFont(rf);
    videoCol->addWidget(videoLabel);
    auto* videoNote = new QLabel(tr("Videos play once each, in playlist order. After the last video the playlist "
                                    "starts again from the first when \"Loop video\" is on in Settings, and stops "
                                    "on the last video when it is off."),
                                 videoSettings);
    videoNote->setObjectName(QStringLiteral("secondaryText"));
    videoNote->setWordWrap(true);
    videoCol->addWidget(videoNote);
    videoCol->addStretch();
    m_settingsStack->addWidget(videoSettings);
    col->addWidget(m_settingsStack);

    m_editorStack->addWidget(editor);
    return m_editorStack;
}

PlaylistModel* PlaylistDialog::current() const {
    return m_view ? m_view->playlist() : nullptr;
}

void PlaylistDialog::selectPlaylist(qint64 id) {
    const int row = m_library->listModel()->rowOf(id);
    if (row >= 0) {
        const QModelIndex idx = m_library->listModel()->index(row);
        m_playlistList->setCurrentIndex(idx);
        m_playlistList->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect);
    } else if (m_library->playlistCount() > 0) {
        selectPlaylist(m_library->listModel()->idAt(0));
        return;
    }
    bindSelectedPlaylist();
}

void PlaylistDialog::onSelectionInSidebar() {
    const QModelIndexList sel = m_playlistList->selectionModel()->selectedIndexes();
    if (!sel.isEmpty()) {
        m_library->setSelected(m_library->listModel()->idAt(sel.first().row()));
    }
    showNotice(QString());
    bindSelectedPlaylist();
}

void PlaylistDialog::bindSelectedPlaylist() {
    const QModelIndexList sel = m_playlistList->selectionModel()->selectedIndexes();
    const qint64 id = sel.isEmpty() ? 0 : m_library->listModel()->idAt(sel.first().row());
    PlaylistModel* model = m_library->playlist(id);
    if (model != m_view->playlist()) {
        m_view->setPlaylist(model);
        if (m_view->selectionModel()) {
            connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
                    &PlaylistDialog::updateUi, Qt::UniqueConnection);
        }
        if (model) {
            connect(model, &PlaylistModel::contentsChanged, this, &PlaylistDialog::updateUi, Qt::UniqueConnection);
            connect(model, &PlaylistModel::currentChanged, this, &PlaylistDialog::updateUi, Qt::UniqueConnection);
        }
    }
    m_binding = true;
    if (model) {
        const RotationSettings r = model->rotation();
        m_unlockCheck->setChecked(r.onUnlock);
        m_startCheck->setChecked(r.onWindowsStart);
        m_intervalCheck->setChecked(r.onInterval);
        int ci = m_intervalCombo->findData(r.intervalMinutes);
        if (ci < 0) {
            m_intervalCombo->addItem(countText(r.intervalMinutes, tr("%1 minute"), tr("%1 minutes")), r.intervalMinutes);
            ci = m_intervalCombo->count() - 1;
        }
        m_intervalCombo->setCurrentIndex(ci);
        m_activeCheck->setChecked(model->isActive());
    }
    m_binding = false;
    if (model) {
        model->refreshAvailability(); // a drive may have been reconnected/removed
    }
    m_view->viewport()->update();
    updateUi();
}

void PlaylistDialog::onRotationEdited() {
    if (m_binding || !current()) {
        return;
    }
    RotationSettings r;
    r.onUnlock = m_unlockCheck->isChecked();
    r.onWindowsStart = m_startCheck->isChecked();
    r.onInterval = m_intervalCheck->isChecked();
    r.intervalMinutes = m_intervalCombo->currentData().toInt();
    m_library->setRotation(current()->id(), r);
    m_intervalCombo->setEnabled(r.onInterval);
}

void PlaylistDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    setWindowIcon(QIcon(playlistIconPath()));
    bindSelectedPlaylist();
}

int PlaylistDialog::selectedRow() const {
    if (!m_view->selectionModel()) {
        return -1;
    }
    // The selection itself, not the view's "current index": accessibility
    // clients (UI Automation SelectionItem.Select, i.e. screen readers)
    // select an item without moving the current index.
    const QModelIndexList selected = m_view->selectionModel()->selectedIndexes();
    return selected.isEmpty() ? -1 : selected.first().row();
}

void PlaylistDialog::selectRow(int row) {
    PlaylistModel* m = current();
    if (!m || row < 0 || row >= m->count()) {
        return;
    }
    const QModelIndex idx = m->index(row);
    m_view->setCurrentIndex(idx);
    m_view->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect);
    m_view->scrollTo(idx);
    updateUi();
}

void PlaylistDialog::showNotice(const QString& text) {
    m_noticeLabel->setText(text);
    m_noticeLabel->setVisible(!text.isEmpty());
}

void PlaylistDialog::onCreatePlaylist(int type) {
    const PlaylistType t = static_cast<PlaylistType>(type);
    bool ok = false;
    const QString name = QInputDialog::getText(this, t == PlaylistType::Video ? tr("New Video Playlist") : tr("New Image Playlist"),
                                               tr("Playlist name:"), QLineEdit::Normal,
                                               t == PlaylistType::Video ? tr("My Videos") : tr("My Images"), &ok);
    if (!ok) {
        return;
    }
    const qint64 id = m_library->createPlaylist(name, t);
    if (id > 0) {
        selectPlaylist(id);
        showNotice(t == PlaylistType::Video ? tr("Video playlist created - add some videos.")
                                            : tr("Image playlist created - add some images."));
    }
}

void PlaylistDialog::startInlineRename() {
    const QModelIndexList sel = m_playlistList->selectionModel()->selectedIndexes();
    if (sel.isEmpty()) {
        return;
    }
    m_playlistList->setCurrentIndex(sel.first());
    m_playlistList->setFocus();
    m_playlistList->edit(sel.first());
}

void PlaylistDialog::showPlaylistMenu(const QPoint& pos) {
    QModelIndex idx = m_playlistList->indexAt(pos);
    if (!idx.isValid()) {
        // Keyboard (Menu key / Shift+F10): act on the selection.
        const QModelIndexList sel = m_playlistList->selectionModel()->selectedIndexes();
        if (sel.isEmpty()) {
            return;
        }
        idx = sel.first();
    }
    m_playlistList->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect);
    m_playlistList->setCurrentIndex(idx);
    QMenu menu(this);
    QAction* rename = menu.addAction(tr("Rename"));
    rename->setShortcut(QKeySequence(Qt::Key_F2));
    QAction* del = menu.addAction(tr("Delete"));
    del->setShortcut(QKeySequence(QKeySequence::Delete));
    QAction* chosen = menu.exec(m_playlistList->viewport()->mapToGlobal(
        idx.isValid() && m_playlistList->indexAt(pos).isValid() ? pos : m_playlistList->visualRect(idx).center()));
    if (chosen == rename) {
        startInlineRename();
    } else if (chosen == del) {
        onDeletePlaylist();
    }
}

bool PlaylistDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_playlistList && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        const bool plain = (key->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
        if (plain && (key->key() == Qt::Key_Delete || key->key() == Qt::Key_D)) {
            onDeletePlaylist();
            return true; // also keeps 'D' from triggering the list's type-ahead search
        }
    }
    return QDialog::eventFilter(watched, event);
}

void PlaylistDialog::showItemMenu(const QPoint& pos) {
    PlaylistModel* m = current();
    if (!m) {
        return;
    }
    QModelIndex idx = m_view->indexAt(pos);
    const bool fromMouse = idx.isValid();
    if (!idx.isValid()) {
        const int row = selectedRow(); // Menu key / Shift+F10
        if (row < 0) {
            return;
        }
        idx = m->index(row);
    }
    const int row = idx.row();
    selectRow(row); // the menu always acts on the item it was opened for
    const bool available = m->isAvailable(row);

    QMenu menu(this);
    QAction* showNow = nullptr;
    QAction* findFile = nullptr;
    QAction* moveLeft = nullptr;
    QAction* moveRight = nullptr;
    // Only actions that make sense for this item right now are listed.
    if (available && !(row == m->currentIndex() && m->isActive())) {
        showNow = menu.addAction(tr("Show Now"));
        showNow->setShortcut(QKeySequence(Qt::Key_Return));
    }
    if (!available) {
        findFile = menu.addAction(tr("Find File…"));
    }
    if (row > 0) {
        moveLeft = menu.addAction(tr("Move Left"));
        moveLeft->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left));
    }
    if (row < m->count() - 1) {
        moveRight = menu.addAction(tr("Move Right"));
        moveRight->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Right));
    }
    if (!menu.isEmpty()) {
        menu.addSeparator();
    }
    QAction* remove = menu.addAction(tr("Remove"));
    remove->setShortcut(QKeySequence(QKeySequence::Delete));
    remove->setToolTip(tr("Remove from this playlist. The file itself is not deleted."));

    const QPoint at = fromMouse ? pos : m_view->visualRect(idx).center();
    QAction* chosen = menu.exec(m_view->viewport()->mapToGlobal(at));
    if (!chosen) {
        return;
    }
    if (chosen == showNow) {
        onShowNow();
    } else if (chosen == findFile) {
        onFindFile(row);
    } else if (chosen == moveLeft) {
        moveRow(row, -1);
    } else if (chosen == moveRight) {
        moveRow(row, +1);
    } else if (chosen == remove) {
        removeRow(row);
    }
}

void PlaylistDialog::onFindFile(int row) {
    PlaylistModel* m = current();
    if (!m || row < 0 || row >= m->count() || m_recoveryRunning) {
        return; // one search at a time
    }
    m->refreshAvailability();
    if (m->isAvailable(row)) {
        showNotice(tr("\"%1\" is available again - nothing to find.").arg(QFileInfo(m->pathAt(row)).fileName()));
        updateUi();
        return;
    }
    m_recoveryRunning = true;
    const qint64 playlistId = m->id();
    const qint64 itemId = m->itemIdAt(row);
    const qint64 mediaId = m->mediaIdAt(row);
    const QString path = m->pathAt(row);
    auto* dialog = new MediaRecoveryDialog(path, m->factsAt(row), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [this, dialog, playlistId, itemId, mediaId, path](int result) {
        m_recoveryRunning = false;
        if (result == MediaRecoveryDialog::Found) {
            const QString found = dialog->chosenPath();
            if (m_library->relocateMedia(mediaId, found)) {
                showNotice(tr("Found \"%1\" at %2. Its location was updated.")
                               .arg(QFileInfo(found).fileName(), QDir::toNativeSeparators(QFileInfo(found).absolutePath())));
                if (PlaylistModel* pm = m_library->playlist(playlistId)) {
                    for (int r = 0; r < pm->count(); ++r) {
                        if (pm->mediaIdAt(r) == mediaId || QFileInfo(pm->pathAt(r)) == QFileInfo(found)) {
                            selectRow(r);
                            break;
                        }
                    }
                }
            }
        } else if (result == MediaRecoveryDialog::NotFound) {
            offerRemovalAfterFailedSearch(itemId, path);
        }
        updateUi();
    });
    dialog->open(); // window-modal, asynchronous: the search runs on a worker thread
}

void PlaylistDialog::offerRemovalAfterFailedSearch(qint64 itemId, const QString& path) {
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("File could not be found"));
    box.setText(tr("Motiva could not locate this media file anywhere on the available drives."));
    box.setInformativeText(
        tr("%1\n\n"
           "Remove (recommended) takes this entry out of the playlist. No file is deleted - Motiva only "
           "forgets the location it had saved.\n\n"
           "Cancel (not recommended) keeps the missing entry so it can be recovered later, for example after "
           "reconnecting a drive.")
            .arg(QDir::toNativeSeparators(path)));
    QPushButton* remove = box.addButton(tr("Remove"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(remove);
    box.exec();
    if (box.clickedButton() != remove) {
        showNotice(tr("Kept the missing entry. You can try Find File again later."));
        return;
    }
    PlaylistModel* m = current();
    const int row = m ? m->rowOfItem(itemId) : -1;
    if (row >= 0) {
        removeRow(row);
    }
}

void PlaylistDialog::updateMissingBanner(int row) {
    PlaylistModel* m = current();
    const int missing = m ? m->count() - m->availableCount() : 0;
    if (!m || missing == 0) {
        m_missingBanner->setVisible(false);
        return;
    }
    const Theme::ThemePalette& pal = Theme::themePalette(Theme::currentTheme());
    m_missingBanner->setStyleSheet(QStringLiteral("QFrame#missingBanner { background: %1; border: 1px solid %2;"
                                                  " border-left: 3px solid %2; border-radius: %3px; }")
                                       .arg(pal.panelBg.name(), QLatin1String(Theme::kStatusWarning))
                                       .arg(qMin(8, cardRadius())));
    m_missingTitle->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(Theme::kStatusWarning)));
    const bool selectedMissing = row >= 0 && !m->isAvailable(row);
    if (selectedMissing) {
        const QString name = QFileInfo(m->pathAt(row)).fileName();
        m_missingTitle->setText(tr("File not found"));
        m_missingText->setText(tr("Motiva can't find \"%1\" at its saved location.").arg(name));
        m_missingText->setToolTip(QDir::toNativeSeparators(m->pathAt(row)));
        m_missingRemoveButton->setVisible(true);
    } else {
        m_missingTitle->setText(missing == 1 ? tr("1 file not found") : tr("%1 files not found").arg(missing));
        m_missingText->setText(tr("Motiva can't find some of this playlist's media at its saved location. "
                                  "Find File searches your drives for it."));
        m_missingText->setToolTip(QString());
        m_missingRemoveButton->setVisible(false);
    }
    m_findFileButton->setEnabled(!m_recoveryRunning);
    m_missingBanner->setVisible(true);
}

void PlaylistDialog::onDeletePlaylist() {
    PlaylistModel* m = current();
    if (!m) {
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Delete Playlist"),
        tr("Delete the playlist \"%1\"?\n\nOnly the playlist is removed - your %2 files are not deleted.")
            .arg(m->name(), m->isVideo() ? tr("video") : tr("image")),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }
    const QString name = m->name();
    const qint64 id = m->id();
    const int row = m_library->listModel()->rowOf(id);
    m_view->setPlaylist(nullptr); // the model is destroyed by deletePlaylist
    if (m_library->deletePlaylist(id)) {
        showNotice(tr("Deleted playlist \"%1\". No files were deleted.").arg(name));
        selectPlaylist(m_library->listModel()->idAt(qMin(row, m_library->playlistCount() - 1)));
    } else {
        bindSelectedPlaylist();
    }
}

void PlaylistDialog::onImport() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Import Motiva Library"), QString(),
                                                      tr("Motiva library (*.mtv);;All Files (*)"));
    if (path.isEmpty()) {
        return;
    }
    const int count = m_library->importLibrary(path);
    if (count >= 0) {
        showNotice(countText(count, tr("Imported %1 playlist."), tr("Imported %1 playlists.")) +
                   tr(" Media that isn't on this PC shows as \"File not found\"."));
        updateUi();
    }
}

void PlaylistDialog::onExport() {
    const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                  .filePath(QStringLiteral("MotivaLibrary.mtv"));
    QString path = QFileDialog::getSaveFileName(this, tr("Export Motiva Library"), suggested,
                                                tr("Motiva library (*.mtv)"));
    if (path.isEmpty()) {
        return;
    }
    if (!path.endsWith(QLatin1String(".mtv"), Qt::CaseInsensitive)) {
        path += QLatin1String(".mtv");
    }
    if (m_library->exportLibrary(path)) {
        showNotice(tr("Exported to %1. It contains your playlists and file locations, not the media files.")
                       .arg(QDir::toNativeSeparators(path)));
    }
}

void PlaylistDialog::addFilesTo(qint64 playlistId, const QStringList& paths, int insertRow) {
    PlaylistModel* m = m_library->playlist(playlistId);
    if (!m) {
        return;
    }
    if (m != current()) {
        selectPlaylist(playlistId);
    }
    const PlaylistModel::AddResult result = m->addFiles(paths, insertRow);
    if (result.failed) {
        return; // errorOccurred already explained it
    }
    const bool video = m->isVideo();
    QStringList parts;
    if (result.added > 0) {
        parts << countText(result.added, video ? tr("Added %1 video.") : tr("Added %1 image."),
                           video ? tr("Added %1 videos.") : tr("Added %1 images."));
    }
    if (result.duplicates > 0) {
        parts << countText(result.duplicates, tr("%1 was already in the playlist."),
                           tr("%1 were already in the playlist."));
    }
    if (result.unsupported > 0) {
        parts << countText(result.unsupported,
                           video ? tr("%1 file skipped - not a supported video.") : tr("%1 file skipped - not a supported image."),
                           video ? tr("%1 files skipped - not supported videos.") : tr("%1 files skipped - not supported images."));
    }
    showNotice(parts.join(QLatin1Char(' ')));
    if (result.added > 0) {
        const int first = (insertRow < 0 || insertRow > m->count() - result.added) ? m->count() - result.added : insertRow;
        selectRow(first);
    }
    updateUi();
}

void PlaylistDialog::onAddMedia() {
    PlaylistModel* m = current();
    if (!m) {
        return;
    }
    QStringList exts = m->isVideo() ? VideoPlayer::supportedVideoExtensions().values()
                                    : VideoPlayer::supportedStaticImageExtensions().values();
    exts.sort(Qt::CaseInsensitive);
    QStringList patterns;
    for (const QString& ext : exts) {
        patterns << QStringLiteral("*.%1").arg(ext);
    }
    const QString filter = (m->isVideo() ? tr("Videos (%1)") : tr("Images (%1)")).arg(patterns.join(QLatin1Char(' '))) +
                           QStringLiteral(";;") + tr("All Files (*)");
    const QStringList files = QFileDialog::getOpenFileNames(
        this, m->isVideo() ? tr("Add Videos to \"%1\"").arg(m->name()) : tr("Add Images to \"%1\"").arg(m->name()),
        QString(), filter);
    if (!files.isEmpty()) {
        addFilesTo(m->id(), files);
    }
}

void PlaylistDialog::onRemove() {
    removeRow(selectedRow());
}

void PlaylistDialog::removeRow(int row) {
    PlaylistModel* m = current();
    if (!m || row < 0 || row >= m->count()) {
        return;
    }
    const QString name = QFileInfo(m->pathAt(row)).fileName();
    // Removes this playlist's reference only. The media record is cleaned up
    // by the library only if no other playlist uses it; files are never touched.
    if (m->removeAt(row)) {
        showNotice(tr("Removed \"%1\" from the playlist. The file itself was not touched.").arg(name));
        if (m->count() == 0 && m->isActive()) {
            m_library->setActive(0); // nothing left to show
        }
        selectRow(qMin(row, m->count() - 1));
    }
    updateUi();
}

void PlaylistDialog::onClear() {
    PlaylistModel* m = current();
    if (!m || m->count() == 0) {
        return;
    }
    const QString question = (m->count() == 1)
        ? tr("Remove the only item from \"%1\"?\n\nThe file is not deleted.").arg(m->name())
        : tr("Remove all %1 items from \"%2\"?\n\nYour files are not deleted.").arg(m->count()).arg(m->name());
    if (QMessageBox::question(this, tr("Clear Playlist"), question, QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    if (m->clear()) {
        if (m->isActive()) {
            m_library->setActive(0); // nothing left to show
        }
        showNotice(tr("Playlist cleared. No files were deleted."));
    }
    updateUi();
}

void PlaylistDialog::onMove(int delta) {
    moveRow(selectedRow(), delta);
}

void PlaylistDialog::moveRow(int row, int delta) {
    PlaylistModel* m = current();
    const int to = row + delta;
    if (!m || row < 0 || to < 0 || to >= m->count()) {
        return;
    }
    if (m->move(row, to)) {
        selectRow(to);
    }
}

void PlaylistDialog::onShowNow() {
    PlaylistModel* m = current();
    const int row = selectedRow();
    if (!m || row < 0) {
        return;
    }
    // setCurrentIndex re-checks the file and refuses a missing one.
    if (!m->setCurrentIndex(row)) {
        showNotice(tr("That file can't be found on disk. Reconnect its drive or remove it from the playlist."));
        return;
    }
    // Choosing an item to show is using this playlist - make it the active
    // one so the item is applied (and rotation continues from it).
    if (!m->isActive()) {
        m_library->setActive(m->id());
    }
    showNotice(QString());
}

void PlaylistDialog::setActiveOnDesktop(bool onDesktop) {
    m_onDesktop = onDesktop;
    updateUi();
}

void PlaylistDialog::updateUi() {
    if (!m_summaryLabel) {
        return;
    }
    const bool hasPlaylists = m_library->playlistCount() > 0;
    PlaylistModel* m = current();
    m_editorStack->setCurrentIndex(m ? 1 : 0);
    m_emptyLabel->setText(hasPlaylists ? tr("Select a playlist on the left.")
                                       : tr("Create a playlist with \"+ New\" to get started.\n\n"
                                            "Image playlists change image on unlock, at Windows start or on a timer.\n"
                                            "Video playlists play their videos one after another."));
    if (!m) {
        m_applyButton->setEnabled(false);
        m_applyButton->setText(tr("Set as Wallpaper"));
        return;
    }

    const int count = m->count();
    const int available = m->availableCount();
    const int missing = count - available;
    const int row = selectedRow();
    const int currentRow = m->currentIndex();
    const bool video = m->isVideo();

    m_nameLabel->setText(m->name());
    QString summary = countText(count, video ? tr("%1 video") : tr("%1 image"), video ? tr("%1 videos") : tr("%1 images"));
    if (count > 0) {
        if (m->isActive()) {
            summary += QStringLiteral("  •  ") + tr("Active - item %1 of %2").arg(currentRow + 1).arg(count);
            summary += m_onDesktop ? tr("  •  On the desktop") : tr("  •  Not on the desktop yet");
        } else if (currentRow >= 0) {
            summary += QStringLiteral("  •  ") + tr("Not active - resumes at item %1").arg(currentRow + 1);
        }
    }
    if (missing > 0) {
        summary += QStringLiteral("  •  ") + countText(missing, tr("%1 file not found"), tr("%1 files not found"));
    }
    m_summaryLabel->setText(summary);
    m_binding = true;
    m_activeCheck->setChecked(m->isActive());
    m_binding = false;

    m_addButton->setText(video ? tr("+  Add Videos") : tr("+  Add Images"));
    m_addButton->setToolTip(video ? tr("Add one or more video files to the end of the playlist")
                                  : tr("Add one or more image files to the end of the playlist"));
    m_settingsStack->setCurrentIndex(video ? 1 : 0);
    m_intervalCombo->setEnabled(m_intervalCheck->isChecked());

    m_showNowButton->setEnabled(row >= 0 && m->isAvailable(row) && (row != currentRow || !m->isActive()));
    updateMissingBanner(row);
    m_clearButton->setEnabled(count > 0);

    const bool onDesktop = m_onDesktop && m->isActive();
    m_applyButton->setText(onDesktop ? tr("This playlist is on the desktop") : tr("Set as Wallpaper"));
    m_applyButton->setEnabled(!onDesktop && available > 0);
}
