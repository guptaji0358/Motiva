#include "MediaRecovery.h"
#include "GlUi.h"
#include <QStandardItemModel>
#include <QKeyEvent>

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
    : GlWindow(tr("Find File"), tr("Locate missing media"), parent), m_originalPath(originalPath), m_facts(facts),
      m_progress(std::make_shared<MediaSearchProgress>()) {
    const QString name = QFileInfo(originalPath).fileName();
    GlCanvas* c = canvas();

    // Page 0: searching
    m_title0 = new GlLabel(c);
    m_title0->setText(tr("Searching for \"%1\"…").arg(name));
    m_title0->setBold(true);
    m_title0->setPointSize(12);
    m_was = new GlLabel(c);
    m_was->setText(tr("Last known location: %1").arg(QDir::toNativeSeparators(originalPath)));
    m_was->setWordWrap(true);
    m_was->setPointSize(10);
    m_was->setTone(GlLabel::Tone::Dim);
    m_was->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_progressBar = new GlProgressBar(c);
    m_progressBar->setIndeterminate(true); // the total folder count isn't known in advance
    m_statusLabel = new GlLabel(c);
    m_statusLabel->setText(tr("Searching your drives…"));
    m_statusLabel->setPointSize(10);
    m_statusLabel->setTone(GlLabel::Tone::Dim);
    m_cancel0 = new GlButton(c, GlButton::Kind::Secondary);
    m_cancel0->setText(tr("Cancel"));
    connect(m_cancel0, &GlButton::clicked, this, &MediaRecoveryDialog::reject);

    // Page 1: choose among several / weak matches
    m_title1 = new GlLabel(c);
    m_title1->setText(tr("Which file is \"%1\"?").arg(name));
    m_title1->setBold(true);
    m_title1->setPointSize(12);
    m_hint = new GlLabel(c);
    m_hint->setText(tr("Motiva found more than one possible match, or none it can be sure about. "
                       "Choose the right file, or cancel."));
    m_hint->setWordWrap(true);
    m_hint->setPointSize(10);
    m_hint->setTone(GlLabel::Tone::Dim);
    m_hint->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_choiceModel = new QStandardItemModel(0, 5, this);
    m_choices = new GlItemView(c);
    m_choices->setModel(m_choiceModel);
    m_choices->setDelegate(new GlTableDelegate({0.24, 0.30, 0.10, 0.16, 0.20}, m_choices));
    m_choices->setMode(GlItemView::Mode::List, QSize(200, 34), 0);
    m_choices->setAccessibleName(tr("Possible matches"));
    m_useButton = new GlButton(c, GlButton::Kind::Primary);
    m_useButton->setText(tr("Use This File"));
    m_useButton->setEnabled(false);
    connect(m_useButton, &GlButton::clicked, this, [this] {
        const int row = m_choices->currentRow();
        if (row >= 0) {
            m_chosen = m_choiceModel->item(row, 0)->data(Qt::UserRole).toString();
            done(Found);
        }
    });
    m_cancel1 = new GlButton(c, GlButton::Kind::Secondary);
    m_cancel1->setText(tr("Cancel"));
    connect(m_cancel1, &GlButton::clicked, this, &MediaRecoveryDialog::reject);
    connect(m_choices, &GlItemView::currentRowChanged, this, [this](int row) { m_useButton->setEnabled(row >= 0); });
    connect(m_choices, &GlItemView::doubleClicked, this, [this] { m_useButton->activate(); });

    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(250); // display refresh only - the search itself is event-free work on a worker
    connect(m_progressTimer, &QTimer::timeout, this, &MediaRecoveryDialog::updateProgressText);
    connect(&m_watcher, &QFutureWatcher<QVector<MediaCandidate>>::finished, this, &MediaRecoveryDialog::onSearchFinished);
    showPage(0);
}

void MediaRecoveryDialog::showPage(int page) {
    m_page = page;
    const bool searching = page == 0;
    for (GlItem* it : QList<GlItem*>{m_title0, m_was, m_progressBar, m_statusLabel, m_cancel0}) {
        it->setVisible(searching);
    }
    for (GlItem* it : QList<GlItem*>{m_title1, m_hint, m_choices, m_useButton, m_cancel1}) {
        it->setVisible(!searching);
    }
    setFixedHeight(searching ? 330 : 560);
    setMinimumWidth(searching ? 640 : 900);
    resize(searching ? 680 : 960, searching ? 330 : 560);
    canvas()->relayout();
}

void MediaRecoveryDialog::layoutContent(const QRectF& content, const QRectF& footer) {
    if (m_page == 0) {
        m_title0->setRect(QRectF(content.left(), content.top(), content.width(), 28));
        const qreal wh = m_was->heightForWidth(content.width()) + 6;
        m_was->setRect(QRectF(content.left(), content.top() + 38, content.width(), wh));
        const qreal y = content.top() + 38 + wh + 16;
        m_progressBar->setRect(QRectF(content.left(), y, content.width(), 12));
        m_statusLabel->setRect(QRectF(content.left(), y + 22, content.width(), 24));
        layoutFooter({m_cancel0}, footer, 120);
    } else {
        m_title1->setRect(QRectF(content.left(), content.top(), content.width(), 28));
        const qreal hh = m_hint->heightForWidth(content.width()) + 6;
        m_hint->setRect(QRectF(content.left(), content.top() + 34, content.width(), hh));
        const qreal y = content.top() + 34 + hh + 10;
        m_choices->setRect(QRectF(content.left(), y, content.width(), std::max<qreal>(120, content.bottom() - y)));
        layoutFooter({m_cancel1, m_useButton}, footer, 140);
    }
}

MediaRecoveryDialog::~MediaRecoveryDialog() {
    // The worker owns its own copy of everything it needs (shared_ptr
    // progress, path, facts); cancelling is enough - it stops at its next
    // check and its result is simply dropped.
    m_progress->cancel = true;
}

void MediaRecoveryDialog::showEvent(QShowEvent* event) {
    GlWindow::showEvent(event);
    if (!m_started) {
        m_started = true;
        startSearch();
    }
}

void MediaRecoveryDialog::reject() {
    m_progress->cancel = true;
    qInfo() << "[Recovery] Search cancelled for" << m_originalPath;
    GlWindow::reject();
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
    m_choiceModel->removeRows(0, m_choiceModel->rowCount());
    m_choiceModel->setHorizontalHeaderLabels({tr("Name"), tr("Folder"), tr("Size"), tr("Modified"), tr("Match")});
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
        QList<QStandardItem*> cells;
        for (const QString& text : {fi.fileName(), QDir::toNativeSeparators(fi.absolutePath()), formatSize(c.size),
                                    QLocale().toString(c.modified.toLocalTime(), QLocale::ShortFormat), match}) {
            cells << new QStandardItem(text);
        }
        cells[0]->setData(c.path, Qt::UserRole);
        cells[1]->setToolTip(c.path);
        m_choiceModel->appendRow(cells);
    }
    m_useButton->setEnabled(false);
    showPage(1);
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
    : GlWindow(tr("Find All at Once"), tr("Locate missing media"), parent), m_entries(entries),
      m_progress(std::make_shared<MediaSearchProgress>()) {
    GlCanvas* c = canvas();

    // Page 0: searching
    m_title0 = new GlLabel(c);
    m_title0->setText(tr("Searching for %n missing file(s)…", nullptr, int(m_entries.size())));
    m_title0->setBold(true);
    m_title0->setPointSize(12);
    m_how = new GlLabel(c);
    m_how->setText(tr("Motiva looks for all of them in a single pass over your folders and drives."));
    m_how->setWordWrap(true);
    m_how->setPointSize(10);
    m_how->setTone(GlLabel::Tone::Dim);
    m_how->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_progressBar = new GlProgressBar(c);
    m_progressBar->setIndeterminate(true); // the total folder count isn't known in advance
    m_statusLabel = new GlLabel(c);
    m_statusLabel->setPointSize(10.5);
    m_countsLabel = new GlLabel(c);
    m_countsLabel->setPointSize(9.5);
    m_countsLabel->setTone(GlLabel::Tone::Dim);
    m_stopButton = new GlButton(c, GlButton::Kind::Secondary);
    m_stopButton->setText(tr("Cancel"));
    connect(m_stopButton, &GlButton::clicked, this, &BulkMediaRecoveryDialog::reject);

    // Page 1: results
    m_summaryLabel = new GlLabel(c);
    m_summaryLabel->setBold(true);
    m_summaryLabel->setPointSize(12);
    m_noteLabel = new GlLabel(c);
    m_noteLabel->setWordWrap(true);
    m_noteLabel->setPointSize(10);
    m_noteLabel->setTone(GlLabel::Tone::Dim);
    m_noteLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_resultModel = new QStandardItemModel(0, 3, this);
    m_results = new GlItemView(c);
    m_results->setModel(m_resultModel);
    m_results->setDelegate(new GlTableDelegate({0.34, 0.20, 0.46}, m_results));
    m_results->setMode(GlItemView::Mode::List, QSize(200, 34), 0);
    m_results->setAccessibleName(tr("Search results"));
    connect(m_results, &GlItemView::clicked, this, &BulkMediaRecoveryDialog::toggleResultRow);
    m_results->setKeyFilter([this](QKeyEvent* e) {
        if (e->key() == Qt::Key_Space && !(e->modifiers() & Qt::ControlModifier)) {
            toggleResultRow(m_results->currentRow());
            return true;
        }
        return false;
    });
    m_applyButton = new GlButton(c, GlButton::Kind::Primary);
    m_applyButton->setText(tr("Apply"));
    m_applyButton->setToolTip(tr("Update the saved location of the checked files. No file is moved or changed."));
    connect(m_applyButton, &GlButton::clicked, this, &QDialog::accept);
    m_closeButton = new GlButton(c, GlButton::Kind::Secondary);
    m_closeButton->setText(tr("Close"));
    connect(m_closeButton, &GlButton::clicked, this, &QDialog::reject);

    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(250); // display refresh only - the search itself runs on a worker
    connect(m_progressTimer, &QTimer::timeout, this, &BulkMediaRecoveryDialog::updateProgressText);
    connect(&m_watcher, &QFutureWatcher<MediaBatchResult>::finished, this, &BulkMediaRecoveryDialog::onSearchFinished);
    showPage(0);
    updateProgressText();
}

void BulkMediaRecoveryDialog::showPage(int page) {
    m_page = page;
    const bool searching = page == 0;
    for (GlItem* it : QList<GlItem*>{m_title0, m_how, m_progressBar, m_statusLabel, m_countsLabel, m_stopButton}) {
        it->setVisible(searching);
    }
    for (GlItem* it : QList<GlItem*>{m_summaryLabel, m_noteLabel, m_results, m_applyButton, m_closeButton}) {
        it->setVisible(!searching);
    }
    setMinimumWidth(searching ? 680 : 960);
    setMinimumHeight(searching ? 340 : 560);
    resize(searching ? 720 : 1040, searching ? 340 : 640);
    canvas()->relayout();
}

void BulkMediaRecoveryDialog::layoutContent(const QRectF& content, const QRectF& footer) {
    if (m_page == 0) {
        m_title0->setRect(QRectF(content.left(), content.top(), content.width(), 28));
        const qreal hh = m_how->heightForWidth(content.width()) + 6;
        m_how->setRect(QRectF(content.left(), content.top() + 38, content.width(), hh));
        const qreal y = content.top() + 38 + hh + 16;
        m_progressBar->setRect(QRectF(content.left(), y, content.width(), 12));
        m_statusLabel->setRect(QRectF(content.left(), y + 22, content.width(), 24));
        m_countsLabel->setRect(QRectF(content.left(), y + 48, content.width(), 22));
        layoutFooter({m_stopButton}, footer, 120);
    } else {
        m_summaryLabel->setRect(QRectF(content.left(), content.top(), content.width(), 28));
        const bool note = !m_noteLabel->text().isEmpty();
        const qreal nh = note ? m_noteLabel->heightForWidth(content.width()) + 6 : 0;
        m_noteLabel->setVisible(note);
        m_noteLabel->setRect(QRectF(content.left(), content.top() + 34, content.width(), nh));
        const qreal y = content.top() + 34 + nh + 10;
        m_results->setRect(QRectF(content.left(), y, content.width(), std::max<qreal>(140, content.bottom() - y)));
        layoutFooter({m_closeButton, m_applyButton}, footer, 130);
    }
}

void BulkMediaRecoveryDialog::toggleResultRow(int row) {
    if (m_updatingChecks || row < 0 || row >= m_resultModel->rowCount()) {
        return;
    }
    QStandardItem* item = m_resultModel->item(row, 0);
    if (!item->data(GlTableDelegate::kCheckableRole).toBool()) {
        return;
    }
    m_updatingChecks = true;
    const bool nowChecked = item->data(Qt::CheckStateRole).toInt() != Qt::Checked;
    item->setData(nowChecked ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
    if (nowChecked && item->data(GlTableDelegate::kDepthRole).toInt() > 0) {
        // One choice per file: untick the other candidates of the same entry.
        const int entry = item->data(kRoleEntry).toInt();
        for (int r = 0; r < m_resultModel->rowCount(); ++r) {
            QStandardItem* other = m_resultModel->item(r, 0);
            if (r != row && other->data(GlTableDelegate::kDepthRole).toInt() > 0 && other->data(kRoleEntry).toInt() == entry) {
                other->setData(Qt::Unchecked, Qt::CheckStateRole);
            }
        }
    }
    m_updatingChecks = false;
    updateApplyButton();
}

BulkMediaRecoveryDialog::~BulkMediaRecoveryDialog() {
    // The worker owns copies of everything it needs; cancelling is enough.
    m_progress->cancel = true;
}

void BulkMediaRecoveryDialog::showEvent(QShowEvent* event) {
    GlWindow::showEvent(event);
    if (!m_started) {
        m_started = true;
        startSearch();
    }
}

void BulkMediaRecoveryDialog::reject() {
    if (m_page == 0 && m_watcher.isRunning()) {
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
    GlWindow::reject();
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
        GlWindow::reject();
        return;
    } catch (...) {
        qWarning() << "[Recovery] Find All failed (unknown error).";
        m_failed = true;
        GlWindow::reject();
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
        GlWindow::reject(); // nothing to show
        return;
    }
    showResults(result);
}

void BulkMediaRecoveryDialog::showResults(const MediaBatchResult& result) {
    m_updatingChecks = true;
    m_resultModel->removeRows(0, m_resultModel->rowCount());
    int found = 0;
    int needChoice = 0;
    int notFound = 0;
    int unreachable = 0;
    const int unreadable = m_progress->inaccessibleFolders.load();

    const auto addRow = [this](int depth, const QString& file, const QString& res, const QString& where,
                               const QIcon& icon, int entry, const QString& path, bool checkable, bool checked,
                               const QString& tip) {
        QList<QStandardItem*> cells{new QStandardItem(file), new QStandardItem(res), new QStandardItem(where)};
        cells[0]->setData(depth, GlTableDelegate::kDepthRole);
        cells[0]->setData(entry, kRoleEntry);
        cells[0]->setData(path, kRolePath);
        cells[0]->setData(checkable, GlTableDelegate::kCheckableRole);
        if (checkable) {
            cells[0]->setData(checked ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
        }
        if (!icon.isNull()) {
            cells[1]->setIcon(icon);
        }
        cells[2]->setToolTip(tip);
        m_resultModel->appendRow(cells);
    };

    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry& e = m_entries[i];
        const QVector<MediaCandidate> candidates = result.candidates.value(i);
        QVector<MediaCandidate> convincing;
        for (const MediaCandidate& c : candidates) {
            if (c.isConvincing()) {
                convincing << c;
            }
        }
        const QString fileName = QFileInfo(e.path).fileName();
        if (convincing.size() == 1) {
            ++found;
            const MediaCandidate& c = convincing.first();
            addRow(0, fileName, tr("Found"), QDir::toNativeSeparators(QFileInfo(c.path).absolutePath()),
                   statusIcon(":/status/check.svg", Theme::kStatusSuccess), i, c.path, true, true, c.path);
        } else if (!candidates.isEmpty()) {
            ++needChoice;
            const QVector<MediaCandidate>& shown = convincing.size() > 1 ? convincing : candidates;
            addRow(0, fileName, tr("Choose a match"), tr("%n possible match(es) - check the right one", nullptr, int(shown.size())),
                   statusIcon(":/status/pending.svg", Theme::kStatusWarning), i, QString(), false, false, QString());
            for (const MediaCandidate& c : shown) {
                const QString how = c.isConvincing()  ? tr("Same name, size and date")
                                    : c.sameName      ? (c.sameSize ? tr("Same name and size") : tr("Same name"))
                                                      : tr("Renamed copy (same size and date)");
                addRow(1, QFileInfo(c.path).fileName(), how, QDir::toNativeSeparators(QFileInfo(c.path).absolutePath()),
                       QIcon(), i, c.path, true, false, c.path);
            }
        } else if (const QString why = MediaRecovery::unreachableReason(e.path); !why.isEmpty()) {
            ++unreachable;
            addRow(0, fileName, tr("Couldn't be searched"), why, statusIcon(":/status/cross.svg", Theme::kStatusWarning),
                   i, QString(), false, false, QString());
        } else {
            ++notFound;
            addRow(0, fileName, tr("Not found"),
                   unreadable > 0 ? tr("Not found in the folders Motiva could read") : tr("Not found on the available drives"),
                   statusIcon(":/status/cross.svg", Theme::kStatusError), i, QString(), false, false, QString());
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
    updateApplyButton();
    showPage(1);
}

void BulkMediaRecoveryDialog::updateApplyButton() {
    const int n = int(resolutions().size());
    m_applyButton->setText(n > 0 ? tr("Apply (%1)").arg(n) : tr("Apply"));
    m_applyButton->setEnabled(n > 0);
}

QVector<BulkMediaRecoveryDialog::Resolution> BulkMediaRecoveryDialog::resolutions() const {
    QVector<Resolution> out;
    if (!m_resultModel) {
        return out;
    }
    for (int r = 0; r < m_resultModel->rowCount(); ++r) {
        const QStandardItem* item = m_resultModel->item(r, 0);
        if (item->data(GlTableDelegate::kCheckableRole).toBool() && item->data(Qt::CheckStateRole).toInt() == Qt::Checked) {
            const int index = item->data(kRoleEntry).toInt();
            out.push_back({m_entries.at(index).mediaId, item->data(kRolePath).toString()});
        }
    }
    return out;
}
