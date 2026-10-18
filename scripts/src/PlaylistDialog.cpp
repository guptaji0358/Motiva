#include "PlaylistDialog.h"
#include "NotificationManager.h"
#include "BackupManager.h"
#include "MediaRecovery.h"
#include "PlaylistLibrary.h"
#include "PlaylistModel.h"
#include "CategoryFilterDialog.h"
#include "CategoryTreePicker.h"
#include "CategoryItemPickerDialog.h"
#include "FlowDialog.h"
#include "GlUi.h"
#include "DialogSizing.h"
#include "Theme.h"
#include "SettingsManager.h"
#include "VideoPlayer.h"

#include <QAbstractProxyModel>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QLinearGradient>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QCollator>
#include <QDialog>
#include <QComboBox>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QAction>
#include "IconButton.h"
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int kCardWidth = 184;
constexpr int kCardHeight = 158;
constexpr int kCardSpacing = 12;

// Orders the cards by file name (natural, case-insensitive: img-2 before
// img-10). Ties keep playlist order. Sorting direction comes from the view.
class ArrangeProxy : public QSortFilterProxyModel {
public:
    explicit ArrangeProxy(QObject* parent) : QSortFilterProxyModel(parent) {
        m_collator.setNumericMode(true);
        m_collator.setCaseSensitivity(Qt::CaseInsensitive);
        setDynamicSortFilter(true);
    }

protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override {
        const int c = m_collator.compare(QFileInfo(left.data(PlaylistModel::PathRole).toString()).fileName(),
                                         QFileInfo(right.data(PlaylistModel::PathRole).toString()).fileName());
        return c != 0 ? c < 0 : left.row() < right.row();
    }

private:
    QCollator m_collator;
};

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

// A small label chip used on cards and rows (glass-dark by default).
void drawChip(QPainter* p, const QRectF& rect, const QColor& fill, const QString& text, qreal radius,
              const QColor& textColor = Qt::white) {
    p->setPen(Qt::NoPen);
    p->setBrush(fill);
    p->drawRoundedRect(rect, radius, radius);
    p->setPen(textColor);
    p->drawText(rect, Qt::AlignCenter, text);
}

void drawGradientChip(QPainter* p, const QRectF& rect, const GlUi::Style& s, const QString& text) {
    QLinearGradient g(rect.topLeft(), rect.topRight());
    g.setColorAt(0, s.accent);
    g.setColorAt(1, s.accent2);
    p->setPen(Qt::NoPen);
    p->setBrush(g);
    const qreal r = qMin<qreal>(rect.height() / 2, 4 + 6 * s.radius);
    p->drawRoundedRect(rect, r, r);
    const QColor c = Theme::contrastRatio(Qt::white, s.accent) >= Theme::contrastRatio(Qt::black, s.accent)
                         ? QColor(Qt::white) : QColor(8, 8, 12);
    p->setPen(c);
    p->drawText(rect, Qt::AlignCenter, text);
}

// One playlist item as a glass card: thumbnail (cropped to fill, on a
// near-black media well), order number, a video marker for video items,
// "Now showing" on the active playlist's current item, and an explicit
// "File not found" state. Colors come from the active Theme at paint time
// (GlUi::Style), so a live theme switch restyles it.
class PlaylistCardDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(kCardWidth, kCardHeight);
    }

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const GlUi::Style s = GlUi::Style::current();
        const auto* model = qobject_cast<const PlaylistModel*>(index.model());
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const bool available = index.data(PlaylistModel::AvailableRole).toBool();
        const bool isCurrent = index.data(PlaylistModel::IsCurrentRole).toBool();
        const bool isVideo = index.data(PlaylistModel::IsVideoRole).toBool();
        bool playlistActive = model && model->isActive();
        if (const auto* proxy = qobject_cast<const QAbstractProxyModel*>(index.model())) {
            if (const auto* src = qobject_cast<const PlaylistModel*>(proxy->sourceModel())) {
                playlistActive = src->isActive();
            }
        }
        const qreal radius = 16 * s.radius + 2;

        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setRenderHint(QPainter::SmoothPixmapTransform, true);

        QRectF card = QRectF(option.rect).adjusted(4, 4, -4, -4);
        if (hovered || selected) {
            card.translate(0, -2); // hover lift
        }
        // Soft shadow + glass body.
        p->setPen(Qt::NoPen);
        for (int i = 0; i < 3; ++i) {
            p->setBrush(QColor(0, 0, 0, (hovered || selected ? 38 : 22) - i * 7));
            p->drawRoundedRect(card.translated(0, 2 + i * 2).adjusted(-i, -i, i, i), radius + i, radius + i);
        }
        p->setBrush(s.dark ? QColor(255, 255, 255, 22) : QColor(255, 255, 255, 170));
        p->drawRoundedRect(card, radius, radius);

        const QRectF thumb = card.adjusted(8, 8, -8, -36);
        const QPainterPath thumbPath = roundedPath(thumb, qMax(2.0, radius - 6));
        p->fillPath(thumbPath, QColor(Theme::kPreviewSurfaceBg));
        if (available) {
            const QImage image = index.data(PlaylistModel::ThumbnailRole).value<QImage>();
            if (!image.isNull()) {
                p->save();
                p->setClipPath(thumbPath, Qt::IntersectClip);
                const QSizeF scaled = QSizeF(image.size()).scaled(thumb.size(), Qt::KeepAspectRatioByExpanding);
                p->drawImage(QRectF(thumb.center().x() - scaled.width() / 2, thumb.center().y() - scaled.height() / 2,
                                    scaled.width(), scaled.height()),
                             image);
                p->restore();
            } else {
                p->setFont(option.font);
                p->setPen(QColor(Theme::kPreviewText));
                const bool pending = index.data(PlaylistModel::ThumbnailPendingRole).toBool();
                const QString message = pending ? PlaylistDialog::tr("Loading…") : PlaylistDialog::tr("No preview");
                if (isVideo && !pending) {
                    const int iconSize = 14;
                    const int gap = 6;
                    const int textW = p->fontMetrics().horizontalAdvance(message);
                    const qreal left = thumb.center().x() - (iconSize + gap + textW) / 2.0;
                    p->drawPixmap(QPointF(left, thumb.center().y() - iconSize / 2.0),
                                  Theme::tintedIcon(QStringLiteral(":/playlist/video-badge.svg"),
                                                    QColor(Theme::kPreviewText), iconSize));
                    p->drawText(QRectF(left + iconSize + gap, thumb.top(), textW + 2, thumb.height()),
                                Qt::AlignLeft | Qt::AlignVCenter, message);
                } else {
                    p->drawText(thumb, Qt::AlignCenter, message);
                }
            }
        } else {
            p->setPen(QPen(s.warn, 1.4, Qt::DashLine));
            p->setBrush(Qt::NoBrush);
            p->drawPath(roundedPath(thumb.adjusted(1, 1, -1, -1), qMax(2.0, radius - 6)));
            QFont f = option.font;
            f.setBold(true);
            p->setFont(f);
            p->setPen(s.warn);
            p->drawText(thumb, Qt::AlignCenter, PlaylistDialog::tr("File not found"));
        }

        QFont badgeFont = option.font;
        badgeFont.setBold(true);
        badgeFont.setPointSizeF(qMax(7.0, badgeFont.pointSizeF() - 1));
        p->setFont(badgeFont);
        const QFontMetrics fm = p->fontMetrics();
        // Position in the playlist (what playback follows), not in the arranged view.
        int sequence = index.row();
        if (const auto* proxy = qobject_cast<const QAbstractProxyModel*>(index.model())) {
            sequence = proxy->mapToSource(index).row();
        }
        const QString number = QStringLiteral("%1").arg(sequence + 1, 2, 10, QLatin1Char('0'));
        const qreal chipR = 3 + 6 * s.radius;
        drawChip(p, QRectF(thumb.left() + 6, thumb.top() + 6, fm.horizontalAdvance(number) + 12, fm.height() + 4),
                 QColor(0, 0, 0, 165), number, chipR);
        if (isVideo && available) {
            const QString tag = PlaylistDialog::tr("VIDEO");
            const int iconSize = fm.height() - 2;
            const QRectF tagRect(thumb.left() + 6, thumb.bottom() - fm.height() - 10,
                                 fm.horizontalAdvance(tag) + iconSize + 18, fm.height() + 4);
            drawChip(p, tagRect, QColor(0, 0, 0, 165), QString(), chipR);
            p->drawPixmap(QPointF(tagRect.left() + 6, tagRect.center().y() - iconSize / 2.0),
                          Theme::tintedIcon(QStringLiteral(":/playlist/video-badge.svg"), Qt::white, iconSize));
            p->setPen(Qt::white);
            p->drawText(QRectF(tagRect.left() + 6 + iconSize + 4, tagRect.top(), fm.horizontalAdvance(tag) + 2,
                               tagRect.height()),
                        Qt::AlignLeft | Qt::AlignVCenter, tag);
        }
        if (isCurrent) {
            const QString label = playlistActive ? PlaylistDialog::tr("Now showing") : PlaylistDialog::tr("Resumes here");
            const qreal w = fm.horizontalAdvance(label) + 14;
            const QRectF chip(thumb.right() - 6 - w, thumb.top() + 6, w, fm.height() + 4);
            if (playlistActive) {
                drawGradientChip(p, chip, s, label);
            } else {
                drawChip(p, chip, QColor(0, 0, 0, 165), label, chipR);
            }
        }

        p->setFont(option.font);
        const QRectF nameRect(card.left() + 10, thumb.bottom() + 6, card.width() - 20, 22);
        p->setPen(available ? s.text : s.textDim);
        p->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                    p->fontMetrics().elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle,
                                                int(nameRect.width())));

        // Rim: gradient + glow when selected, accent when it is what is on the desktop.
        p->setBrush(Qt::NoBrush);
        if (selected) {
            QLinearGradient g(card.topLeft(), card.bottomRight());
            g.setColorAt(0, s.accent);
            g.setColorAt(1, s.accent2);
            p->setPen(QPen(QBrush(g), 2.2));
            p->drawRoundedRect(card.adjusted(1.1, 1.1, -1.1, -1.1), radius, radius);
            for (int i = 1; i <= 4; ++i) {
                QColor k = s.accent2;
                k.setAlphaF(0.22 * (1.0 - i / 5.0));
                p->setPen(QPen(k, 2));
                p->drawRoundedRect(card.adjusted(-i * 1.5, -i * 1.5, i * 1.5, i * 1.5), radius + i * 1.5, radius + i * 1.5);
            }
        } else {
            QColor outline = (isCurrent && playlistActive) ? s.accent : hovered ? GlUi::Style::current().accent2 : s.glassEdge;
            if (hovered && !(isCurrent && playlistActive)) outline.setAlphaF(0.8);
            p->setPen(QPen(outline, (isCurrent && playlistActive) ? 1.8 : 1.1));
            p->drawRoundedRect(card.adjusted(0.6, 0.6, -0.6, -0.6), radius, radius);
        }
        p->restore();
    }
};

// A "My Playlists" row: name, kind + item count, and an "Active" marker on
// the playlist that controls the wallpaper.
class PlaylistRowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return QSize(200, 58); }

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const GlUi::Style s = GlUi::Style::current();
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const bool active = index.data(PlaylistListModel::IsActiveRole).toBool();
        const int type = index.data(PlaylistListModel::TypeRole).toInt();
        const bool video = type == static_cast<int>(PlaylistType::Video);
        const bool mixed = type == static_cast<int>(PlaylistType::Mixed);
        const int count = index.data(PlaylistListModel::CountRole).toInt();
        const qreal radius = 14 * s.radius + 3;

        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        const QRectF row = QRectF(option.rect).adjusted(2, 3, -2, -3);
        QColor titleColor = s.text, subColor = s.textDim;
        if (selected) {
            QLinearGradient g(row.topLeft(), row.topRight());
            g.setColorAt(0, s.accent);
            g.setColorAt(1, GlUi::Style::current().accent2);
            p->setPen(Qt::NoPen);
            p->setBrush(g);
            p->drawRoundedRect(row, radius, radius);
            const QColor on = Theme::contrastRatio(Qt::white, s.accent) >= Theme::contrastRatio(Qt::black, s.accent)
                                  ? QColor(Qt::white) : QColor(8, 8, 12);
            titleColor = on;
            subColor = on;
            subColor.setAlphaF(0.78);
        } else if (hovered) {
            QColor c = s.accent;
            c.setAlphaF(0.16);
            p->setPen(Qt::NoPen);
            p->setBrush(c);
            p->drawRoundedRect(row, radius, radius);
        }

        QFont bold = option.font;
        bold.setBold(true);
        const QFontMetrics boldFm(bold);
        const QFontMetrics fm(option.font);
        const QString activeLabel = PlaylistDialog::tr("Active");
        const qreal pillW = active ? fm.horizontalAdvance(activeLabel) + 16 : 0;
        const QRectF nameRect(row.left() + 14, row.top() + 7, row.width() - 28 - pillW, boldFm.height());
        p->setFont(bold);
        p->setPen(titleColor);
        p->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                    boldFm.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, int(nameRect.width())));

        p->setFont(option.font);
        p->setPen(subColor);
        const QString subtitle = (mixed ? PlaylistDialog::tr("Mixed") : video ? PlaylistDialog::tr("Videos") : PlaylistDialog::tr("Images")) +
            QStringLiteral("  •  ") + QString::number(count);
        p->drawText(QRectF(row.left() + 14, nameRect.bottom() + 2, row.width() - 28, fm.height()),
                    Qt::AlignLeft | Qt::AlignVCenter, subtitle);

        if (active) {
            const QRectF chip(row.right() - 10 - pillW, row.top() + 8, pillW, fm.height() + 4);
            if (selected) {
                drawChip(p, chip, QColor(0, 0, 0, 90), activeLabel, 4 + 6 * s.radius);
            } else {
                drawGradientChip(p, chip, s, activeLabel);
            }
        }
        p->restore();
    }
};

} // namespace

// ---------------------------------------------------------------- PlaylistView

PlaylistView::PlaylistView(GlCanvas* canvas, QObject* parent) : QObject(parent) {
    m_proxy = new ArrangeProxy(this);
    m_item = new GlItemView(canvas);
    m_item->setModel(m_proxy);
    m_item->setDelegate(new PlaylistCardDelegate(this));
    m_item->setMode(GlItemView::Mode::Grid, QSize(kCardWidth, kCardHeight), kCardSpacing / 2);
    m_item->setReorderEnabled(true);
    m_item->setAccessibleName(tr("Playlist items"));
    m_item->setKeyFilter([this](QKeyEvent* e) { return handleKey(e); });
    connect(m_item, &GlItemView::reorderRequested, this, [this](int from, int pos) {
        if (isArranged()) {
            return; // dragging reorders the playlist - not while the cards are arranged by name
        }
        const int src = sourceRow(from);
        const int to = (pos > src) ? pos - 1 : pos;
        if (src >= 0 && to != src) {
            emit moveRequested(src, to);
        }
    });
    connect(m_item, &GlItemView::doubleClicked, this, [this](int vr) { emit itemDoubleClicked(sourceRow(vr)); });
    connect(m_item, &GlItemView::activated, this, [this](int vr) { emit showNowRequested(sourceRow(vr)); });
    connect(m_item, &GlItemView::currentRowChanged, this, [this](int) { emit selectionChanged(); });
    connect(m_item, &GlItemView::contextMenuRequested, this, [this](const QPointF&, const QPoint& global, int vr) {
        emit itemContextMenu(global, vr >= 0 ? sourceRow(vr) : -1);
    });
}

void PlaylistView::setPlaylist(PlaylistModel* playlist) {
    m_playlist = playlist;
    m_proxy->setSourceModel(playlist);
    const bool video = playlist && playlist->isVideo();
    const bool mixed = playlist && playlist->isMixed();
    m_item->setEmptyText(tr("This playlist is empty"),
                         mixed ? tr("Click \"Add Media\" or drop image and video files here.")
                         : video ? tr("Click \"Add Videos\" or drop video files here.")
                                 : tr("Click \"Add Images\" or drop image files here."));
    m_item->update();
}

void PlaylistView::setArrangement(Arrangement arrangement) {
    m_arrangement = arrangement;
    // Column -1 = no sorting: the proxy mirrors the playlist's own order.
    if (arrangement == Arrangement::None) {
        m_proxy->sort(-1);
    } else {
        m_proxy->sort(0, arrangement == Arrangement::AtoZ ? Qt::AscendingOrder : Qt::DescendingOrder);
    }
    m_item->update();
}

int PlaylistView::sourceRow(int viewRow) const {
    if (viewRow < 0) {
        return -1;
    }
    return m_proxy->mapToSource(m_proxy->index(viewRow, 0)).row();
}

int PlaylistView::viewRow(int sourceRow) const {
    return m_playlist ? m_proxy->mapFromSource(m_playlist->index(sourceRow)).row() : -1;
}

int PlaylistView::selectedSourceRow() const {
    return sourceRow(m_item->currentRow());
}

void PlaylistView::selectSourceRow(int row) {
    const int vr = viewRow(row);
    if (vr >= 0) {
        m_item->setCurrentRow(vr, true);
    }
}

void PlaylistView::clearSelection() {
    m_item->clearCurrent();
}

void PlaylistView::setSourceRowHidden(int row, bool hidden) {
    const int vr = viewRow(row);
    if (vr >= 0) {
        m_item->setRowHidden(vr, hidden);
    }
}

bool PlaylistView::isSourceRowHidden(int row) const {
    const int vr = viewRow(row);
    return vr >= 0 && m_item->isRowHidden(vr);
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

int PlaylistView::visibleNeighbour(int row, int step) const {
    if (!m_playlist) {
        return -1;
    }
    for (int r = row + step; r >= 0 && r < m_playlist->count(); r += step) {
        if (!isSourceRowHidden(r)) {
            return r;
        }
    }
    return -1;
}

bool PlaylistView::handleKey(QKeyEvent* event) {
    const int row = selectedSourceRow();
    if (row >= 0 && !isArranged() && (event->modifiers() & Qt::ControlModifier)) {
        // Past the visible neighbour - rows outside the current category
        // are hidden and keep their place.
        const int left = visibleNeighbour(row, -1);
        const int right = visibleNeighbour(row, +1);
        if (event->key() == Qt::Key_Left && left >= 0) {
            emit moveRequested(row, left);
            return true;
        }
        if (event->key() == Qt::Key_Right && right >= 0) {
            emit moveRequested(row, right);
            return true;
        }
        if (event->key() == Qt::Key_Home && row > 0) {
            emit moveRequested(row, 0);
            return true;
        }
        if (event->key() == Qt::Key_End && m_playlist && row < m_playlist->count() - 1) {
            emit moveRequested(row, m_playlist->count() - 1);
            return true;
        }
    }
    if (row >= 0 && event->key() == Qt::Key_Delete) {
        emit removeRequested(row);
        return true;
    }
    return false;
}

void PlaylistDialog::applyThemedIcons() {
    const QString v = Theme::iconVariant(Theme::currentTheme());
    const auto icon = [&v](const char* n) { return QIcon(QStringLiteral(":/playlist/%1/%2.svg").arg(v, QLatin1String(n))); };
    if (m_addButton) {
        m_addButton->setIcon(icon("add"));
    }
    // The New Playlist buttons use the shared (theme-independent) playlist-add badge.
    const QIcon createIcon(QStringLiteral(":/playlist/playlist-add.svg"));
    for (GlButton* b : {m_newImageButton, m_newVideoButton, m_newMixedButton}) {
        if (b) {
            b->setIcon(createIcon);
            b->setTintIcon(false); // the badge keeps its own colors
        }
    }
    if (m_buildCategoryButton) {
        m_buildCategoryButton->setIcon(icon("add"));
    }
    if (m_importButton) {
        m_importButton->setIcon(icon("import"));
    }
    if (m_exportButton) {
        m_exportButton->setIcon(icon("export"));
    }
    if (m_categoryMenuButton) {
        m_categoryMenuButton->setIcon(icon("more"));
    }
}

void PlaylistDialog::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ThemeChange) {
        applyThemedIcons();
        if (m_canvas) {
            m_canvas->requestAnimation();
        }
    }
}

// -------------------------------------------------------------- PlaylistDialog

PlaylistDialog::PlaylistDialog(PlaylistLibrary* library, QWidget* parent)
    : QWidget(parent, Qt::Window), m_library(library) {
    setObjectName(QStringLiteral("playlistsWindow"));
    setWindowFlag(Qt::FramelessWindowHint, true);
    setAcceptDrops(true);
    setWindowTitle(tr("Playlists"));
    setWindowIcon(QIcon(playlistIconPath()));
    buildUi();

    // Ctrl+Alt+N: New Image Playlist. Ctrl+Shift+N: New Video Playlist.
    // Each goes straight to naming the playlist - no type menu. (Ctrl+N is
    // deliberately not bound: it used to pop the "+ New" type menu.)
    // Window-scoped like MainWindow's Ctrl+V shortcut; ignored while a text
    // field has focus so typing is never hijacked.
    auto* newImageShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_N), this);
    connect(newImageShortcut, &QShortcut::activated, this, [this] {
        if (!isTextInputFocused()) {
            createPlaylist(false);
        }
    });
    auto* newMixedShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_M), this);
    connect(newMixedShortcut, &QShortcut::activated, this, [this] {
        if (!isTextInputFocused()) {
            createMixedPlaylist();
        }
    });
    auto* newVideoShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), this);
    connect(newVideoShortcut, &QShortcut::activated, this, [this] {
        if (!isTextInputFocused()) {
            createPlaylist(true);
        }
    });

    connect(m_library, &PlaylistLibrary::filtersChanged, this, [this](qint64 playlistId) {
        if (current() && current()->id() == playlistId) {
            reloadCategories(false);
        }
    });

    connect(m_library, &PlaylistLibrary::playlistsChanged, this, &PlaylistDialog::updateUi);
    // Cleanup & Reset closes the library and destroys every playlist model;
    // let go of ours first, then bind to whatever the reopened library has.
    connect(m_library, &PlaylistLibrary::aboutToReset, this, [this] { m_view->setPlaylist(nullptr); });
    connect(m_library, &PlaylistLibrary::resetFinished, this, [this] { selectPlaylist(m_library->selectedId()); });
    connect(m_library, &PlaylistLibrary::activeChanged, this, [this] { bindSelectedPlaylist(); });
    connect(m_library, &PlaylistLibrary::activeCurrentChanged, this, &PlaylistDialog::updateUi);
    connect(m_library, &PlaylistLibrary::sourceNotice, this, [this](const QString& message) { showNotice(message); });
    connect(m_library, &PlaylistLibrary::activeScopeChanged, this, &PlaylistDialog::updateUi);
    // The Playlists window shows toasts too; its header row is kept clear.
    NotificationManager::instance()->attachHost(this, /*topInset=*/64);
    connect(m_library, &PlaylistLibrary::errorOccurred, this, [this](const QString& message) {
        // Detail goes to the log; the user gets the reason in one line.
        qWarning() << "[Playlist] Library write failed:" << message;
        announce(int(Notification::Kind::Error), tr("Unable to save playlist changes"), message,
                 QStringLiteral("library-error"),
                 tr("The change could not be saved to the playlist library. %1").arg(message), /*desktop=*/true);
    });
    selectPlaylist(m_library->selectedId());
    DialogSizing::applyComfortableSize(this, QSize(1180, 780));
    setMinimumSize(920, 660);
}

void PlaylistDialog::buildUi() {
    auto* box = new QVBoxLayout(this);
    box->setContentsMargins(0, 0, 0, 0);
    m_canvas = new GlCanvas(this);
    box->addWidget(m_canvas);
    m_canvas->setAccessibleName(tr("Playlists"));
    m_canvas->setLayoutCallback([this](const QSizeF& s) { layoutGl(s); });

    // Panels first, so every control paints above them.
    m_titlePanel = new GlPanel(m_canvas);
    m_titlePanel->setBlock(GlPanel::Block::Accent);
    m_titlePanel->setRadiusPx(16);
    m_toolbarPanel = new GlPanel(m_canvas);
    m_toolbarPanel->setRadiusPx(16);
    m_sidebarPanel = new GlPanel(m_canvas);
    m_sidebarPanel->setBlock(GlPanel::Block::Accent2);
    m_editorPanel = new GlPanel(m_canvas);
    m_editorPanel->setRadiusPx(20);

    m_titleLabel = new GlLabel(m_canvas);
    m_titleLabel->setText(tr("Playlists"));
    m_titleLabel->setPointSize(14);
    m_titleLabel->setBold(true);
    m_minButton = new GlButton(m_canvas, GlButton::Kind::Minimize);
    m_minButton->setToolTip(tr("Minimize"));
    connect(m_minButton, &GlButton::clicked, this, &QWidget::showMinimized);
    m_maxButton = new GlButton(m_canvas, GlButton::Kind::Maximize);
    connect(m_maxButton, &GlButton::clicked, this, [this] {
        if (isMaximized()) showNormal(); else showMaximized();
    });
    m_closeButton = new GlButton(m_canvas, GlButton::Kind::Close);
    m_closeButton->setToolTip(tr("Close"));
    connect(m_closeButton, &GlButton::clicked, this, &QWidget::close);

    const auto makeButton = [this](GlButton::Kind kind, const QString& text, const QString& name, const QString& tip,
                                   const std::function<void()>& onClick) {
        auto* b = new GlButton(m_canvas, kind);
        b->setText(text);
        b->setAccessibleName(name);
        b->setToolTip(tip);
        connect(b, &GlButton::clicked, this, [onClick] { onClick(); });
        return b;
    };
    const auto makeLabel = [this](const QString& text, qreal pt = 10.0, bool dim = false) {
        auto* l = new GlLabel(m_canvas);
        l->setText(text);
        l->setPointSize(pt);
        if (dim) l->setTone(GlLabel::Tone::Dim);
        return l;
    };

    // Toolbar: creation and add actions (same handlers and shortcuts as before).
    m_newImageButton = makeButton(GlButton::Kind::Secondary, tr("Image Playlist"), tr("New Image Playlist"),
                                  tr("New image playlist (Ctrl+Alt+N)"), [this] { createPlaylist(false); });
    m_newVideoButton = makeButton(GlButton::Kind::Secondary, tr("Video Playlist"), tr("New Video Playlist"),
                                  tr("New video playlist (Ctrl+Shift+N)"), [this] { createPlaylist(true); });
    m_newMixedButton = makeButton(GlButton::Kind::Secondary, tr("Mixed Playlist"), tr("New Mixed Playlist"),
                                  tr("New mixed playlist - images and videos in one order (Ctrl+Alt+M)"),
                                  [this] { createMixedPlaylist(); });
    m_addButton = new GlButton(m_canvas, GlButton::Kind::Secondary); // text/tooltip follow the open playlist
    m_addButton->setAccessibleName(tr("Add Media"));
    connect(m_addButton, &GlButton::clicked, this, &PlaylistDialog::onAddMedia);
    m_importButton = makeButton(GlButton::Kind::Icon, QString(), tr("Import"),
                                tr("Import: add the playlists from another Motiva library (.mtv) to yours"),
                                [this] { onImport(); });
    m_exportButton = makeButton(GlButton::Kind::Icon, QString(), tr("Export"),
                                tr("Export: save a copy of your playlist library as a .mtv file. Media files "
                                   "are referenced, not copied."),
                                [this] { onExport(); });

    // Sidebar: My Playlists.
    m_sidebarHeading = new GlLabel(m_canvas);
    m_sidebarHeading->setText(tr("My Playlists"));
    m_sidebarHeading->setCaps(true);
    m_sidebarHeading->setBold(true);
    m_sidebarHeading->setPointSize(8.5);
    m_sidebarHeading->setTone(GlLabel::Tone::Accent);
    m_playlistList = new GlItemView(m_canvas);
    m_playlistList->setModel(m_library->listModel());
    m_playlistList->setDelegate(new PlaylistRowDelegate(m_playlistList));
    m_playlistList->setMode(GlItemView::Mode::List, QSize(200, 58), 2);
    m_playlistList->setFramed(false);
    m_playlistList->setAccessibleName(tr("My Playlists"));
    m_playlistList->setToolTip(QString());
    m_playlistList->setKeyFilter([this](QKeyEvent* key) {
        const bool plain = (key->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
        if (plain && (key->key() == Qt::Key_Delete || key->key() == Qt::Key_D)) {
            onDeletePlaylist(); // also keeps 'D' from being treated as a search key
            return true;
        }
        if (plain && key->key() == Qt::Key_F2) {
            startInlineRename();
            return true;
        }
        if (key->key() == Qt::Key_Menu) {
            return false;
        }
        return false;
    });
    connect(m_playlistList, &GlItemView::currentRowChanged, this, [this](int) { onSelectionInSidebar(); });
    connect(m_playlistList, &GlItemView::doubleClicked, this, [this](int) { startInlineRename(); });
    connect(m_playlistList, &GlItemView::contextMenuRequested, this,
            [this](const QPointF&, const QPoint& global, int row) { showPlaylistMenu(global, row); });
    m_renameEdit = new GlTextInput(m_canvas);
    m_renameEdit->setConsumeEscape(true);
    m_renameEdit->setVisible(false);
    connect(m_renameEdit, &GlTextInput::editingFinished, this, [this] { finishInlineRename(true); });
    connect(m_renameEdit, &GlTextInput::cancelled, this, [this] { finishInlineRename(false); });
    connect(m_renameEdit, &GlTextInput::focusLost, this, [this] { finishInlineRename(true); });

    // Editor header.
    m_emptyLabel = makeLabel(QString(), 11, true);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_nameLabel = makeLabel(QString(), 15);
    m_nameLabel->setBold(true);
    m_summaryLabel = makeLabel(QString(), 9.5, true);
    m_activeCheck = new GlToggle(m_canvas);
    m_activeCheck->setText(tr("Use this playlist"));
    m_activeCheck->setToolTip(tr("The active playlist - or the selected category of it - decides what is shown and "
                                 "changes it automatically. Only one source can be active. Turning it off keeps the current item."));
    connect(m_activeCheck, &GlToggle::toggled, this, [this](bool checked) {
        if (m_binding || !current()) {
            return;
        }
        const bool ok = checked ? m_library->setActive(current()->id(), selectedCategoryId()) : m_library->setActive(0);
        if (!ok && checked) {
            m_binding = true;
            m_activeCheck->setChecked(false);
            m_binding = false;
            showNotice(tr("Add at least one item that can be found on disk first."));
        }
    });

    // Categories: saved filters of THIS playlist. "All" is built in.
    m_categoryPicker = new GlTreeDropdown(m_canvas);
    m_categoryPicker->setAccessibleName(tr("Category"));
    connect(m_categoryPicker, &GlTreeDropdown::contextMenuRequested, this,
            [this](const QPoint& global) { showCategoryMenu(global); });
    connect(m_categoryPicker, &GlTreeDropdown::currentCategoryChanged, this, [this] {
        applyItemFilter();
        updateUi();
    });
    m_categoryMenuButton = makeButton(GlButton::Kind::Icon, QString(), tr("Category actions"),
                                      tr("Edit, rename or delete this category"), [this] {
        showCategoryMenu(m_canvas->mapToGlobal(QPoint(int(m_categoryMenuButton->rect().left()),
                                                      int(m_categoryMenuButton->rect().bottom()))));
    });
    m_buildCategoryButton = makeButton(GlButton::Kind::Secondary, tr("Build Category"), tr("Build Category"),
                                       tr("Save a filter for this playlist, e.g. file names containing \"anime\""),
                                       [this] { onBuildCategory(0); });
    // Direct contents only, or the category plus everything below it.
    m_includeSubCheck = new GlToggle(m_canvas);
    m_includeSubCheck->setText(tr("Include subcategories"));
    m_includeSubCheck->setToolTip(tr("On: a category shows (and plays) its own items plus those of all its "
                                     "subcategories. Off: only the items assigned to the category itself."));
    m_includeSubCheck->setChecked(m_library->includeSubcategories());
    connect(m_includeSubCheck, &GlToggle::toggled, this, [this](bool on) { m_library->setIncludeSubcategories(on); });

    m_arrangeLabel = makeLabel(tr("Arrange"), 8.5, true);
    m_arrangeLabel->setCaps(true);
    m_arrangeLabel->setBold(true);
    m_arrangeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_arrangeCombo = new GlDropdown(m_canvas);
    m_arrangeCombo->setCompact(true);
    m_arrangeCombo->addItem(tr("None"), static_cast<int>(PlaylistView::Arrangement::None));
    m_arrangeCombo->addItem(tr("A – Z"), static_cast<int>(PlaylistView::Arrangement::AtoZ));
    m_arrangeCombo->addItem(tr("Z – A"), static_cast<int>(PlaylistView::Arrangement::ZtoA));
    m_arrangeCombo->setCurrentIndex(0);
    m_arrangeCombo->setAccessibleName(tr("Arrange items"));
    m_arrangeCombo->setToolTip(tr("Only changes how the cards are shown here. None shows the playlist's own order - "
                                  "the order the wallpaper follows, which A – Z / Z – A never change."));
    m_itemSearch = new GlTextInput(m_canvas);
    m_itemSearch->setPlaceholder(tr("Search this playlist…"));
    m_itemSearch->setClearable(true);
    m_itemSearch->setAccessibleName(tr("Search this playlist"));
    m_itemSearchTimer = new QTimer(this);
    m_itemSearchTimer->setSingleShot(true);
    m_itemSearchTimer->setInterval(150);
    connect(m_itemSearch, &GlTextInput::textChanged, m_itemSearchTimer, qOverload<>(&QTimer::start));
    connect(m_itemSearchTimer, &QTimer::timeout, this, [this] {
        applyItemFilter();
        updateUi();
    });
    m_filterTimer = new QTimer(this);
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(0);
    connect(m_filterTimer, &QTimer::timeout, this, [this] {
        applyItemFilter();
        updateUi();
    });

    m_view = new PlaylistView(m_canvas, this);
    connect(m_arrangeCombo, &GlDropdown::currentIndexChanged, this, [this] {
        m_view->setArrangement(static_cast<PlaylistView::Arrangement>(m_arrangeCombo->currentData().toInt()));
        applyItemFilter(); // sorting resets the view's hidden rows
        updateUi();
    });
    m_showingLabel = makeLabel(QString(), 9.5, true);
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
    connect(m_view, &PlaylistView::itemDoubleClicked, this, [this](int row) {
        selectRow(row);
        if (current() && !current()->isAvailable(row)) {
            onFindFile(row);
        } else {
            onShowNow();
        }
    });
    connect(m_view, &PlaylistView::itemContextMenu, this,
            [this](const QPoint& global, int row) { showItemMenu(global, row); });
    connect(m_view, &PlaylistView::selectionChanged, this, &PlaylistDialog::updateUi);

    m_hintLabel = makeLabel(tr("Right-click an item for more actions  •  Drag items to set the order (Arrange: None)  •  "
                               "Double-click an item to show it now"),
                            9, true);
    m_showNowButton = makeButton(GlButton::Kind::Secondary, tr("Show Now"), tr("Show Now"),
                                 tr("Make the selected item the current one (Enter)"), [this] { onShowNow(); });
    m_flowButton = makeButton(GlButton::Kind::Secondary, tr("Wallpaper Flow…"), tr("Wallpaper Flow"),
                              tr("See and edit the order in which this playlist changes the wallpaper, as a diagram"),
                              [this] { openFlow(); });
    m_clearButton = makeButton(GlButton::Kind::Secondary, tr("Clear"), tr("Clear"),
                               tr("Remove every item from this playlist. No files are deleted."), [this] { onClear(); });

    // Missing media: explains the state and offers Find File (search the
    // drives) or removing the entry.
    m_missingBanner = new GlPanel(m_canvas);
    m_missingBanner->setBlock(GlPanel::Block::Warning);
    m_missingBanner->setRadiusPx(16);
    m_missingBanner->setVisible(false);
    m_missingTitle = makeLabel(tr("File not found"), 10.5);
    m_missingTitle->setBold(true);
    m_missingTitle->setVisible(false);
    m_missingText = makeLabel(QString(), 9.5, true);
    m_missingText->setVisible(false);
    m_findFileButton = makeButton(GlButton::Kind::Secondary, tr("Find File"), tr("Find File"),
                                  tr("Search your drives for this file and repair its location"), [this] {
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
    m_findAllButton = makeButton(GlButton::Kind::Secondary, tr("Find All at Once"), tr("Find All at Once"),
                                 tr("Search for every missing file of this playlist in a single pass over your drives"),
                                 [this] { onFindAll(); });
    m_restoreBackupButton = makeButton(GlButton::Kind::Secondary, tr("Restore from Backup"), tr("Restore from Backup"),
                                       tr("Copy this file back from Motiva's backup. An existing file is never overwritten."),
                                       [this] { onRestoreFromBackup(selectedRow()); });
    m_missingRemoveButton = makeButton(GlButton::Kind::Secondary, tr("Remove"), tr("Remove"),
                                       tr("Remove this entry from the playlist. No file is deleted."), [this] { onRemove(); });
    for (GlButton* b : {m_findFileButton, m_findAllButton, m_restoreBackupButton, m_missingRemoveButton}) {
        b->setVisible(false);
    }

    m_noticeLabel = makeLabel(QString(), 9.5, true);
    m_noticeLabel->setVisible(false);

    // Settings differ by kind: image playlists have change triggers; video
    // playlists advance when a video ends.
    m_rotationLabel = makeLabel(tr("Change image automatically"), 8.5);
    m_rotationLabel->setCaps(true);
    m_rotationLabel->setBold(true);
    m_rotationLabel->setTone(GlLabel::Tone::Accent);
    m_unlockCheck = new GlToggle(m_canvas);
    m_unlockCheck->setText(tr("After Lock → Unlock"));
    m_unlockCheck->setToolTip(tr("Show the next image each time you unlock Windows after it was locked."));
    m_startCheck = new GlToggle(m_canvas);
    m_startCheck->setText(tr("After Windows restarts or you sign in"));
    m_startCheck->setToolTip(tr("Show the next image when Motiva starts in a new Windows session. Restarting "
                                "Explorer or Motiva itself does not count."));
    m_intervalCheck = new GlToggle(m_canvas);
    m_intervalCheck->setText(tr("Every"));
    m_intervalCombo = new GlDropdown(m_canvas);
    m_intervalCombo->setCompact(true);
    m_intervalCombo->setAccessibleName(tr("Image change interval"));
    for (int m : {1, 5, 10, 15, 30, 60, 120, 180, 360, 720, 1440}) {
        m_intervalCombo->addItem(m < 60 ? countText(m, tr("%1 minute"), tr("%1 minutes"))
                                        : countText(m / 60, tr("%1 hour"), tr("%1 hours")),
                                 m);
    }
    m_intervalCombo->setCurrentIndex(0);
    m_imageNote = makeLabel(tr("Images change in playlist order and wrap around after the last. Automatic "
                               "changes only happen while this playlist is the active one."),
                            9, true);
    for (GlToggle* c : {m_unlockCheck, m_startCheck, m_intervalCheck}) {
        connect(c, &GlToggle::toggled, this, &PlaylistDialog::onRotationEdited);
    }
    connect(m_intervalCombo, &GlDropdown::currentIndexChanged, this, &PlaylistDialog::onRotationEdited);
    m_videoNote = makeLabel(tr("Videos play once each, in playlist order. After the last video the playlist "
                               "starts again from the first when \"Loop video\" is on in Settings, and stops "
                               "on the last video when it is off."),
                            9.5, true);
    m_videoNote->setWordWrap(true);

    // Footer.
    m_libraryLabel = makeLabel(m_library->isPersistent()
                                   ? tr("Library: %1").arg(QFileInfo(m_library->libraryPath()).fileName())
                                   : tr("Library not saved this session"),
                               9, true);
    m_libraryLabel->setToolTip(QDir::toNativeSeparators(m_library->libraryPath()));
    m_applyButton = new GlButton(m_canvas, GlButton::Kind::Primary);
    m_applyButton->setText(tr("Set as Wallpaper"));
    connect(m_applyButton, &GlButton::clicked, this, [this] {
        if (PlaylistModel* m = current()) {
            emit applyToDesktopRequested(m->id(), selectedCategoryId());
        }
    });
    m_footerClose = makeButton(GlButton::Kind::Secondary, tr("Close"), tr("Close"), QString(), [this] { close(); });

    applyThemedIcons();
    updateWindowButtonsState();
}

void PlaylistDialog::updateWindowButtonsState() {
    if (!m_maxButton) {
        return;
    }
    m_maxButton->setKind(isMaximized() ? GlButton::Kind::Restore : GlButton::Kind::Maximize);
    m_maxButton->setToolTip(isMaximized() ? tr("Restore") : tr("Maximize"));
}

// Positions everything. The card grid takes whatever vertical space the
// fixed rows leave; rows that do not apply right now (banner, notice,
// settings of the other playlist kind) take none.
void PlaylistDialog::layoutGl(const QSizeF& size) {
    if (!m_footerClose || !m_applyButton) {
        return;
    }
    const qreal W = size.width(), H = size.height();
    const qreal m = 12;
    PlaylistModel* pl = current();

    // Title strip.
    const QRectF strip(m, m, W - 2 * m, 46);
    m_titlePanel->setRect(strip);
    m_titleLabel->setRect(QRectF(strip.left() + 20, strip.top() + 8, 220, 30));
    const qreal bw = 36, bh = 30, by = strip.top() + 8;
    qreal x = strip.right() - 10 - bw;
    m_closeButton->setRect(QRectF(x, by, bw, bh));
    x -= bw + 2;
    m_maxButton->setRect(QRectF(x, by, bw, bh));
    x -= bw + 2;
    m_minButton->setRect(QRectF(x, by, bw, bh));

    // Toolbar.
    const QRectF tool(m, strip.bottom() + 8, W - 2 * m, 52);
    m_toolbarPanel->setRect(tool);
    qreal tx = tool.left() + 12;
    const qreal ty = tool.top() + 10, th = 32;
    const auto place = [&](GlButton* b, qreal w) {
        b->setRect(QRectF(tx, ty, w, th));
        tx += w + 8;
    };
    place(m_newImageButton, 148);
    place(m_newVideoButton, 148);
    place(m_newMixedButton, 148);
    tx += 10;
    place(m_addButton, 140);
    m_exportButton->setRect(QRectF(tool.right() - 12 - 38, ty - 1, 38, 34));
    m_importButton->setRect(QRectF(tool.right() - 12 - 38 - 8 - 38, ty - 1, 38, 34));

    // Footer.
    const QRectF footer(m, H - m - 52, W - 2 * m, 52);
    m_footerClose->setRect(QRectF(footer.right() - 110, footer.top() + 8, 110, 38));
    m_applyButton->setRect(QRectF(footer.right() - 110 - 12 - 280, footer.top() + 2, 280, 50));
    m_libraryLabel->setRect(QRectF(footer.left() + 8, footer.top() + 12, std::max<qreal>(80, footer.width() - 430), 28));

    // Body: sidebar + editor.
    const qreal bodyTop = tool.bottom() + 8;
    const qreal bodyBottom = footer.top() - 8;
    const qreal sideW = 264;
    const QRectF side(m, bodyTop, sideW, bodyBottom - bodyTop);
    const QRectF ed(side.right() + 10, bodyTop, W - m - (side.right() + 10), bodyBottom - bodyTop);
    m_sidebarPanel->setRect(side);
    m_editorPanel->setRect(ed);
    m_sidebarHeading->setRect(QRectF(side.left() + 20, side.top() + 18, side.width() - 40, 20));
    m_playlistList->setRect(QRectF(side.left() + 8, side.top() + 44, side.width() - 16, side.height() - 52));
    if (m_renameRow >= 0) {
        const QRectF rr = m_playlistList->rowRect(m_renameRow);
        if (!rr.isNull()) {
            m_renameEdit->setRect(QRectF(rr.left() + 6, rr.top() + 4, rr.width() - 12, 36));
            m_renameEdit->setClip(m_playlistList->rect());
        }
    }

    // Editor content.
    const bool has = pl != nullptr;
    const QRectF inner = ed.adjusted(18, 16, -18, -14);
    m_emptyLabel->setVisible(!has);
    m_emptyLabel->setRect(inner);
    const QList<GlItem*> editorItems = {m_nameLabel, m_summaryLabel, m_activeCheck, m_categoryPicker,
                                        m_categoryMenuButton, m_buildCategoryButton, m_includeSubCheck, m_arrangeLabel,
                                        m_arrangeCombo, m_itemSearch, m_view->item(), m_showingLabel, m_hintLabel,
                                        m_showNowButton, m_flowButton, m_clearButton, m_rotationLabel, m_unlockCheck,
                                        m_startCheck, m_intervalCheck, m_intervalCombo, m_imageNote, m_videoNote};
    for (GlItem* it : editorItems) {
        it->setVisible(has);
    }
    if (!has) {
        for (GlItem* it : QList<GlItem*>{m_missingBanner, m_missingTitle, m_missingText, m_findFileButton, m_findAllButton,
                                         m_restoreBackupButton, m_missingRemoveButton, m_noticeLabel}) {
            it->setVisible(false);
        }
        return;
    }

    qreal y = inner.top();
    // Header: name + summary left, "Use this playlist" right.
    const qreal toggleW = std::min<qreal>(320, inner.width() * 0.42);
    m_nameLabel->setRect(QRectF(inner.left(), y, inner.width() - toggleW - 10, 26));
    m_summaryLabel->setRect(QRectF(inner.left(), y + 26, inner.width() - toggleW - 10, 20));
    m_activeCheck->setRect(QRectF(inner.right() - toggleW, y + 2, toggleW, 44));
    y += 56;
    // Category row.
    const qreal rowH = 38;
    qreal cx = inner.left();
    m_categoryPicker->setRect(QRectF(cx, y, 240, 34));
    cx += 240 + 8;
    m_categoryMenuButton->setRect(QRectF(cx, y - 1, 38, 36));
    cx += 38 + 8;
    m_buildCategoryButton->setRect(QRectF(cx, y, 156, 34));
    cx += 156 + 12;
    m_includeSubCheck->setRect(QRectF(cx, y - 5, std::max<qreal>(0, inner.right() - cx), 44));
    m_includeSubCheck->setVisible(inner.right() - cx > 200);
    y += rowH + 6;
    // Search + arrange row.
    const qreal arrangeW = 130;
    m_arrangeCombo->setRect(QRectF(inner.right() - arrangeW, y, arrangeW, 34));
    m_arrangeLabel->setRect(QRectF(inner.right() - arrangeW - 8 - 80, y, 80, 34));
    m_itemSearch->setRect(QRectF(inner.left(), y, std::max<qreal>(120, inner.width() - arrangeW - 8 - 80 - 14), 34));
    y += rowH + 6;

    // Reserve space from the bottom first.
    qreal bottom = inner.bottom();
    const bool video = m_videoSettings;
    const qreal settingsH = video ? 56 : 128;
    // Settings block.
    const qreal sTop = bottom - settingsH;
    if (video) {
        m_rotationLabel->setVisible(false);
        for (GlItem* it : QList<GlItem*>{m_unlockCheck, m_startCheck, m_intervalCheck, m_intervalCombo, m_imageNote}) {
            it->setVisible(false);
        }
        m_videoNote->setVisible(true);
        m_videoNote->setRect(QRectF(inner.left(), sTop, inner.width(), settingsH));
    } else {
        m_videoNote->setVisible(false);
        m_rotationLabel->setRect(QRectF(inner.left() + 4, sTop, inner.width(), 18));
        const qreal half = inner.width() / 2;
        m_unlockCheck->setRect(QRectF(inner.left(), sTop + 20, half - 6, 40));
        m_startCheck->setRect(QRectF(inner.left() + half, sTop + 20, half, 40));
        m_intervalCheck->setRect(QRectF(inner.left(), sTop + 60, 180, 40));
        m_intervalCombo->setRect(QRectF(inner.left() + 188, sTop + 63, 200, 34));
        m_imageNote->setRect(QRectF(inner.left() + 4, sTop + 102, inner.width() - 4, 22));
    }
    bottom = sTop - 8;
    // Notice (inline feedback line).
    const bool notice = !m_noticeLabel->text().isEmpty();
    m_noticeLabel->setVisible(notice);
    if (notice) {
        m_noticeLabel->setRect(QRectF(inner.left() + 4, bottom - 20, inner.width() - 4, 20));
        bottom -= 24;
    }
    // Missing banner.
    const bool banner = m_missingBanner->isVisible();
    if (banner) {
        const QRectF br(inner.left(), bottom - 70, inner.width(), 70);
        m_missingBanner->setRect(br);
        // Buttons right-to-left.
        qreal bx = br.right() - 12;
        for (GlButton* b : {m_missingRemoveButton, m_restoreBackupButton, m_findAllButton, m_findFileButton}) {
            if (!b->isVisible()) {
                continue;
            }
            const qreal w = b == m_findFileButton ? 100 : b == m_findAllButton ? 150 : b == m_restoreBackupButton ? 170 : 90;
            b->setRect(QRectF(bx - w, br.center().y() - 17, w, 34));
            bx -= w + 8;
        }
        m_missingTitle->setRect(QRectF(br.left() + 20, br.top() + 12, bx - br.left() - 28, 22));
        m_missingText->setRect(QRectF(br.left() + 20, br.top() + 36, bx - br.left() - 28, 22));
        m_missingTitle->setVisible(true);
        m_missingText->setVisible(true);
        bottom -= 76;
    } else {
        m_missingTitle->setVisible(false);
        m_missingText->setVisible(false);
    }
    // Action row.
    m_showNowButton->setRect(QRectF(inner.left(), bottom - 36, 120, 34));
    m_flowButton->setRect(QRectF(inner.left() + 128, bottom - 36, 170, 34));
    m_clearButton->setRect(QRectF(inner.right() - 100, bottom - 36, 100, 34));
    bottom -= 42;
    // The hint line only when the grid keeps a comfortable height.
    const bool showHint = (bottom - 20 - 22) - y > 300;
    m_hintLabel->setVisible(showHint);
    if (showHint) {
        m_hintLabel->setRect(QRectF(inner.left() + 4, bottom - 18, inner.width() - 4, 18));
        bottom -= 20;
    }
    m_showingLabel->setRect(QRectF(inner.left() + 4, bottom - 18, inner.width() - 4, 18));
    bottom -= 22;
    // The grid fills the rest.
    m_view->item()->setRect(QRectF(inner.left(), y, inner.width(), std::max<qreal>(60, bottom - y)));
}

bool PlaylistDialog::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    if (eventType == "windows_generic_MSG" && m_titlePanel &&
        GlUi::handleFrameHitTest(this, m_canvas, m_titlePanel->rect(), message, result)) {
        return true;
    }
    return QWidget::nativeEvent(eventType, message, result);
}

void PlaylistDialog::dragEnterEvent(QDragEnterEvent* event) {
    if (current() && !m_view->acceptedLocalFiles(event->mimeData()).isEmpty()) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    } else {
        event->ignore();
    }
}

void PlaylistDialog::dragMoveEvent(QDragMoveEvent* event) {
    const QPointF pos = event->position();
    if (!current() || m_view->acceptedLocalFiles(event->mimeData()).isEmpty() || !m_view->item()->rect().contains(pos)) {
        m_view->item()->setDropIndicator(-1);
        event->ignore();
        return;
    }
    m_view->item()->setDropIndicator(m_view->isArranged() ? -1 : m_view->item()->insertPositionAt(pos));
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

void PlaylistDialog::dragLeaveEvent(QDragLeaveEvent* event) {
    m_view->item()->setDropIndicator(-1);
    QWidget::dragLeaveEvent(event);
}

void PlaylistDialog::dropEvent(QDropEvent* event) {
    const QPointF pos = event->position();
    const int insert = m_view->isArranged() ? -1 : m_view->item()->insertPositionAt(pos);
    m_view->item()->setDropIndicator(-1);
    const QStringList files = m_view->acceptedLocalFiles(event->mimeData());
    if (files.isEmpty() || !current() || !m_view->item()->rect().contains(pos)) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
    addFilesTo(current()->id(), files, insert);
}

PlaylistModel* PlaylistDialog::current() const {
    return m_view ? m_view->playlist() : nullptr;
}

qint64 PlaylistDialog::selectedCategoryId() const {
    const PlaylistFilterInfo* filter = currentFilter();
    return filter ? filter->id : 0;
}

bool PlaylistDialog::isSelectionActive() const {
    const PlaylistModel* m = current();
    return m && m->isActive() && m_library->activeCategoryId() == selectedCategoryId();
}

void PlaylistDialog::activateSelection() {
    if (PlaylistModel* m = current()) {
        m_library->setActive(m->id(), selectedCategoryId());
    }
}

void PlaylistDialog::selectPlaylist(qint64 id) {
    const int row = m_library->listModel()->rowOf(id);
    if (row >= 0) {
        m_playlistList->setCurrentRow(row); // emits -> onSelectionInSidebar when it changes
    } else if (m_library->playlistCount() > 0) {
        selectPlaylist(m_library->listModel()->idAt(0));
        return;
    }
    bindSelectedPlaylist();
}

void PlaylistDialog::onSelectionInSidebar() {
    const int row = m_playlistList->currentRow();
    if (row >= 0) {
        m_library->setSelected(m_library->listModel()->idAt(row));
    }
    showNotice(QString());
    bindSelectedPlaylist();
}

void PlaylistDialog::bindSelectedPlaylist() {
    const int listRow = m_playlistList->currentRow();
    const qint64 id = listRow < 0 ? 0 : m_library->listModel()->idAt(listRow);
    PlaylistModel* model = m_library->playlist(id);
    if (model != m_view->playlist()) {
        m_view->setPlaylist(model);
        if (model) {
            connect(model, &QAbstractItemModel::modelReset, m_filterTimer, qOverload<>(&QTimer::start),
                    Qt::UniqueConnection);
            connect(model, &QAbstractItemModel::layoutChanged, m_filterTimer, qOverload<>(&QTimer::start),
                    Qt::UniqueConnection);
            connect(model, &QAbstractItemModel::rowsInserted, m_filterTimer, qOverload<>(&QTimer::start),
                    Qt::UniqueConnection);
            connect(model, &QAbstractItemModel::rowsRemoved, m_filterTimer, qOverload<>(&QTimer::start),
                    Qt::UniqueConnection);
            connect(model, &QAbstractItemModel::rowsMoved, m_filterTimer, qOverload<>(&QTimer::start),
                    Qt::UniqueConnection);
            connect(model, &PlaylistModel::contentsChanged, m_filterTimer, qOverload<>(&QTimer::start),
                    Qt::UniqueConnection);
        }
        if ((model ? model->id() : 0) != m_filtersPlaylistId) {
            m_itemSearch->blockSignals(true);
            m_itemSearch->clear();
            m_itemSearch->blockSignals(false);
            reloadCategories(true); // every playlist opens on "All"
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

void PlaylistDialog::setSettings(SettingsManager* settings) {
    m_settings = settings;
    restoreSavedGeometry();
}

void PlaylistDialog::restoreSavedGeometry() {
    if (!m_settings) {
        return;
    }
    const QByteArray saved = m_settings->playlistWindowGeometry();
    if (saved.isEmpty() || !restoreGeometry(saved)) {
        return;
    }
    // Monitors may have changed since: if too little of the window is on any
    // screen to grab it, bring it back onto the nearest one.
    const QRect frame = frameGeometry();
    bool reachable = false;
    for (QScreen* screen : QGuiApplication::screens()) {
        const QRect shared = screen->availableGeometry().intersected(frame);
        if (shared.width() >= 120 && shared.height() >= 60) {
            reachable = true;
            break;
        }
    }
    if (!reachable) {
        QScreen* screen = QGuiApplication::primaryScreen();
        const QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 720);
        resize(size().boundedTo(avail.size() * 9 / 10));
        move(avail.center() - rect().center());
    }
}

void PlaylistDialog::saveGeometryLater() {
    if (!m_settings || !isVisible()) {
        return;
    }
    if (!m_geometryTimer) {
        m_geometryTimer = new QTimer(this);
        m_geometryTimer->setSingleShot(true);
        m_geometryTimer->setInterval(400);
        connect(m_geometryTimer, &QTimer::timeout, this, [this] {
            if (m_settings && !isMinimized()) {
                m_settings->setPlaylistWindowGeometry(saveGeometry());
            }
        });
    }
    m_geometryTimer->start();
}

void PlaylistDialog::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    saveGeometryLater();
}

void PlaylistDialog::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    saveGeometryLater();
}

void PlaylistDialog::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    // Closed inside the debounce window: write the geometry now.
    if (m_settings && !isMinimized() && !event->spontaneous()) {
        m_settings->setPlaylistWindowGeometry(saveGeometry());
    }
}

void PlaylistDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier) {
        close();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PlaylistDialog::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    setWindowIcon(QIcon(playlistIconPath()));
    bindSelectedPlaylist();
}

int PlaylistDialog::selectedRow() const {
    return m_view->selectedSourceRow();
}

void PlaylistDialog::selectRow(int row) {
    PlaylistModel* m = current();
    if (!m || row < 0 || row >= m->count()) {
        return;
    }
    m_view->selectSourceRow(row);
    updateUi();
}

void PlaylistDialog::setBackupManager(BackupManager* backup) {
    m_backup = backup;
    if (!m_backup) {
        return;
    }
    connect(m_backup, &BackupManager::restoreProgress, this, [this](qint64, int percent) {
        showNotice(tr("Restoring from backup… %1%").arg(percent));
    });
    connect(m_backup, &BackupManager::restoreFinished, this,
            [this](qint64, bool ok, const QString& path, const QString& message) {
                showNotice(ok ? tr("Restored \"%1\" from backup. Its place in every playlist is unchanged.")
                                    .arg(QFileInfo(path).fileName())
                              : tr("Could not restore from backup: %1").arg(message));
                updateUi();
            });
}

// "Restore from Backup": the backup is copied back to the file's original
// location when that is free; if a file is already there, or the folder/drive
// isn't, the user picks where to put it. Never overwrites. The media record
// is the same one - only its saved path changes (PlaylistLibrary::relocateMedia).
void PlaylistDialog::onRestoreFromBackup(int row) {
    PlaylistModel* m = current();
    if (!m || !m_backup || row < 0 || row >= m->count() || m_backup->isRestoring()) {
        return;
    }
    const qint64 mediaId = m->mediaIdAt(row);
    if (!m_backup->hasUsableBackup(mediaId)) {
        showNotice(tr("There is no usable backup of this file."));
        updateUi();
        return;
    }
    QString destination = m->pathAt(row);
    const QFileInfo target(destination);
    if (target.exists() || !target.dir().exists()) {
        const QString reason = target.exists()
                                   ? tr("A different file already exists at the original location, and Motiva never "
                                        "overwrites files.")
                                   : tr("The original folder isn't available.");
        QMessageBox::information(this, tr("Restore from Backup"),
                                 reason + QStringLiteral("\n\n") + tr("Choose where to restore the backup."));
        destination = QFileDialog::getSaveFileName(this, tr("Restore from Backup"),
                                                   target.exists() ? QDir::home().filePath(target.fileName())
                                                                   : QDir::home().filePath(target.fileName()),
                                                   QString(), nullptr, QFileDialog::DontConfirmOverwrite);
        if (destination.isEmpty()) {
            return;
        }
        if (QFileInfo::exists(destination)) {
            showNotice(tr("A file already exists there - choose a name that isn't in use. Nothing was overwritten."));
            return;
        }
    }
    showNotice(tr("Restoring from backup…"));
    m_backup->restoreMedia(mediaId, destination);
    updateUi();
}

void PlaylistDialog::showNotice(const QString& text) {
    m_noticeLabel->setText(text);
    m_noticeLabel->setVisible(!text.isEmpty());
    m_canvas->relayout();
}

void PlaylistDialog::announce(int kind, const QString& title, const QString& message, const QString& key,
                              const QString& inlineText, bool desktop, bool background) {
    Notification n;
    n.kind = static_cast<Notification::Kind>(kind);
    n.category = background ? Notification::Category::Background : Notification::Category::General;
    n.title = title;
    n.message = message;
    n.key = key;
    n.desktop = desktop;
    NotificationManager* notifier = NotificationManager::instance();
    const bool shown = notifier->notify(n);
    // Switched off by the user: their choice, say nothing more. Otherwise a
    // missing toast (no visible window, a suppressed repeat) falls back to
    // this window's own notice line.
    if (!shown && !inlineText.isEmpty() && notifier->inAppEnabledFor(n)) {
        showNotice(inlineText);
    }
}

void PlaylistDialog::onCreatePlaylist(int type) {
    const PlaylistType t = static_cast<PlaylistType>(type);
    bool ok = false;
    const bool mixed = t == PlaylistType::Mixed;
    const QString name = QInputDialog::getText(
        this, mixed ? tr("New Mixed Playlist") : t == PlaylistType::Video ? tr("New Video Playlist") : tr("New Image Playlist"),
        tr("Playlist name:"), QLineEdit::Normal,
        mixed ? tr("My Media") : t == PlaylistType::Video ? tr("My Videos") : tr("My Images"), &ok);
    if (!ok) {
        return;
    }
    const qint64 id = m_library->createPlaylist(name, t);
    if (id > 0) {
        selectPlaylist(id);
        const PlaylistModel* created = m_library->playlist(id);
        const QString createdName = created ? created->name() : name;
        announce(int(Notification::Kind::Success), tr("Playlist Created"),
                 mixed ? tr("\"%1\" was created. Add images and videos in any order.").arg(createdName)
                 : t == PlaylistType::Video ? tr("\"%1\" was created. Add some videos to it.").arg(createdName)
                                            : tr("\"%1\" was created. Add some images to it.").arg(createdName),
                 QStringLiteral("playlist-created:%1").arg(id),
                 mixed ? tr("Mixed playlist created - add images and videos.")
                 : t == PlaylistType::Video ? tr("Video playlist created - add some videos.")
                                            : tr("Image playlist created - add some images."));
    }
}

void PlaylistDialog::startInlineRename() {
    const int row = m_playlistList->currentRow();
    if (row < 0) {
        return;
    }
    m_renameRow = row;
    m_playlistList->scrollTo(row);
    m_renameEdit->setVisible(true);
    m_canvas->relayout();
    m_renameEdit->setText(m_library->listModel()->index(row).data(Qt::DisplayRole).toString());
    m_renameEdit->focusInput(true);
}

// Enter / focus loss commit (through PlaylistListModel::setData, the single
// rename path); Escape cancels.
void PlaylistDialog::finishInlineRename(bool commit) {
    if (m_renameRow < 0) {
        return;
    }
    const int row = m_renameRow;
    m_renameRow = -1;
    const QString text = m_renameEdit->text().trimmed();
    m_renameEdit->setVisible(false);
    m_playlistList->setCurrentRow(row, false);
    m_canvas->setFocusedItem(m_playlistList);
    if (commit && !text.isEmpty() && row < m_library->listModel()->rowCount()) {
        m_library->listModel()->setData(m_library->listModel()->index(row), text, Qt::EditRole);
    }
    m_canvas->requestAnimation();
}

void PlaylistDialog::showPlaylistMenu(const QPoint& global, int row) {
    if (row < 0) {
        row = m_playlistList->currentRow(); // keyboard (Menu key): act on the selection
        if (row < 0) {
            return;
        }
    }
    m_playlistList->setCurrentRow(row, false);
    QMenu menu(this);
    QAction* rename = menu.addAction(tr("Rename"));
    rename->setShortcut(QKeySequence(Qt::Key_F2));
    QAction* del = menu.addAction(tr("Delete"));
    del->setShortcut(QKeySequence(QKeySequence::Delete));
    QAction* chosen = menu.exec(global);
    if (chosen == rename) {
        startInlineRename();
    } else if (chosen == del) {
        onDeletePlaylist();
    }
}

void PlaylistDialog::showItemMenu(const QPoint& global, int sourceRow) {
    PlaylistModel* m = current();
    if (!m) {
        return;
    }
    int row = sourceRow;
    if (row < 0) {
        row = selectedRow(); // Menu key / Shift+F10
        if (row < 0) {
            return;
        }
    }
    selectRow(row); // the menu always acts on the item it was opened for
    const bool available = m->isAvailable(row);

    QMenu menu(this);
    QAction* showNow = nullptr;
    QAction* findFile = nullptr;
    QAction* moveLeft = nullptr;
    QAction* moveRight = nullptr;
    QAction* moveFirst = nullptr;
    QAction* moveLast = nullptr;
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
    if (row > 0) {
        moveFirst = menu.addAction(tr("Move to Start"));
        moveFirst->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Home));
    }
    if (row < m->count() - 1) {
        moveLast = menu.addAction(tr("Move to End"));
        moveLast->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_End));
    }
    if (!menu.isEmpty()) {
        menu.addSeparator();
    }
    // Add to Category: the playlist's own categories, as a hierarchy.
    const qint64 itemId = m->itemIdAt(row);
    QMenu* categoryMenu = menu.addMenu(tr("Add to Category"));
    categoryMenu->setToolTipsVisible(true);
    QHash<QAction*, qint64> categoryActions;
    if (m_filters.isEmpty()) {
        QAction* none = categoryMenu->addAction(tr("No categories yet"));
        none->setEnabled(false);
    } else {
        addCategoryTree(categoryMenu, 0, {}, categoryActions, [itemId](const PlaylistFilterInfo& f, QAction* a) {
            if (!f.isSelection()) {
                a->setEnabled(false);
                a->setToolTip(PlaylistDialog::tr("Rule-based category: it collects items by its conditions."));
            } else if (f.itemIds.contains(itemId)) {
                a->setCheckable(true);
                a->setChecked(true);
                a->setEnabled(false);
                a->setToolTip(PlaylistDialog::tr("This item is already in the category."));
            }
        });
    }
    categoryMenu->addSeparator();
    QAction* newCategory = categoryMenu->addAction(tr("New Category…"));
    menu.addSeparator();
    QAction* remove = menu.addAction(tr("Remove"));
    remove->setShortcut(QKeySequence(QKeySequence::Delete));
    remove->setToolTip(tr("Remove from this playlist. The file itself is not deleted."));

    QAction* chosen = menu.exec(global);
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
    } else if (chosen == moveFirst || chosen == moveLast) {
        if (m_view->isArranged()) {
            showNotice(tr("Set Arrange to None to change the order - A – Z / Z – A only rearrange the view."));
        } else if (m->move(row, chosen == moveFirst ? 0 : m->count() - 1)) {
            selectRow(chosen == moveFirst ? 0 : m->count() - 1);
        }
    } else if (categoryActions.contains(chosen)) {
        assignItemsToCategory(categoryActions.value(chosen), {itemId});
    } else if (chosen == newCategory) {
        onNewCategoryFromItems({itemId});
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
        if (result == QDialog::Rejected) {
            announce(int(Notification::Kind::Info), tr("Search Cancelled"),
                     tr("Motiva stopped looking for \"%1\".").arg(QFileInfo(path).fileName()),
                     QStringLiteral("find-file-cancelled"), QString(), false, /*background=*/true);
        }
        if (result == MediaRecoveryDialog::Found) {
            const QString found = dialog->chosenPath();
            if (m_library->relocateMedia(mediaId, found)) {
                announce(int(Notification::Kind::Success), tr("File Found"),
                         tr("\"%1\" was found at %2. Its location was updated.")
                             .arg(QFileInfo(found).fileName(), QDir::toNativeSeparators(QFileInfo(found).absolutePath())),
                         QStringLiteral("find-file-done"),
                         tr("Found \"%1\" at %2. Its location was updated.")
                             .arg(QFileInfo(found).fileName(), QDir::toNativeSeparators(QFileInfo(found).absolutePath())),
                         /*desktop=*/true, /*background=*/true);
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

void PlaylistDialog::onFindAll() {
    PlaylistModel* m = current();
    if (!m || m_recoveryRunning) {
        return; // one search at a time
    }
    m->refreshAvailability();
    // One entry per media record (a file listed twice is searched once).
    QVector<BulkMediaRecoveryDialog::Entry> entries;
    QSet<qint64> seen;
    for (int r = 0; r < m->count(); ++r) {
        if (m->isAvailable(r) || seen.contains(m->mediaIdAt(r))) {
            continue;
        }
        seen.insert(m->mediaIdAt(r));
        entries.push_back({m->mediaIdAt(r), m->pathAt(r), m->factsAt(r)});
    }
    if (entries.isEmpty()) {
        showNotice(tr("Nothing is missing - every file in this playlist is available."));
        updateUi();
        return;
    }
    m_recoveryRunning = true;
    updateUi();
    const qint64 playlistId = m->id();
    auto* dialog = new BulkMediaRecoveryDialog(entries, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [this, dialog, playlistId, total = int(entries.size())](int result) {
        m_recoveryRunning = false;
        const int located = dialog->locatedCount();
        const int unreadable = dialog->unreadableFolders();
        const QString unreadableNote = unreadable > 0
            ? tr(" %1 folder(s) couldn't be read, so files inside them weren't checked.").arg(unreadable)
            : QString();
        if (dialog->failed()) {
            announce(int(Notification::Kind::Error), tr("Search failed"),
                     tr("Motiva couldn't finish looking for the missing files. Details are in the log."),
                     QStringLiteral("find-all-failed"), QString(), /*desktop=*/true, /*background=*/true);
        } else if (result != QDialog::Accepted && located == 0) {
            if (dialog->wasCancelled()) {
                announce(int(Notification::Kind::Info), tr("Search Cancelled"),
                         tr("Motiva stopped looking for the missing files."), QStringLiteral("find-all-done"), QString(),
                         false, /*background=*/true);
            } else {
                announce(int(Notification::Kind::Warning), tr("No Matching Files Found"),
                         tr("None of the %1 missing files were found.").arg(total) + unreadableNote,
                         QStringLiteral("find-all-done"), QString(), /*desktop=*/true, /*background=*/true);
            }
        }
        if (result == QDialog::Accepted) {
            // Resolved by media id: each repair reloads the affected models,
            // so rows move - ids stay valid.
            int repaired = 0;
            const auto resolutions = dialog->resolutions();
            for (const auto& res : resolutions) {
                if (m_library->relocateMedia(res.mediaId, res.newPath)) {
                    ++repaired;
                }
            }
            const int stillMissing = [&] {
                PlaylistModel* pm = m_library->playlist(playlistId);
                if (!pm) {
                    return 0;
                }
                pm->refreshAvailability();
                return pm->count() - pm->availableCount();
            }();
            const QString summary = tr("Repaired %1 of %2 missing files.").arg(repaired).arg(total) +
                                    (stillMissing > 0 ? tr(" %1 still missing.").arg(stillMissing) : QString());
            announce(int(repaired == 0 || stillMissing > 0 ? Notification::Kind::Warning : Notification::Kind::Success),
                     repaired == 0 ? tr("No Files Repaired") : tr("Missing Files Repaired"), summary + unreadableNote,
                     QStringLiteral("find-all-done"), summary, /*desktop=*/true, /*background=*/true);
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
        m_canvas->relayout();
        return;
    }
    const bool selectedMissing = row >= 0 && !m->isAvailable(row);
    m_missingTitle->setColor(GlUi::Style::current().warn);
    if (selectedMissing) {
        const QString name = QFileInfo(m->pathAt(row)).fileName();
        m_missingTitle->setText(tr("File not found"));
        m_missingText->setText(tr("Motiva can't find \"%1\" at its saved location.").arg(name));
        m_missingText->setToolTip(QDir::toNativeSeparators(m->pathAt(row)));
        m_missingRemoveButton->setVisible(true);
    } else {
        m_missingTitle->setText(missing == 1 ? tr("1 file not found") : tr("%1 files not found").arg(missing));
        m_missingText->setText(tr("Motiva can't find some of this playlist's media. Find File searches your drives."));
        m_missingText->setToolTip(QString());
        m_missingRemoveButton->setVisible(false);
    }
    m_findFileButton->setVisible(true);
    m_findFileButton->setEnabled(!m_recoveryRunning);
    m_findAllButton->setVisible(missing > 1);
    m_findAllButton->setEnabled(!m_recoveryRunning);
    m_restoreBackupButton->setVisible(selectedMissing && m_backup && m_backup->hasUsableBackup(m->mediaIdAt(row)));
    m_restoreBackupButton->setEnabled(!m_backup || !m_backup->isRestoring());
    m_missingBanner->setVisible(true);
    m_canvas->relayout();
}

void PlaylistDialog::onDeletePlaylist() {
    PlaylistModel* m = current();
    if (!m) {
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Delete Playlist"),
        tr("Delete the playlist \"%1\"?\n\nOnly the playlist is removed - your %2 files are not deleted.")
            .arg(m->name(), m->isMixed() ? tr("image and video") : m->isVideo() ? tr("video") : tr("image")),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }
    const QString name = m->name();
    const qint64 id = m->id();
    const int row = m_library->listModel()->rowOf(id);
    m_view->setPlaylist(nullptr); // the model is destroyed by deletePlaylist
    if (m_library->deletePlaylist(id)) {
        announce(int(Notification::Kind::Success), tr("Playlist Deleted"),
                 tr("\"%1\" was deleted. No files were deleted.").arg(name),
                 QStringLiteral("playlist-deleted:%1").arg(id),
                 tr("Deleted playlist \"%1\". No files were deleted.").arg(name));
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
        // errorOccurred already explained the cause; this replaces that
        // toast in place (same key) with what the user was trying to do.
        announce(int(Notification::Kind::Error),
                 paths.size() == 1 ? tr("Unable to add wallpaper to the playlist")
                                   : tr("Unable to add wallpapers to the playlist"),
                 tr("Nothing was added to \"%1\".").arg(m->name()), QStringLiteral("library-error"), QString(),
                 /*desktop=*/true);
        return;
    }
    const bool video = m->isVideo();
    const bool mixed = m->isMixed();
    QStringList parts;
    if (result.added > 0) {
        parts << countText(result.added, mixed ? tr("Added %1 item.") : video ? tr("Added %1 video.") : tr("Added %1 image."),
                           mixed ? tr("Added %1 items.") : video ? tr("Added %1 videos.") : tr("Added %1 images."));
    }
    if (result.duplicates > 0) {
        parts << countText(result.duplicates, tr("%1 was already in the playlist."),
                           tr("%1 were already in the playlist."));
    }
    if (result.unsupported > 0) {
        parts << countText(result.unsupported,
                           mixed ? tr("%1 file skipped - not a supported image or video.")
                           : video ? tr("%1 file skipped - not a supported video.") : tr("%1 file skipped - not a supported image."),
                           mixed ? tr("%1 files skipped - not supported images or videos.")
                           : video ? tr("%1 files skipped - not supported videos.") : tr("%1 files skipped - not supported images."));
    }
    const QString summary = parts.join(QLatin1Char(' '));
    if (result.added > 0) {
        const int first = (insertRow < 0 || insertRow > m->count() - result.added) ? m->count() - result.added : insertRow;
        // Reported only now, after the model confirmed the write - one
        // toast for the whole batch.
        QString text = result.added == 1
            ? tr("\"%1\" was added to \"%2\".").arg(QFileInfo(m->pathAt(first)).fileName(), m->name())
            : tr("%1 wallpapers were added to \"%2\".").arg(result.added).arg(m->name());
        if (parts.size() > 1) {
            text += QLatin1Char(' ') + parts.mid(1).join(QLatin1Char(' ')); // duplicates / skipped files
        }
        announce(int(Notification::Kind::Success), result.added == 1 ? tr("Wallpaper Added") : tr("Wallpapers Added"),
                 text, QStringLiteral("playlist-added:%1").arg(playlistId), summary);
        selectRow(first);
    } else if (!summary.isEmpty()) {
        // Nothing was added (all duplicates / unsupported): a warning, not a success.
        announce(int(Notification::Kind::Warning), tr("Nothing Added"), summary,
                 QStringLiteral("playlist-add-none:%1").arg(playlistId), summary);
    }
    updateUi();
}

void PlaylistDialog::onAddMedia() {
    PlaylistModel* m = current();
    if (!m) {
        return;
    }
    QStringList exts = m->isMixed() ? (VideoPlayer::supportedVideoExtensions() + VideoPlayer::supportedStaticImageExtensions()).values()
                       : m->isVideo() ? VideoPlayer::supportedVideoExtensions().values()
                                      : VideoPlayer::supportedStaticImageExtensions().values();
    exts.sort(Qt::CaseInsensitive);
    QStringList patterns;
    for (const QString& ext : exts) {
        patterns << QStringLiteral("*.%1").arg(ext);
    }
    const QString kind = m->isMixed() ? tr("Images and Videos (%1)") : m->isVideo() ? tr("Videos (%1)") : tr("Images (%1)");
    const QString filter = kind.arg(patterns.join(QLatin1Char(' '))) + QStringLiteral(";;") + tr("All Files (*)");
    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        m->isMixed() ? tr("Add Media to \"%1\"").arg(m->name())
        : m->isVideo() ? tr("Add Videos to \"%1\"").arg(m->name()) : tr("Add Images to \"%1\"").arg(m->name()),
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
        int next = qMin(row, m->count() - 1);
        if (next >= 0 && m_view->isSourceRowHidden(next)) {
            const int after = m_view->visibleNeighbour(next, +1);
            next = after >= 0 ? after : m_view->visibleNeighbour(next, -1);
        }
        selectRow(next);
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
    if (m_view->isArranged()) {
        showNotice(tr("Set Arrange to None to change the order - A – Z / Z – A only rearrange the view."));
        return;
    }
    // To the next VISIBLE position: items outside the current category are
    // hidden and keep their place in the playlist.
    const int to = m_view->visibleNeighbour(row, delta < 0 ? -1 : +1);
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
        announce(int(Notification::Kind::Warning), tr("File Not Found"),
                 tr("\"%1\" can't be found on disk. Reconnect its drive or remove it from the playlist.")
                     .arg(QFileInfo(m->pathAt(row)).fileName()),
                 QStringLiteral("show-missing"),
                 tr("That file can't be found on disk. Reconnect its drive or remove it from the playlist."));
        return;
    }
    // Choosing an item to show is using this playlist - make it the active
    // one so the item is applied (and rotation continues from it).
    if (!isSelectionActive()) {
        activateSelection(); // the category being viewed (the shown item belongs to it)
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
    m_emptyLabel->setText(hasPlaylists ? tr("Select a playlist on the left.")
                                       : tr("Create a playlist with \"Image Playlist\" or \"Video Playlist\" to get started.\n\n"
                                            "Image playlists change image on unlock, at Windows start or on a timer.\n"
                                            "Video playlists play their videos one after another."));
    m_addButton->setEnabled(m != nullptr);
    if (!m) {
        m_applyButton->setEnabled(false);
        m_applyButton->setText(tr("Set as Wallpaper"));
        m_canvas->relayout();
        return;
    }

    const int count = m->count();
    const int available = m->availableCount();
    const int missing = count - available;
    const int row = selectedRow();
    const int currentRow = m->currentIndex();
    const bool video = m->isVideo();
    const bool mixed = m->isMixed();

    m_nameLabel->setText(m->name());
    QString summary = mixed ? countText(count, tr("%1 item"), tr("%1 items"))
                            : countText(count, video ? tr("%1 video") : tr("%1 image"), video ? tr("%1 videos") : tr("%1 images"));
    if (mixed && count > 0) {
        int videos = 0;
        for (int r = 0; r < count; ++r) {
            videos += m->isVideoAt(r) ? 1 : 0;
        }
        summary += tr(" (%1 images, %2 videos)").arg(count - videos).arg(videos);
    }
    if (count > 0) {
        if (m->isActive()) {
            const QString category = m_library->activeCategoryName();
            summary += QStringLiteral("  •  ") + (category.isEmpty() ? tr("Active") : tr("Active category: %1").arg(category)) +
                tr(" - item %1 of %2").arg(m->scopePosition()).arg(m->scopeCount());
            summary += m_onDesktop ? tr("  •  On the desktop") : tr("  •  Not on the desktop yet");
        } else if (currentRow >= 0) {
            summary += QStringLiteral("  •  ") + tr("Not active - resumes at item %1").arg(currentRow + 1);
        }
    }
    if (missing > 0) {
        summary += QStringLiteral("  •  ") + countText(missing, tr("%1 file not found"), tr("%1 files not found"));
    }
    m_summaryLabel->setText(summary);
    int shown = 0;
    for (int r = 0; r < count; ++r) {
        shown += m_view->isSourceRowHidden(r) ? 0 : 1;
    }
    const bool filtered = currentFilter() || !m_itemSearch->text().trimmed().isEmpty();
    m_showingLabel->setText(!filtered        ? countText(count, tr("Showing %1 item"), tr("Showing %1 items"))
                            : shown == 0     ? tr("No items match - showing 0 of %1").arg(count)
                                             : tr("Showing %1 of %2 items").arg(shown).arg(count));
    m_categoryMenuButton->setEnabled(currentFilter() != nullptr);
    m_categoryPicker->setAccessibleName(tr("Categories of \"%1\"").arg(m->name()));
    m_binding = true;
    m_activeCheck->setChecked(isSelectionActive());
    m_activeCheck->setText(currentFilter() ? tr("Use this category") : tr("Use this playlist"));
    m_binding = false;

    m_addButton->setText(mixed ? tr("Add Media") : video ? tr("Add Videos") : tr("Add Images"));
    m_addButton->setToolTip(mixed ? tr("Add image and video files to the end of the playlist, in the order you choose")
                            : video ? tr("Add one or more video files to the end of the playlist")
                                    : tr("Add one or more image files to the end of the playlist"));
    m_videoSettings = video;
    m_rotationLabel->setText(mixed ? tr("Change image automatically (videos play to their end, then move on)")
                                   : tr("Change image automatically"));
    m_intervalCombo->setEnabled(m_intervalCheck->isChecked());

    m_showNowButton->setEnabled(row >= 0 && m->isAvailable(row) && (row != currentRow || !m->isActive()));
    updateMissingBanner(row);
    m_clearButton->setEnabled(count > 0);

    const bool onDesktop = m_onDesktop && isSelectionActive();
    int usable = available; // available items in the selected category ("All" = the playlist)
    if (currentFilter()) {
        usable = 0;
        const QSet<qint64> members = categoryItemIds(QString());
        for (int r = 0; r < count; ++r) {
            usable += (members.contains(m->itemIdAt(r)) && m->isAvailable(r)) ? 1 : 0;
        }
    }
    m_applyButton->setText(onDesktop ? (currentFilter() ? tr("This category is on the desktop") : tr("This playlist is on the desktop"))
                                     : tr("Set as Wallpaper"));
    m_applyButton->setToolTip(currentFilter() ? tr("Rotate only the %1 item(s) of the selected category on the desktop. "
                                                   "The playlist's order is not changed.").arg(usable)
                                              : QString());
    m_applyButton->setEnabled(!onDesktop && usable > 0);
    m_canvas->relayout();
}

// ------------------------------------------------------- shortcuts helpers

void PlaylistDialog::createPlaylist(bool video) {
    onCreatePlaylist(static_cast<int>(video ? PlaylistType::Video : PlaylistType::Image));
}

void PlaylistDialog::createMixedPlaylist() {
    onCreatePlaylist(static_cast<int>(PlaylistType::Mixed));
}

bool PlaylistDialog::isTextInputFocused() {
    if (GlTextInput::anyFocused()) {
        return true;
    }
    QWidget* w = QApplication::focusWidget();
    if (!w) {
        return false;
    }
    if (auto* combo = qobject_cast<QComboBox*>(w)) {
        return combo->isEditable();
    }
    return qobject_cast<QLineEdit*>(w) || qobject_cast<QTextEdit*>(w) || qobject_cast<QAbstractSpinBox*>(w) ||
           w->inherits("QPlainTextEdit");
}

// ------------------------------------------------ virtual categories (filters)

void PlaylistDialog::reloadCategories(bool resetToAll) {
    PlaylistModel* m = current();
    const qint64 keep = resetToAll ? 0 : m_categoryPicker->currentCategoryId();
    m_filtersPlaylistId = m ? m->id() : 0;
    m_filters = m ? m_library->filters(m->id()) : QVector<PlaylistFilterInfo>{};
    m_categoryPicker->blockSignals(true);
    m_categoryPicker->setCategories(m_filters, tr("All")); // "All" is built in: every item of the playlist
    m_categoryPicker->setCurrentCategory(keep);
    m_categoryPicker->blockSignals(false);
    if (m_includeSubCheck && m_includeSubCheck->isChecked() != m_library->includeSubcategories()) {
        m_includeSubCheck->blockSignals(true);
        m_includeSubCheck->setChecked(m_library->includeSubcategories());
        m_includeSubCheck->blockSignals(false);
    }
    applyItemFilter();
    updateUi();
}

const PlaylistFilterInfo* PlaylistDialog::currentFilter() const {
    const qint64 id = m_categoryPicker ? m_categoryPicker->currentCategoryId() : 0;
    for (const PlaylistFilterInfo& f : m_filters) {
        if (f.id == id) {
            return &f;
        }
    }
    return nullptr; // "All"
}

QSet<qint64> PlaylistDialog::categoryItemIds(const QString& search) const {
    QSet<qint64> ids;
    PlaylistModel* m = current();
    if (!m) {
        return ids;
    }
    const PlaylistFilterInfo* filter = currentFilter();
    if (!filter && search.isEmpty()) {
        for (int r = 0; r < m->count(); ++r) {
            ids.insert(m->itemIdAt(r));
        }
        return ids;
    }
    // The library resolves a category (rules, hand-picked members, and - when
    // "Include subcategories" is on - everything below it) so the view and
    // playback always agree; the search then narrows within the result.
    if (filter) {
        ids = m_library->categoryItemIds(m->id(), filter->id);
        if (!search.isEmpty()) {
            ids.intersect(m_library->matchingItems(m->id(), nullptr, search));
        }
        return ids;
    }
    return m_library->matchingItems(m->id(), nullptr, search);
}

void PlaylistDialog::openFlow() {
    PlaylistModel* m = current();
    if (!m) {
        return;
    }
    const PlaylistFilterInfo* filter = currentFilter();
    const QString scopeName = tr("%1 > %2").arg(m->name(), filter ? filter->name : tr("All"));
    FlowDialog flow(m_library, m, scopeName, [this] { return categoryItemIds(QString()); }, this);
    flow.setScopeCategoryId(selectedCategoryId());
    connect(&flow, &FlowDialog::addMediaRequested, this, [this, &flow] {
        onAddMedia();
        flow.refresh();
    });
    flow.exec();
    applyItemFilter();
    updateUi();
}

void PlaylistDialog::applyItemFilter() {
    PlaylistModel* m = current();
    if (!m) {
        return;
    }
    const PlaylistFilterInfo* filter = currentFilter();
    const QString search = m_itemSearch->text().trimmed();
    if (!filter && search.isEmpty()) {
        for (int r = 0; r < m->count(); ++r) {
            m_view->setSourceRowHidden(r, false);
        }
    } else {
        const QSet<qint64> visible = categoryItemIds(search);
        for (int r = 0; r < m->count(); ++r) {
            m_view->setSourceRowHidden(r, !visible.contains(m->itemIdAt(r)));
        }
    }
    const int row = selectedRow();
    if (row >= 0 && m_view->isSourceRowHidden(row)) {
        m_view->clearSelection();
    }
}

void PlaylistDialog::showCategoryMenu(const QPoint& globalPos) {
    QMenu menu(this);
    const bool user = currentFilter() != nullptr; // "All" can't be edited, renamed or deleted
    const PlaylistFilterInfo* selected = currentFilter();
    QAction* editAction = menu.addAction(selected && selected->isSelection() ? tr("Manage Selection…") : tr("Edit Category…"));
    QAction* renameAction = menu.addAction(tr("Rename…"));
    menu.addSeparator();
    QAction* deleteAction = menu.addAction(tr("Delete Category"));
    for (QAction* a : {editAction, renameAction, deleteAction}) {
        a->setEnabled(user);
    }
    menu.addSeparator();
    QAction* buildAction = menu.addAction(tr("Build Category…"));
    QAction* subAction = menu.addAction(tr("Build Subcategory…"));
    subAction->setEnabled(user);
    subAction->setToolTip(tr("Build a new category inside the selected one"));
    // Move the selected category under another one (or back to the top level).
    const qint64 selId = selected ? selected->id : 0;
    const qint64 selParent = selected ? selected->parentId : 0;
    QMenu* moveMenu = menu.addMenu(tr("Move Category To"));
    moveMenu->setEnabled(user);
    QHash<QAction*, qint64> moveActions;
    QAction* toTop = nullptr;
    if (user) {
        toTop = moveMenu->addAction(tr("Top level"));
        toTop->setEnabled(selParent > 0);
        moveMenu->addSeparator();
        QSet<qint64> excluded = categoryDescendantIds(m_filters, selId);
        excluded.insert(selId); // never into itself or one of its own subcategories
        addCategoryTree(moveMenu, 0, excluded, moveActions, [selParent](const PlaylistFilterInfo& f, QAction* a) {
            a->setEnabled(f.id != selParent);
        });
    }
    QAction* chosen = menu.exec(globalPos);
    if (chosen && chosen == toTop) {
        onMoveCategory(selId, 0);
        return;
    }
    if (chosen && moveActions.contains(chosen)) {
        onMoveCategory(selId, moveActions.value(chosen));
        return;
    }
    if (chosen == subAction) {
        onBuildCategory(selId);
        return;
    }
    if (chosen == editAction) {
        onEditCategory();
    } else if (chosen == renameAction) {
        onRenameCategory();
    } else if (chosen == deleteAction) {
        onDeleteCategory();
    } else if (chosen == buildAction) {
        onBuildCategory(0);
    }
}

void PlaylistDialog::addCategoryTree(QMenu* menu, qint64 parentId, const QSet<qint64>& excluded,
                                     QHash<QAction*, qint64>& actions,
                                     const std::function<void(const PlaylistFilterInfo&, QAction*)>& decorate) {
    QSet<qint64> known;
    for (const PlaylistFilterInfo& f : std::as_const(m_filters)) {
        known.insert(f.id);
    }
    const auto label = [](const QString& name) { return QString(name).replace(QLatin1Char('&'), QStringLiteral("&&")); };
    for (const PlaylistFilterInfo& f : std::as_const(m_filters)) {
        // A category whose parent is unknown is shown at the top level.
        const qint64 effectiveParent = known.contains(f.parentId) ? f.parentId : 0;
        if (effectiveParent != parentId || excluded.contains(f.id)) {
            continue;
        }
        bool hasChildren = false;
        for (const PlaylistFilterInfo& c : std::as_const(m_filters)) {
            hasChildren = hasChildren || (c.parentId == f.id && !excluded.contains(c.id));
        }
        if (!hasChildren) {
            QAction* a = menu->addAction(label(f.name));
            actions.insert(a, f.id);
            decorate(f, a);
            continue;
        }
        QMenu* sub = menu->addMenu(label(f.name));
        sub->setToolTipsVisible(true);
        QAction* self = sub->addAction(label(f.name));
        actions.insert(self, f.id);
        decorate(f, self);
        sub->addSeparator();
        addCategoryTree(sub, f.id, excluded, actions, decorate);
    }
}

void PlaylistDialog::onMoveCategory(qint64 categoryId, qint64 newParentId) {
    PlaylistModel* m = current();
    if (!m || categoryId <= 0) {
        return;
    }
    if (m_library->setCategoryParent(categoryId, m->id(), newParentId)) {
        showNotice(newParentId > 0 ? tr("Category moved. No items were changed.")
                                   : tr("Category moved to the top level. No items were changed."));
    }
}

void PlaylistDialog::assignItemsToCategory(qint64 categoryId, const QSet<qint64>& itemIds) {
    PlaylistModel* m = current();
    if (!m || itemIds.isEmpty()) {
        return;
    }
    QString name;
    for (const PlaylistFilterInfo& f : std::as_const(m_filters)) {
        if (f.id == categoryId) {
            name = f.name;
        }
    }
    const int added = m_library->addItemsToCategory(categoryId, m->id(), itemIds);
    if (added < 0) {
        return; // errorOccurred already explained why
    }
    if (added == 0) {
        showNotice(tr("Already in \"%1\" - nothing changed.").arg(name));
        return;
    }
    announce(int(Notification::Kind::Success), tr("Added to Category"),
             added == 1 ? tr("1 item was added to \"%1\". The playlist itself is unchanged.").arg(name)
                        : tr("%1 items were added to \"%2\". The playlist itself is unchanged.").arg(added).arg(name),
             QStringLiteral("category-assigned:%1").arg(categoryId),
             tr("Added to category \"%1\".").arg(name));
}

void PlaylistDialog::onNewCategoryFromItems(const QSet<qint64>& itemIds) {
    PlaylistModel* m = current();
    if (!m || itemIds.isEmpty()) {
        return;
    }
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, tr("New Category"), tr("Category name:"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    if (name.compare(QLatin1String("All"), Qt::CaseInsensitive) == 0 || name.compare(tr("All"), Qt::CaseInsensitive) == 0) {
        showNotice(tr("\"All\" is built in - choose another name."));
        return;
    }
    for (const PlaylistFilterInfo& f : std::as_const(m_filters)) {
        if (f.name.compare(name, Qt::CaseInsensitive) == 0) {
            showNotice(tr("This playlist already has a category called \"%1\".").arg(f.name));
            return;
        }
    }
    const qint64 id = m_library->createFilter(m->id(), name, FilterDefinition{}, QStringLiteral("selected"));
    if (id <= 0) {
        return;
    }
    if (!m_library->setCategoryItems(id, m->id(), itemIds)) {
        m_library->deleteFilter(id, m->id()); // no half-made category
        return;
    }
    showNotice(tr("Category \"%1\" created with the selected item. The playlist itself is unchanged.").arg(name));
}

void PlaylistDialog::onBuildCategory(qint64 parentId) {
    PlaylistModel* m = current();
    if (!m) {
        return;
    }
    QString parentName;
    for (const PlaylistFilterInfo& f : std::as_const(m_filters)) {
        if (parentId > 0 && f.id == parentId) {
            parentName = f.name;
        }
    }
    if (parentId > 0 && parentName.isEmpty()) {
        parentId = 0; // the parent vanished meanwhile: build a top-level category
    }
    // Two ways to build a category: saved conditions, or a hand-picked
    // selection of this playlist's items.
    QMessageBox choice(QMessageBox::Question, parentId > 0 ? tr("Build Subcategory") : tr("Build Category"),
                       parentId > 0 ? tr("How do you want to build a subcategory of \"%1\"?").arg(parentName)
                                    : tr("How do you want to build a category for \"%1\"?").arg(m->name()),
                       QMessageBox::NoButton, this);
    choice.setInformativeText(tr("Conditions: a saved filter, e.g. file names containing \"anime\". Matching items "
                                 "are included automatically.\n\nSelect from Playlist: tick the items of this playlist "
                                 "yourself. Nothing is copied."));
    QPushButton* byConditions = choice.addButton(tr("Conditions"), QMessageBox::AcceptRole);
    QPushButton* bySelection = choice.addButton(tr("Select from Playlist"), QMessageBox::AcceptRole);
    choice.addButton(QMessageBox::Cancel);
    choice.exec();
    if (choice.clickedButton() == bySelection) {
        CategoryItemPickerDialog picker(m_library, m, QString(), {}, 0, this);
        if (picker.exec() != QDialog::Accepted) {
            return;
        }
        const qint64 id = m_library->createFilter(m->id(), picker.categoryName(), FilterDefinition{},
                                                  QStringLiteral("selected"));
        if (id <= 0) {
            return; // errorOccurred already explained it
        }
        if (!m_library->setCategoryItems(id, m->id(), picker.selectedItemIds())) {
            m_library->deleteFilter(id, m->id()); // no half-made category
            return;
        }
        if (parentId > 0) {
            m_library->setCategoryParent(id, m->id(), parentId);
        }
        reloadCategories(false);
        m_categoryPicker->setCurrentCategory(id);
        showNotice(tr("Category created from your selection. Nothing was copied - the playlist is unchanged."));
        return;
    }
    if (choice.clickedButton() != byConditions) {
        return;
    }
    CategoryFilterDialog dialog(m_library, m, nullptr, this);
    if (dialog.exec() == QDialog::Accepted) {
        if (parentId > 0) {
            m_library->setCategoryParent(dialog.filterId(), m->id(), parentId);
        }
        reloadCategories(false);
        m_categoryPicker->setCurrentCategory(dialog.filterId());
        showNotice(tr("Category created. It's a saved filter - the playlist itself is unchanged."));
    }
}

void PlaylistDialog::onEditCategory() {
    PlaylistModel* m = current();
    const PlaylistFilterInfo* filter = currentFilter();
    if (!m || !filter) {
        return;
    }
    const PlaylistFilterInfo copy = *filter;
    if (copy.isSelection()) {
        CategoryItemPickerDialog picker(m_library, m, copy.name, copy.itemIds, copy.id, this);
        if (picker.exec() == QDialog::Accepted && m_library->setCategoryItems(copy.id, m->id(), picker.selectedItemIds())) {
            reloadCategories(false);
        }
        return;
    }
    CategoryFilterDialog dialog(m_library, m, &copy, this);
    if (dialog.exec() == QDialog::Accepted) {
        reloadCategories(false);
    }
}

void PlaylistDialog::onRenameCategory() {
    PlaylistModel* m = current();
    const PlaylistFilterInfo* filter = currentFilter();
    if (!m || !filter) {
        return;
    }
    const PlaylistFilterInfo copy = *filter;
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, tr("Rename Category"), tr("Category name:"), QLineEdit::Normal, copy.name, &ok).trimmed();
    if (!ok || name.isEmpty() || name == copy.name) {
        return;
    }
    if (name.compare(QLatin1String("All"), Qt::CaseInsensitive) == 0 || name.compare(tr("All"), Qt::CaseInsensitive) == 0) {
        showNotice(tr("\"All\" is built in - choose another name."));
        return;
    }
    for (const PlaylistFilterInfo& f : std::as_const(m_filters)) {
        if (f.id != copy.id && f.name.compare(name, Qt::CaseInsensitive) == 0) {
            showNotice(tr("This playlist already has a category called \"%1\".").arg(f.name));
            return;
        }
    }
    m_library->updateFilter(copy.id, m->id(), name, copy.definition);
}

void PlaylistDialog::onDeleteCategory() {
    PlaylistModel* m = current();
    const PlaylistFilterInfo* filter = currentFilter();
    if (!m || !filter) {
        return;
    }
    const PlaylistFilterInfo copy = *filter;
    const auto answer = QMessageBox::question(
        this, tr("Delete Category"),
        tr("Delete the category \"%1\"?\n\nOnly the saved filter is removed. Every item stays in \"%2\".")
                .arg(copy.name, m->name()) +
            (categoryDescendantIds(m_filters, copy.id).isEmpty()
                 ? QString()
                 : tr("\n\nIts subcategories are kept and move up one level.")) +
            (m_library->activeId() == m->id() && m_library->activeCategoryId() == copy.id
                 ? tr("\n\nThis category is the current wallpaper source; Motiva will stop rotating until you choose another.")
                 : QString()),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer == QMessageBox::Yes && m_library->deleteFilter(copy.id, m->id())) {
        m_categoryPicker->setCurrentCategory(0);
        showNotice(tr("Deleted category \"%1\". No items were removed.").arg(copy.name));
    }
}
