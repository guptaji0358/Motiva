#include "MediaPreview.h"

#include "PlaylistModel.h"

#include <QAudioOutput>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMediaPlayer>
#include <QPainter>
#include <QScreen>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QWidget>
#include <QWindow>

namespace {

constexpr Qt::WindowFlags kFlags = Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint;

QString closeHint() {
    return QStringLiteral("   -   Esc or click to close");
}

// Borderless full-screen viewer; any click or key closes it.
class ImageViewer : public QWidget {
public:
    ImageViewer(const QImage& image, const QString& caption, QScreen* screen)
        : QWidget(nullptr, kFlags), m_image(image), m_caption(caption) {
        setAttribute(Qt::WA_DeleteOnClose);
        setCursor(Qt::PointingHandCursor);
        if (screen) {
            setGeometry(screen->geometry());
        }
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        QRect r(QPoint(0, 0), m_image.size().scaled(size(), Qt::KeepAspectRatio));
        r.moveCenter(rect().center());
        p.drawImage(r, m_image);
        p.setPen(QColor(255, 255, 255, 190));
        p.drawText(rect().adjusted(16, 0, -16, -12), Qt::AlignBottom | Qt::AlignHCenter, m_caption + closeHint());
    }
    void mousePressEvent(QMouseEvent*) override { close(); }
    void keyPressEvent(QKeyEvent*) override { close(); }

private:
    QImage m_image;
    QString m_caption;
};

// Full-screen looping video, muted. Space pauses/resumes; click or any other key closes.
class VideoViewer : public QWidget {
public:
    VideoViewer(const QString& path, QScreen* screen) : QWidget(nullptr, kFlags) {
        setAttribute(Qt::WA_DeleteOnClose);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setStyleSheet(QStringLiteral("background:black;"));
        if (screen) {
            setGeometry(screen->geometry());
        }
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        m_video = new QVideoWidget(this);
        m_video->setFocusPolicy(Qt::NoFocus);
        m_video->installEventFilter(this);
        layout->addWidget(m_video, 1);
        auto* caption = new QLabel(QFileInfo(path).fileName() + closeHint() + QStringLiteral("   -   Space to pause"), this);
        caption->setAlignment(Qt::AlignCenter);
        caption->setStyleSheet(QStringLiteral("color:rgba(255,255,255,190);background:black;padding:8px;"));
        layout->addWidget(caption);

        m_player = new QMediaPlayer(this);
        auto* audio = new QAudioOutput(this);
        audio->setMuted(true); // a preview must never start making noise
        m_player->setAudioOutput(audio);
        m_player->setVideoOutput(m_video);
        m_player->setLoops(QMediaPlayer::Infinite);
        m_player->setSource(QUrl::fromLocalFile(path));
        m_player->play();
    }
    ~VideoViewer() override {
        m_player->stop();
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == m_video && e->type() == QEvent::MouseButtonPress) {
            close();
            return true;
        }
        return QWidget::eventFilter(o, e);
    }
    void mousePressEvent(QMouseEvent*) override { close(); }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Space) {
            m_player->playbackState() == QMediaPlayer::PlayingState ? m_player->pause() : m_player->play();
            return;
        }
        close();
    }

private:
    QVideoWidget* m_video = nullptr;
    QMediaPlayer* m_player = nullptr;
};

} // namespace

namespace MediaPreview {

QString show(const QString& path, QWidget* anchor) {
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        return QObject::tr("That file can't be found on disk, so it can't be previewed.");
    }
    QScreen* screen = anchor && anchor->windowHandle() ? anchor->windowHandle()->screen() : QGuiApplication::primaryScreen();
    QWidget* viewer = nullptr;
    if (PlaylistModel::isVideoFile(path)) {
        viewer = new VideoViewer(path, screen);
    } else if (PlaylistModel::isImageFile(path)) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QSize want = screen ? (screen->size() * screen->devicePixelRatio()) : QSize(1920, 1080);
        if (reader.size().isValid() && (reader.size().width() > want.width() || reader.size().height() > want.height())) {
            reader.setScaledSize(reader.size().scaled(want, Qt::KeepAspectRatio));
        }
        const QImage image = reader.read();
        if (image.isNull()) {
            return QObject::tr("This image could not be opened.");
        }
        viewer = new ImageViewer(image, QFileInfo(path).fileName(), screen);
    } else {
        return QObject::tr("Only images and videos can be previewed.");
    }
    viewer->showFullScreen();
    viewer->activateWindow();
    viewer->setFocus();
    return QString();
}

} // namespace MediaPreview
