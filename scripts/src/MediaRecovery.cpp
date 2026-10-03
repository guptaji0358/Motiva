#include "MediaRecovery.h"

#include <QDebug>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>
#include <QTimeZone>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <windows.h>

namespace {

constexpr int kMaxCandidates = 50;

// Folder names never descended into during the drive-wide phases: system,
// installed-program and per-app data locations that don't hold the user's
// own media and are expensive (or access-denied) to walk.
bool isSkippedFolderName(const QString& name) {
    static const QSet<QString> kSkipped = {
        QStringLiteral("windows"), QStringLiteral("windows.old"), QStringLiteral("program files"),
        QStringLiteral("program files (x86)"), QStringLiteral("programdata"), QStringLiteral("appdata"),
        QStringLiteral("$recycle.bin"), QStringLiteral("system volume information"), QStringLiteral("recovery"),
        QStringLiteral("perflogs"), QStringLiteral("$windows.~bt"), QStringLiteral("$windows.~ws"),
        QStringLiteral("$sysreset"), QStringLiteral("msocache"), QStringLiteral("config.msi"),
        QStringLiteral("node_modules"), QStringLiteral(".git"), QStringLiteral(".svn"), QStringLiteral(".cache"),
    };
    return kSkipped.contains(name.toLower());
}

QString dirKey(const QString& path) {
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
}

class Searcher {
public:
    Searcher(const QString& originalPath, const MediaFacts& facts, std::shared_ptr<MediaSearchProgress> progress)
        : m_fileName(QFileInfo(originalPath).fileName()), m_suffix(QFileInfo(originalPath).suffix()),
          m_facts(facts), m_progress(std::move(progress)) {
        // The original folder chain may itself sit inside a normally-skipped
        // location (e.g. AppData); those specific ancestors stay allowed.
        QDir d = QFileInfo(originalPath).absoluteDir();
        while (true) {
            m_allowedAncestors.insert(dirKey(d.absolutePath()));
            if (!d.cdUp()) {
                break;
            }
        }
    }

    // Walks `root` (iteratively), collecting candidates. Returns false if
    // cancelled.
    bool scan(const QString& root) {
        QVector<QString> stack{root};
        while (!stack.isEmpty()) {
            if (m_progress->cancel.load()) {
                return false;
            }
            const QString dir = stack.takeLast();
            const QString key = dirKey(dir);
            if (m_visited.contains(key)) {
                continue;
            }
            m_visited.insert(key);
            m_progress->foldersScanned.fetch_add(1);

            QDirIterator it(dir, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
            int sinceCancelCheck = 0;
            while (it.hasNext()) {
                const QFileInfo fi = it.nextFileInfo();
                if (++sinceCancelCheck >= 256) {
                    sinceCancelCheck = 0;
                    if (m_progress->cancel.load()) {
                        return false;
                    }
                }
                if (fi.isDir()) {
                    // No junctions/symlinks: avoids loops and double visits.
                    if (fi.isSymLink() || fi.isJunction()) {
                        continue;
                    }
                    const QString childKey = dirKey(fi.absoluteFilePath());
                    const bool allowed = m_allowedAncestors.contains(childKey);
                    if (!allowed && (isSkippedFolderName(fi.fileName()) ||
                                     (fi.isHidden() && fi.fileName().startsWith(QLatin1Char('$'))))) {
                        continue;
                    }
                    stack.push_back(fi.absoluteFilePath());
                    continue;
                }
                consider(fi);
            }
        }
        return true;
    }

    bool hasConvincing() const {
        for (const MediaCandidate& c : m_found) {
            if (c.isConvincing()) {
                return true;
            }
        }
        return false;
    }

    QVector<MediaCandidate> results() const { return m_found; }
    bool full() const { return m_found.size() >= kMaxCandidates; }

private:
    void consider(const QFileInfo& fi) {
        const bool sameName = fi.fileName().compare(m_fileName, Qt::CaseInsensitive) == 0;
        const bool sameSuffix = fi.suffix().compare(m_suffix, Qt::CaseInsensitive) == 0;
        if (!sameName && !sameSuffix) {
            return;
        }
        const qint64 size = fi.size();
        const bool sameSize = m_facts.isKnown() && size == m_facts.size;
        const QDateTime modified = fi.lastModified(QTimeZone::UTC);
        const bool sameModified = m_facts.isKnown() && m_facts.modified.isValid() &&
                                  qAbs(modified.secsTo(m_facts.modified)) <= 2;
        // A differently-named file only counts if it is byte-size and
        // timestamp identical (a renamed copy) - never on extension alone.
        if (!sameName && !(sameSize && sameModified && size > 0)) {
            return;
        }
        if (m_found.size() >= kMaxCandidates) {
            return;
        }
        const QString path = QDir::toNativeSeparators(fi.absoluteFilePath());
        for (const MediaCandidate& c : m_found) {
            if (c.path.compare(path, Qt::CaseInsensitive) == 0) {
                return;
            }
        }
        m_found.push_back({path, size, modified, sameName, sameSize, sameModified});
    }

    QString m_fileName;
    QString m_suffix;
    MediaFacts m_facts;
    std::shared_ptr<MediaSearchProgress> m_progress;
    QSet<QString> m_visited;
    QSet<QString> m_allowedAncestors;
    QVector<MediaCandidate> m_found;
};

QStringList searchDrives() {
    QStringList roots;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) {
            continue;
        }
        const QString root = QStringLiteral("%1:/").arg(QChar('A' + i));
        const std::wstring w = QDir::toNativeSeparators(root).toStdWString();
        const UINT type = GetDriveTypeW(w.c_str());
        // Local disks and removable drives (USB sticks, SD cards) only - no
        // network shares or optical media.
        if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) {
            roots << root;
        }
    }
    return roots;
}

QString formatSize(qint64 bytes) {
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

} // namespace

QVector<MediaCandidate> MediaRecovery::search(const QString& originalPath, const MediaFacts& facts,
                                              std::shared_ptr<MediaSearchProgress> progress) {
    Searcher searcher(originalPath, facts, progress);

    // 1. Nearest existing folder of the original location.
    // (Walked as strings: QDir::cdUp() refuses to step through folders that
    // no longer exist, which is exactly the case when a folder was moved.)
    QString nearest = QFileInfo(originalPath).absolutePath();
    while (!QFileInfo(nearest).isDir()) {
        const QString parent = QFileInfo(nearest).absolutePath();
        if (parent == nearest) {
            break;
        }
        nearest = parent;
    }
    QStringList phases[3];
    if (QFileInfo(nearest).isDir() && !QDir(nearest).isRoot()) {
        phases[0] << nearest;
    }
    // 2. The user's media folders.
    for (auto loc : {QStandardPaths::PicturesLocation, QStandardPaths::MoviesLocation, QStandardPaths::DesktopLocation,
                     QStandardPaths::DownloadLocation, QStandardPaths::DocumentsLocation}) {
        const QString p = QStandardPaths::writableLocation(loc);
        if (!p.isEmpty()) {
            phases[1] << p;
        }
    }
    const QString oneDrive = qEnvironmentVariable("OneDrive");
    if (!oneDrive.isEmpty()) {
        phases[1] << oneDrive;
    }
    // 3. Every local drive.
    phases[2] = searchDrives();

    for (const QStringList& phase : phases) {
        for (const QString& root : phase) {
            if (!searcher.scan(root) || searcher.full()) {
                return searcher.results();
            }
        }
        if (searcher.hasConvincing()) {
            break; // the same file (name+size+time) - no need to walk every drive
        }
    }
    return searcher.results();
}

// ---------------------------------------------------------------- dialog

MediaRecoveryDialog::MediaRecoveryDialog(const QString& originalPath, const MediaFacts& facts, QWidget* parent)
    : QDialog(parent), m_originalPath(originalPath), m_facts(facts),
      m_progress(std::make_shared<MediaSearchProgress>()) {
    const QString name = QFileInfo(originalPath).fileName();
    setWindowTitle(tr("Find File"));
    setMinimumWidth(560);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(12);
    m_pages = new QStackedWidget(this);
    root->addWidget(m_pages);

    // Page 0: searching
    auto* searching = new QWidget(m_pages);
    auto* sCol = new QVBoxLayout(searching);
    sCol->setContentsMargins(0, 0, 0, 0);
    sCol->setSpacing(10);
    auto* title = new QLabel(tr("Searching for \"%1\"…").arg(name), searching);
    QFont tf = title->font();
    tf.setBold(true);
    title->setFont(tf);
    sCol->addWidget(title);
    auto* was = new QLabel(tr("Last known location: %1").arg(QDir::toNativeSeparators(originalPath)), searching);
    was->setObjectName(QStringLiteral("secondaryText"));
    was->setWordWrap(true);
    sCol->addWidget(was);
    m_progressBar = new QProgressBar(searching);
    m_progressBar->setRange(0, 0); // indeterminate: the total folder count isn't known in advance
    m_progressBar->setTextVisible(false);
    m_progressBar->setMaximumHeight(6);
    sCol->addWidget(m_progressBar);
    m_statusLabel = new QLabel(tr("Searching your drives…"), searching);
    m_statusLabel->setObjectName(QStringLiteral("secondaryText"));
    sCol->addWidget(m_statusLabel);
    auto* sButtons = new QDialogButtonBox(QDialogButtonBox::Cancel, searching);
    connect(sButtons, &QDialogButtonBox::rejected, this, &MediaRecoveryDialog::reject);
    sCol->addWidget(sButtons);
    m_pages->addWidget(searching);

    // Page 1: choose among several / weak matches
    auto* choose = new QWidget(m_pages);
    auto* cCol = new QVBoxLayout(choose);
    cCol->setContentsMargins(0, 0, 0, 0);
    cCol->setSpacing(10);
    auto* chooseTitle = new QLabel(tr("Which file is \"%1\"?").arg(name), choose);
    chooseTitle->setFont(tf);
    cCol->addWidget(chooseTitle);
    auto* chooseHint = new QLabel(tr("Motiva found more than one possible match, or none it can be sure about. "
                                     "Choose the right file, or cancel."),
                                  choose);
    chooseHint->setObjectName(QStringLiteral("secondaryText"));
    chooseHint->setWordWrap(true);
    cCol->addWidget(chooseHint);
    m_choices = new QTreeWidget(choose);
    m_choices->setRootIsDecorated(false);
    m_choices->setUniformRowHeights(true);
    m_choices->setHeaderLabels({tr("Name"), tr("Folder"), tr("Size"), tr("Modified"), tr("Match")});
    m_choices->header()->setStretchLastSection(false);
    m_choices->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_choices->setMinimumHeight(180);
    connect(m_choices, &QTreeWidget::itemSelectionChanged, this,
            [this] { m_useButton->setEnabled(!m_choices->selectedItems().isEmpty()); });
    connect(m_choices, &QTreeWidget::itemDoubleClicked, this, [this] { m_useButton->click(); });
    cCol->addWidget(m_choices, 1);
    auto* cButtons = new QHBoxLayout();
    cButtons->addStretch();
    m_useButton = new QPushButton(tr("Use This File"), choose);
    m_useButton->setObjectName(QStringLiteral("primaryButton"));
    m_useButton->setEnabled(false);
    connect(m_useButton, &QPushButton::clicked, this, [this] {
        const auto selected = m_choices->selectedItems();
        if (!selected.isEmpty()) {
            m_chosen = selected.first()->data(0, Qt::UserRole).toString();
            done(Found);
        }
    });
    cButtons->addWidget(m_useButton);
    auto* cancel = new QPushButton(tr("Cancel"), choose);
    connect(cancel, &QPushButton::clicked, this, &MediaRecoveryDialog::reject);
    cButtons->addWidget(cancel);
    cCol->addLayout(cButtons);
    m_pages->addWidget(choose);

    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(250); // display refresh only - the search itself is event-free work on a worker
    connect(m_progressTimer, &QTimer::timeout, this, &MediaRecoveryDialog::updateProgressText);
    connect(&m_watcher, &QFutureWatcher<QVector<MediaCandidate>>::finished, this, &MediaRecoveryDialog::onSearchFinished);
}

MediaRecoveryDialog::~MediaRecoveryDialog() {
    // The worker owns its own copy of everything it needs (shared_ptr
    // progress, path, facts); cancelling is enough - it stops at its next
    // check and its result is simply dropped.
    m_progress->cancel = true;
}

void MediaRecoveryDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (!m_started) {
        m_started = true;
        startSearch();
    }
}

void MediaRecoveryDialog::reject() {
    m_progress->cancel = true;
    qInfo() << "[Recovery] Search cancelled for" << m_originalPath;
    QDialog::reject();
}

void MediaRecoveryDialog::startSearch() {
    qInfo() << "[Recovery] Searching for" << m_originalPath << "size=" << m_facts.size << "modified=" << m_facts.modified;
    const QString path = m_originalPath;
    const MediaFacts facts = m_facts;
    auto progress = m_progress;
    m_watcher.setFuture(QtConcurrent::run([path, facts, progress] { return MediaRecovery::search(path, facts, progress); }));
    m_progressTimer->start();
}

void MediaRecoveryDialog::updateProgressText() {
    m_statusLabel->setText(tr("Searching your drives… %1 folders checked").arg(m_progress->foldersScanned.load()));
}

void MediaRecoveryDialog::onSearchFinished() {
    m_progressTimer->stop();
    if (m_progress->cancel.load()) {
        return;
    }
    const QVector<MediaCandidate> found = m_watcher.result();
    QVector<MediaCandidate> convincing;
    for (const MediaCandidate& c : found) {
        if (c.isConvincing()) {
            convincing << c;
        }
    }
    qInfo() << "[Recovery] Search finished:" << m_progress->foldersScanned.load() << "folders," << found.size()
            << "candidate(s)," << convincing.size() << "convincing.";
    if (convincing.size() == 1) {
        m_chosen = convincing.first().path; // exactly one: same name, size and time
        done(Found);
        return;
    }
    if (!found.isEmpty()) {
        showChoices(convincing.size() > 1 ? convincing : found);
        return;
    }
    done(NotFound);
}

void MediaRecoveryDialog::showChoices(const QVector<MediaCandidate>& candidates) {
    m_choices->clear();
    for (const MediaCandidate& c : candidates) {
        const QFileInfo fi(c.path);
        QString match;
        if (c.isConvincing()) {
            match = tr("Same name, size and date");
        } else if (c.sameName && c.sameSize) {
            match = tr("Same name and size");
        } else if (c.sameName) {
            match = m_facts.isKnown() ? tr("Same name, different size") : tr("Same name");
        } else {
            match = tr("Renamed copy (same size and date)");
        }
        auto* item = new QTreeWidgetItem(m_choices, {fi.fileName(), QDir::toNativeSeparators(fi.absolutePath()),
                                                     formatSize(c.size),
                                                     QLocale().toString(c.modified.toLocalTime(), QLocale::ShortFormat),
                                                     match});
        item->setData(0, Qt::UserRole, c.path);
        item->setToolTip(1, c.path);
    }
    for (int col : {0, 2, 3, 4}) {
        m_choices->resizeColumnToContents(col);
    }
    m_pages->setCurrentIndex(1);
    resize(qMax(width(), 760), qMax(height(), 360));
}
