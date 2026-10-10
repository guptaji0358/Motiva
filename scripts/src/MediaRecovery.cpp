#include "MediaRecovery.h"

#include "Theme.h"

#include <QColor>
#include <QCoreApplication>
#include <QDebug>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
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
#include <exception>

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

// Cross-operation cache of where files were last found, by case-folded file
// name. Entries are only hints: each one is re-checked against the disk
// (exists + the request's own name/size/time rules) before it is trusted, so
// a stale entry costs one stat call and can never produce a wrong result.
// Holds paths only - no file data.
constexpr int kMaxCacheEntries = 4096;
QMutex g_cacheMutex;
QHash<QString, QStringList> g_foundCache;

// True when a directory that listed nothing is actually unreadable (access
// denied, drive gone/not ready) rather than just empty. Only called for
// empty listings, so a normal scan never pays for it.
bool isUnreadableDirectory(const QString& dir) {
    QString native = QDir::toNativeSeparators(dir);
    if (!native.startsWith(QLatin1String("\\\\"))) {
        native.prepend(QLatin1String("\\\\?\\")); // long-path safe
    }
    if (!native.endsWith(QLatin1Char('\\'))) {
        native += QLatin1Char('\\');
    }
    native += QLatin1Char('*');
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(reinterpret_cast<LPCWSTR>(native.utf16()), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError() != ERROR_FILE_NOT_FOUND;
    }
    FindClose(h);
    return false;
}

// Matches every directory entry against ALL requested files at once. Files
// are looked up by case-folded name (and, for a renamed copy, by suffix then
// size+time), so the cost of one more request is a hash entry, not another
// walk of the disk.
class MultiSearcher {
public:
    MultiSearcher(const QVector<MediaSearchRequest>& requests, std::shared_ptr<MediaSearchProgress> progress)
        : m_progress(std::move(progress)) {
        m_targets.resize(requests.size());
        for (int i = 0; i < requests.size(); ++i) {
            const QFileInfo orig(requests[i].path);
            Target& t = m_targets[i];
            t.fileName = orig.fileName();
            t.nameKey = t.fileName.toCaseFolded();
            t.facts = requests[i].facts;
            if (!t.nameKey.isEmpty()) {
                m_byName[t.nameKey].push_back(i);
            }
            // A renamed copy is only ever recognised by identical size+time,
            // so only requests with known facts take part in suffix matching.
            if (t.facts.isKnown() && t.facts.modified.isValid() && t.facts.size > 0) {
                m_bySuffix[orig.suffix().toCaseFolded()].push_back(i);
            }
            // The original folder chain may itself sit inside a normally-
            // skipped location (e.g. AppData); those ancestors stay allowed.
            QDir d = orig.absoluteDir();
            while (true) {
                m_allowedAncestors.insert(dirKey(d.absolutePath()));
                if (!d.cdUp()) {
                    break;
                }
            }
        }
    }

    // Re-checks previously found locations for these names; no directory is
    // walked. Returns true if every target is now settled.
    bool applyCache() {
        QHash<QString, QStringList> hints;
        {
            QMutexLocker lock(&g_cacheMutex);
            for (const Target& t : std::as_const(m_targets)) {
                const auto it = g_foundCache.constFind(t.nameKey);
                if (it != g_foundCache.constEnd()) {
                    hints.insert(t.nameKey, it.value());
                }
            }
        }
        for (int i = 0; i < m_targets.size(); ++i) {
            const QStringList paths = hints.value(m_targets[i].nameKey);
            for (const QString& p : paths) {
                const QFileInfo fi(p);
                if (fi.isFile()) {
                    evaluate(i, fi);
                }
            }
        }
        endPhase();
        return allSettled();
    }

    // Walks `root` (iteratively), matching every file against all targets.
    // Returns false if cancelled.
    bool scan(const QString& root) {
        QVector<QString> stack{root};
        int pendingFiles = 0;
        auto flushFiles = [&] {
            if (pendingFiles) {
                m_progress->filesExamined.fetch_add(pendingFiles);
                pendingFiles = 0;
            }
        };
        while (!stack.isEmpty()) {
            if (m_progress->cancel.load()) {
                flushFiles();
                return false;
            }
            const QString dir = stack.takeLast();
            const QString key = dirKey(dir);
            if (m_visited.contains(key)) {
                continue; // overlapping roots / nested requests: never listed twice
            }
            m_visited.insert(key);
            m_progress->foldersScanned.fetch_add(1);

            QDirIterator it(dir, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
            int sinceCancelCheck = 0;
            int entries = 0;
            while (it.hasNext()) {
                const QFileInfo fi = it.nextFileInfo();
                ++entries;
                if (++sinceCancelCheck >= 256) {
                    sinceCancelCheck = 0;
                    flushFiles();
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
                ++pendingFiles;
                consider(fi);
            }
            if (entries == 0 && isUnreadableDirectory(dir)) {
                noteInaccessible(dir);
            }
        }
        flushFiles();
        return true;
    }

    // Called between phases: a target with a convincing match is settled and
    // stops being matched (no later phase can improve on "same file").
    void endPhase() {
        for (Target& t : m_targets) {
            if (t.hasConvincing || t.found.size() >= kMaxCandidates) {
                t.settled = true;
            }
        }
    }

    bool allSettled() const {
        for (const Target& t : m_targets) {
            if (!t.settled) {
                return false;
            }
        }
        return true;
    }

    void noteInaccessible(const QString& dir) {
        m_progress->inaccessibleFolders.fetch_add(1);
        if (m_inaccessible.size() < 20) {
            m_inaccessible << QDir::toNativeSeparators(dir);
        }
    }

    MediaBatchResult takeResult(bool cancelled) {
        MediaBatchResult r;
        r.cancelled = cancelled;
        r.inaccessible = m_inaccessible;
        r.candidates.reserve(m_targets.size());
        for (const Target& t : std::as_const(m_targets)) {
            r.candidates.push_back(t.found);
        }
        return r;
    }

    // Remember convincing finds for later operations.
    void storeInCache() const {
        QMutexLocker lock(&g_cacheMutex);
        if (g_foundCache.size() > kMaxCacheEntries) {
            g_foundCache.clear();
        }
        for (const Target& t : m_targets) {
            QStringList paths;
            for (const MediaCandidate& c : t.found) {
                if (c.isConvincing()) {
                    paths << c.path;
                }
            }
            if (!paths.isEmpty()) {
                // Merge: two requests may share a file name (different folders).
                QStringList& entry = g_foundCache[t.nameKey];
                for (const QString& p : std::as_const(paths)) {
                    if (!entry.contains(p, Qt::CaseInsensitive)) {
                        entry << p;
                    }
                }
            }
        }
    }

private:
    struct Target {
        QString fileName;
        QString nameKey;
        MediaFacts facts;
        QVector<MediaCandidate> found;
        bool hasConvincing = false;
        bool settled = false;
    };

    void consider(const QFileInfo& fi) {
        const auto nameIt = m_byName.constFind(fi.fileName().toCaseFolded());
        const auto sufIt = m_bySuffix.constFind(fi.suffix().toCaseFolded());
        const bool byName = nameIt != m_byName.constEnd();
        const bool bySuffix = sufIt != m_bySuffix.constEnd();
        if (!byName && !bySuffix) {
            return;
        }
        if (byName) {
            for (int i : *nameIt) {
                evaluate(i, fi);
            }
        }
        if (bySuffix) {
            for (int i : *sufIt) {
                if (!byName || !nameIt->contains(i)) {
                    evaluate(i, fi);
                }
            }
        }
    }

    void evaluate(int index, const QFileInfo& fi) {
        Target& t = m_targets[index];
        if (t.settled || t.found.size() >= kMaxCandidates) {
            return;
        }
        const bool sameName = fi.fileName().toCaseFolded() == t.nameKey;
        const qint64 size = fi.size();
        const bool sameSize = t.facts.isKnown() && size == t.facts.size;
        const QDateTime modified = fi.lastModified(QTimeZone::UTC);
        const bool sameModified = t.facts.isKnown() && t.facts.modified.isValid() &&
                                  qAbs(modified.secsTo(t.facts.modified)) <= 2;
        // A differently-named file only counts if it is byte-size and
        // timestamp identical (a renamed copy) - never on extension alone.
        if (!sameName && !(sameSize && sameModified && size > 0)) {
            return;
        }
        const QString path = QDir::toNativeSeparators(fi.absoluteFilePath());
        for (const MediaCandidate& c : std::as_const(t.found)) {
            if (c.path.compare(path, Qt::CaseInsensitive) == 0) {
                return; // same file reached twice (cache + walk, overlapping roots)
            }
        }
        MediaCandidate cand{path, size, modified, sameName, sameSize, sameModified};
        if (cand.isConvincing() && !t.hasConvincing) {
            t.hasConvincing = true;
            m_progress->matched.fetch_add(1);
        }
        t.found.push_back(cand);
    }

    std::shared_ptr<MediaSearchProgress> m_progress;
    QVector<Target> m_targets;
    QHash<QString, QVector<int>> m_byName;   // case-folded file name -> targets
    QHash<QString, QVector<int>> m_bySuffix; // case-folded suffix -> targets (renamed-copy match)
    QSet<QString> m_visited;
    QSet<QString> m_allowedAncestors;
    QStringList m_inaccessible;
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

QString MediaRecovery::unreachableReason(const QString& path) {
    const QString native = QDir::toNativeSeparators(path);
    if (native.startsWith(QLatin1String("\\\\"))) {
        return QCoreApplication::translate("MediaRecovery", "It is on a network location, which Motiva doesn't search.");
    }
    if (native.size() >= 2 && native.at(1) == QLatin1Char(':') && native.at(0).isLetter()) {
        const QChar letter = native.at(0).toUpper();
        const DWORD mask = GetLogicalDrives();
        const int bit = letter.unicode() - 'A';
        if (bit < 0 || bit >= 26 || !(mask & (1u << bit))) {
            return QCoreApplication::translate("MediaRecovery", "Drive %1: isn't connected.").arg(letter);
        }
        const std::wstring root = QStringLiteral("%1:\\").arg(letter).toStdWString();
        const UINT type = GetDriveTypeW(root.c_str());
        if (type == DRIVE_REMOTE) {
            return QCoreApplication::translate("MediaRecovery", "It is on a network drive, which Motiva doesn't search.");
        }
        if (GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return QCoreApplication::translate("MediaRecovery", "Drive %1: isn't ready.").arg(letter);
        }
    }
    return QString();
}

MediaBatchResult MediaRecovery::searchMany(const QVector<MediaSearchRequest>& requests,
                                           std::shared_ptr<MediaSearchProgress> progress,
                                           const QStringList& rootsOverride) {
    if (requests.isEmpty()) {
        return {};
    }
    // A removed card/disc must report "not ready" instead of popping a
    // system "insert a disk" dialog from this worker thread.
    DWORD previousMode = 0;
    const bool modeSet = SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previousMode) != 0;
    struct ModeRestore {
        bool set;
        DWORD prev;
        ~ModeRestore() {
            if (set) {
                SetThreadErrorMode(prev, nullptr);
            }
        }
    } restore{modeSet, previousMode};

    MultiSearcher searcher(requests, progress);
    if (searcher.applyCache()) {
        return searcher.takeResult(false); // every file re-verified at a known location: no walk at all
    }

    // Each phase's roots are the union over all requests, de-duplicated.
    QStringList phases[3];
    QSet<QString> seen;
    auto addRoot = [&](QStringList& phase, const QString& path) {
        if (!path.isEmpty() && !seen.contains(dirKey(path))) {
            seen.insert(dirKey(path));
            phase << path;
        }
    };
    if (!rootsOverride.isEmpty()) {
        for (const QString& root : rootsOverride) {
            addRoot(phases[0], root);
        }
    } else {
        // 1. Nearest existing folder of each original location.
        // (Walked as strings: QDir::cdUp() refuses to step through folders
        // that no longer exist, which is exactly the case when a folder was
        // moved.)
        for (const MediaSearchRequest& req : requests) {
            QString nearest = QFileInfo(req.path).absolutePath();
            while (!QFileInfo(nearest).isDir()) {
                const QString parent = QFileInfo(nearest).absolutePath();
                if (parent == nearest) {
                    break;
                }
                nearest = parent;
            }
            if (QFileInfo(nearest).isDir() && !QDir(nearest).isRoot()) {
                addRoot(phases[0], nearest);
            }
        }
        // 2. The user's media folders.
        for (auto loc : {QStandardPaths::PicturesLocation, QStandardPaths::MoviesLocation,
                         QStandardPaths::DesktopLocation, QStandardPaths::DownloadLocation,
                         QStandardPaths::DocumentsLocation}) {
            addRoot(phases[1], QStandardPaths::writableLocation(loc));
        }
        addRoot(phases[1], qEnvironmentVariable("OneDrive"));
        // 3. Every local drive.
        for (const QString& drive : searchDrives()) {
            phases[2] << drive; // drive roots are never nested in earlier roots
        }
    }

    // Phase-1 folders that sit inside a media folder / drive are still
    // listed once only: MultiSearcher keeps a visited set across all roots.
    bool cancelled = false;
    for (const QStringList& phase : phases) {
        for (const QString& root : phase) {
            if (!searcher.scan(root)) {
                cancelled = true;
                break;
            }
        }
        if (cancelled) {
            break;
        }
        searcher.endPhase();
        if (searcher.allSettled()) {
            break; // every file located (name+size+time) - no need to walk the rest
        }
    }
    if (!cancelled) {
        searcher.storeInCache();
    }
    return searcher.takeResult(cancelled);
}

QVector<MediaCandidate> MediaRecovery::search(const QString& originalPath, const MediaFacts& facts,
                                              std::shared_ptr<MediaSearchProgress> progress) {
    const MediaBatchResult r = searchMany({MediaSearchRequest{originalPath, facts}}, std::move(progress));
    return r.candidates.value(0);
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

// ---------------------------------------------------------- Find All at Once

namespace {
constexpr int kRoleEntry = Qt::UserRole;     // index into m_entries
constexpr int kRolePath = Qt::UserRole + 1;  // path to relocate to (checkable rows)
constexpr int kStatusIconSize = 16;

QIcon statusIcon(const char* resource, const char* color) {
    return QIcon(Theme::tintedIcon(QLatin1String(resource), QColor(QLatin1String(color)), kStatusIconSize));
}
} // namespace

BulkMediaRecoveryDialog::BulkMediaRecoveryDialog(const QVector<Entry>& entries, QWidget* parent)
    : QDialog(parent), m_entries(entries), m_progress(std::make_shared<MediaSearchProgress>()) {
    setWindowTitle(tr("Find All at Once"));
    setMinimumWidth(620);

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
    auto* title = new QLabel(tr("Searching for %n missing file(s)…", nullptr, int(m_entries.size())), searching);
    QFont tf = title->font();
    tf.setBold(true);
    title->setFont(tf);
    sCol->addWidget(title);
    auto* how = new QLabel(tr("Motiva looks for all of them in a single pass over your folders and drives."), searching);
    how->setObjectName(QStringLiteral("secondaryText"));
    how->setWordWrap(true);
    sCol->addWidget(how);
    m_progressBar = new QProgressBar(searching);
    m_progressBar->setRange(0, 0); // indeterminate: the total folder count isn't known in advance
    m_progressBar->setTextVisible(false);
    m_progressBar->setMaximumHeight(6);
    sCol->addWidget(m_progressBar);
    m_statusLabel = new QLabel(searching);
    sCol->addWidget(m_statusLabel);
    m_countsLabel = new QLabel(searching);
    m_countsLabel->setObjectName(QStringLiteral("secondaryText"));
    sCol->addWidget(m_countsLabel);
    auto* sButtons = new QHBoxLayout();
    sButtons->addStretch();
    m_stopButton = new QPushButton(tr("Cancel"), searching);
    connect(m_stopButton, &QPushButton::clicked, this, &BulkMediaRecoveryDialog::reject);
    sButtons->addWidget(m_stopButton);
    sCol->addLayout(sButtons);
    m_pages->addWidget(searching);

    // Page 1: results
    auto* results = new QWidget(m_pages);
    auto* rCol = new QVBoxLayout(results);
    rCol->setContentsMargins(0, 0, 0, 0);
    rCol->setSpacing(10);
    m_summaryLabel = new QLabel(results);
    m_summaryLabel->setFont(tf);
    rCol->addWidget(m_summaryLabel);
    m_noteLabel = new QLabel(results);
    m_noteLabel->setObjectName(QStringLiteral("secondaryText"));
    m_noteLabel->setWordWrap(true);
    rCol->addWidget(m_noteLabel);
    m_results = new QTreeWidget(results);
    m_results->setRootIsDecorated(true);
    m_results->setUniformRowHeights(true);
    m_results->setIconSize(QSize(kStatusIconSize, kStatusIconSize));
    m_results->setHeaderLabels({tr("File"), tr("Result"), tr("Location")});
    m_results->header()->setStretchLastSection(false);
    m_results->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_results->setMinimumHeight(220);
    connect(m_results, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int) {
        if (m_updatingChecks) {
            return;
        }
        m_updatingChecks = true;
        if (QTreeWidgetItem* parentItem = item->parent(); parentItem && item->checkState(0) == Qt::Checked) {
            for (int i = 0; i < parentItem->childCount(); ++i) { // one choice per file
                if (parentItem->child(i) != item) {
                    parentItem->child(i)->setCheckState(0, Qt::Unchecked);
                }
            }
        }
        m_updatingChecks = false;
        updateApplyButton();
    });
    rCol->addWidget(m_results, 1);
    auto* rButtons = new QHBoxLayout();
    rButtons->addStretch();
    m_applyButton = new QPushButton(tr("Apply"), results);
    m_applyButton->setObjectName(QStringLiteral("primaryButton"));
    m_applyButton->setToolTip(tr("Update the saved location of the checked files. No file is moved or changed."));
    connect(m_applyButton, &QPushButton::clicked, this, &QDialog::accept);
    rButtons->addWidget(m_applyButton);
    auto* close = new QPushButton(tr("Close"), results);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    rButtons->addWidget(close);
    rCol->addLayout(rButtons);
    m_pages->addWidget(results);

    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(250); // display refresh only - the search itself runs on a worker
    connect(m_progressTimer, &QTimer::timeout, this, &BulkMediaRecoveryDialog::updateProgressText);
    connect(&m_watcher, &QFutureWatcher<MediaBatchResult>::finished, this, &BulkMediaRecoveryDialog::onSearchFinished);
    updateProgressText();
}

BulkMediaRecoveryDialog::~BulkMediaRecoveryDialog() {
    // The worker owns copies of everything it needs; cancelling is enough.
    m_progress->cancel = true;
}

void BulkMediaRecoveryDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (!m_started) {
        m_started = true;
        startSearch();
    }
}

void BulkMediaRecoveryDialog::reject() {
    if (m_pages->currentIndex() == 0 && m_watcher.isRunning()) {
        // Stop the walk, then show whatever it located so far (see
        // onSearchFinished) - never a half-applied state.
        if (!m_stopping) {
            m_stopping = true;
            m_progress->cancel = true;
            m_stopButton->setEnabled(false);
            m_statusLabel->setText(tr("Stopping…"));
            qInfo() << "[Recovery] Find All cancelled by user";
        }
        return;
    }
    QDialog::reject();
}

void BulkMediaRecoveryDialog::startSearch() {
    QVector<MediaSearchRequest> requests;
    requests.reserve(m_entries.size());
    for (const Entry& e : std::as_const(m_entries)) {
        requests.push_back({e.path, e.facts});
    }
    qInfo() << "[Recovery] Find All: searching for" << requests.size() << "file(s) in one pass";
    auto progress = m_progress;
    m_watcher.setFuture(QtConcurrent::run([requests, progress] { return MediaRecovery::searchMany(requests, progress); }));
    m_progressTimer->start();
}

void BulkMediaRecoveryDialog::updateProgressText() {
    if (!m_stopping) {
        m_statusLabel->setText(tr("%1 of %2 files located so far").arg(m_progress->matched.load()).arg(m_entries.size()));
    }
    m_countsLabel->setText(tr("%1 folders checked  •  %2 files examined")
                               .arg(m_progress->foldersScanned.load())
                               .arg(m_progress->filesExamined.load()));
}

void BulkMediaRecoveryDialog::onSearchFinished() {
    m_progressTimer->stop();
    MediaBatchResult result;
    try {
        result = m_watcher.result();
    } catch (const std::exception& e) {
        // The worker threw (e.g. out of memory): report it, never crash.
        qWarning() << "[Recovery] Find All failed:" << e.what();
        m_failed = true;
        QDialog::reject();
        return;
    } catch (...) {
        qWarning() << "[Recovery] Find All failed (unknown error).";
        m_failed = true;
        QDialog::reject();
        return;
    }
    int convincing = 0;
    for (const QVector<MediaCandidate>& list : result.candidates) {
        for (const MediaCandidate& c : list) {
            if (c.isConvincing()) {
                ++convincing;
                break;
            }
        }
    }
    qInfo() << "[Recovery] Find All finished:" << m_progress->foldersScanned.load() << "folders,"
            << m_progress->filesExamined.load() << "files examined," << convincing << "of" << m_entries.size()
            << "located," << m_progress->inaccessibleFolders.load() << "unreadable folder(s)"
            << (result.cancelled ? "(cancelled)" : "");
    m_cancelled = result.cancelled;
    m_unreadable = m_progress->inaccessibleFolders.load();
    if (result.cancelled && convincing == 0) {
        QDialog::reject(); // nothing to show
        return;
    }
    showResults(result);
}

void BulkMediaRecoveryDialog::showResults(const MediaBatchResult& result) {
    m_updatingChecks = true;
    m_results->clear();
    int found = 0;
    int needChoice = 0;
    int notFound = 0;
    int unreachable = 0;
    const int unreadable = m_progress->inaccessibleFolders.load();

    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry& e = m_entries[i];
        const QVector<MediaCandidate> candidates = result.candidates.value(i);
        QVector<MediaCandidate> convincing;
        for (const MediaCandidate& c : candidates) {
            if (c.isConvincing()) {
                convincing << c;
            }
        }
        auto* item = new QTreeWidgetItem(m_results);
        item->setText(0, QFileInfo(e.path).fileName());
        item->setToolTip(0, QDir::toNativeSeparators(e.path));
        item->setData(0, kRoleEntry, i);
        if (convincing.size() == 1) {
            ++found;
            const MediaCandidate& c = convincing.first();
            item->setIcon(1, statusIcon(":/status/check.svg", Theme::kStatusSuccess));
            item->setText(1, tr("Found"));
            item->setText(2, QDir::toNativeSeparators(QFileInfo(c.path).absolutePath()));
            item->setToolTip(2, c.path);
            item->setData(0, kRolePath, c.path);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, Qt::Checked);
        } else if (!candidates.isEmpty()) {
            ++needChoice;
            const QVector<MediaCandidate>& shown = convincing.size() > 1 ? convincing : candidates;
            item->setIcon(1, statusIcon(":/status/pending.svg", Theme::kStatusWarning));
            item->setText(1, tr("Choose a match"));
            item->setText(2, tr("%n possible match(es) - check the right one", nullptr, int(shown.size())));
            for (const MediaCandidate& c : shown) {
                auto* child = new QTreeWidgetItem(item);
                const QString how = c.isConvincing()  ? tr("Same name, size and date")
                                    : c.sameName      ? (c.sameSize ? tr("Same name and size") : tr("Same name"))
                                                      : tr("Renamed copy (same size and date)");
                child->setText(0, QFileInfo(c.path).fileName());
                child->setText(1, how);
                child->setText(2, QDir::toNativeSeparators(QFileInfo(c.path).absolutePath()));
                child->setToolTip(2, c.path);
                child->setData(0, kRoleEntry, i);
                child->setData(0, kRolePath, c.path);
                child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
                child->setCheckState(0, Qt::Unchecked);
            }
            item->setExpanded(true);
        } else if (const QString why = MediaRecovery::unreachableReason(e.path); !why.isEmpty()) {
            ++unreachable;
            item->setIcon(1, statusIcon(":/status/cross.svg", Theme::kStatusWarning));
            item->setText(1, tr("Couldn't be searched"));
            item->setText(2, why);
        } else {
            ++notFound;
            item->setIcon(1, statusIcon(":/status/cross.svg", Theme::kStatusError));
            item->setText(1, tr("Not found"));
            item->setText(2, unreadable > 0 ? tr("Not found in the folders Motiva could read")
                                            : tr("Not found on the available drives"));
        }
    }
    m_updatingChecks = false;
    m_located = found;

    QString summary = tr("%1 of %2 files found").arg(found).arg(m_entries.size());
    if (result.cancelled) {
        summary = tr("Search stopped - %1 of %2 files found so far").arg(found).arg(m_entries.size());
    }
    m_summaryLabel->setText(summary);

    QStringList notes;
    if (needChoice > 0) {
        notes << tr("%n file(s) have several possible matches and need your choice.", nullptr, needChoice);
    }
    if (notFound > 0) {
        notes << tr("%n file(s) were not found.", nullptr, notFound);
    }
    if (unreachable > 0) {
        notes << tr("%n file(s) are on a drive or location that couldn't be searched.", nullptr, unreachable);
    }
    if (unreadable > 0) {
        notes << tr("%n folder(s) couldn't be read (access denied or disconnected), so files inside them may "
                    "exist but weren't checked.", nullptr, unreadable);
    }
    m_noteLabel->setText(notes.join(QLatin1Char(' ')));
    m_noteLabel->setToolTip(result.inaccessible.join(QLatin1Char('\n')));
    m_noteLabel->setVisible(!notes.isEmpty());

    for (int col : {0, 1}) {
        m_results->resizeColumnToContents(col);
    }
    updateApplyButton();
    m_pages->setCurrentIndex(1);
    resize(qMax(width(), 800), qMax(height(), 420));
}

void BulkMediaRecoveryDialog::updateApplyButton() {
    const int n = int(resolutions().size());
    m_applyButton->setText(n > 0 ? tr("Apply (%1)").arg(n) : tr("Apply"));
    m_applyButton->setEnabled(n > 0);
}

QVector<BulkMediaRecoveryDialog::Resolution> BulkMediaRecoveryDialog::resolutions() const {
    QVector<Resolution> out;
    if (!m_results) {
        return out;
    }
    auto take = [&](const QTreeWidgetItem* item) {
        if ((item->flags() & Qt::ItemIsUserCheckable) && item->checkState(0) == Qt::Checked) {
            const int index = item->data(0, kRoleEntry).toInt();
            out.push_back({m_entries.at(index).mediaId, item->data(0, kRolePath).toString()});
        }
    };
    for (int i = 0; i < m_results->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* top = m_results->topLevelItem(i);
        take(top);
        for (int c = 0; c < top->childCount(); ++c) {
            take(top->child(c));
        }
    }
    return out;
}
