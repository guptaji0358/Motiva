#include "CloudManager.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QVersionNumber>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <windows.h>
#include <wincrypt.h>

namespace {

constexpr int kMinMajor = 1, kMinMinor = 65;
constexpr int kListTimeoutMs = 90 * 1000;
constexpr int kSignInTimeoutMs = 5 * 60 * 1000;
constexpr int kPreviewTimeoutMs = 15 * 60 * 1000;
constexpr int kTransferIdleMs = 3 * 60 * 1000;

QByteArray dpapiProtect(const QByteArray& plain) {
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Motiva cloud", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return {};
    }
    const QByteArray result(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    LocalFree(out.pbData);
    return result;
}

QByteArray dpapiUnprotect(const QByteArray& cipher) {
    DATA_BLOB in{DWORD(cipher.size()), reinterpret_cast<BYTE*>(const_cast<char*>(cipher.constData()))};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return {};
    }
    const QByteArray result(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
}

QString sanitizedFileName(const QString& name) {
    QString out;
    for (const QChar c : name) {
        out += (c.unicode() < 32 || QStringLiteral("<>:\"/\\|?*").contains(c)) ? QLatin1Char('_') : c;
    }
    out = out.trimmed();
    while (out.endsWith(QLatin1Char('.'))) {
        out.chop(1);
    }
    if (out.isEmpty()) {
        out = QStringLiteral("file");
    }
    const QFileInfo fi(out);
    if (out.size() > 120) {
        const QString ext = fi.suffix().left(10);
        out = fi.completeBaseName().left(110) + (ext.isEmpty() ? QString() : QLatin1Char('.') + ext);
    }
    return out;
}

QString joinPath(const QString& dir, const QString& name) {
    return dir.isEmpty() ? name : dir + QLatin1Char('/') + name;
}

// rclone filter syntax: escape glob metacharacters so a file name matches literally.
QString filterLiteral(const QString& name) {
    QString out;
    for (const QChar c : name) {
        if (QStringLiteral("\\*?[]{}").contains(c)) {
            out += QLatin1Char('\\');
        }
        out += c;
    }
    return out;
}

QString parentOf(const QString& path) {
    const int i = path.lastIndexOf(QLatin1Char('/'));
    return i < 0 ? QString() : path.left(i);
}

QString timestamp() {
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
}

QDateTime parseRcloneTime(const QString& text) {
    // "2026-10-23T18:50:31.7323627+05:30": Qt wants at most 3 fractional digits.
    static const QRegularExpression frac(QStringLiteral(R"(\.(\d{3})\d+)"));
    QString t = text;
    t.replace(frac, QStringLiteral(".\\1"));
    QDateTime d = QDateTime::fromString(t, Qt::ISODateWithMs);
    if (!d.isValid()) {
        d = QDateTime::fromString(t, Qt::ISODate);
    }
    return d.toUTC();
}

QString answerFor(const QJsonObject& option) {
    const QString name = option.value(QStringLiteral("Name")).toString();
    const QString type = option.value(QStringLiteral("Type")).toString();
    const QJsonValue def = option.value(QStringLiteral("Default"));
    if (name == QLatin1String("config_is_local")) {
        return QStringLiteral("true"); // sign in through the browser on this PC
    }
    if (name == QLatin1String("config_shared_client_id")) {
        return QStringLiteral("true"); // rclone's shared Google app until the publisher ships its own
    }
    if (type == QLatin1String("bool") || def.isBool()) {
        return def.isBool() && !def.toBool() ? QStringLiteral("false") : QStringLiteral("true");
    }
    QString d;
    if (def.isString()) {
        d = def.toString();
    } else if (def.isDouble()) {
        d = QString::number(def.toDouble(), 'f', 0);
    }
    if (!d.isEmpty()) {
        return d;
    }
    const QJsonArray examples = option.value(QStringLiteral("Examples")).toArray();
    if (!examples.isEmpty()) {
        return examples.first().toObject().value(QStringLiteral("Value")).toString();
    }
    return QString();
}

} // namespace

CloudManager* CloudManager::instance() {
    static CloudManager* s = new CloudManager(); // lives for the whole process
    return s;
}

CloudManager::CloudManager(QObject* parent)
    : QObject(parent), m_settings(QSettings::NativeFormat, QSettings::UserScope, "Motiva", "Motiva") {}

CloudManager::~CloudManager() = default;

QList<CloudProviderInfo> CloudManager::providers() {
    return {
        {QStringLiteral("gdrive"), QStringLiteral("Google Drive"), QStringLiteral("drive"),
         QStringLiteral("https://console.cloud.google.com/apis/credentials")},
        {QStringLiteral("onedrive"), QStringLiteral("Microsoft OneDrive"), QStringLiteral("onedrive"),
         QStringLiteral("https://entra.microsoft.com")},
        {QStringLiteral("dropbox"), QStringLiteral("Dropbox"), QStringLiteral("dropbox"),
         QStringLiteral("https://www.dropbox.com/developers/apps")},
    };
}

CloudProviderInfo CloudManager::provider(const QString& id) {
    for (const CloudProviderInfo& p : providers()) {
        if (p.id == id) {
            return p;
        }
    }
    return {};
}

// ------------------------------------------------------------------ locations

QString CloudManager::cacheRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/cloud-cache");
}

QString CloudManager::localTrashRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/cloud-trash");
}

QString CloudManager::configPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/rclone/rclone.conf");
}

void CloudManager::setConfigDirectoryForTesting(const QString& dir) {
    m_configDirOverride = dir;
}

void CloudManager::setRclonePathForTesting(const QString& exe) {
    m_rcloneOverride = exe;
    m_toolChecked = false;
    m_ready = false;
}

QString CloudManager::cachePathFor(const QString& remote, const CloudEntry& entry) const {
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash((remote + QLatin1Char(':') + entry.path).toUtf8(), QCryptographicHash::Sha1).toHex().left(10));
    return QStringLiteral("%1/%2/%3/%4").arg(cacheRoot(), sanitizedFileName(remote), key, sanitizedFileName(entry.name));
}

QString CloudManager::remotePath(const QString& remote, const QString& path) {
    return remote + QLatin1Char(':') + path;
}

QString CloudManager::uniqueLocalName(const QString& dir, const QString& name) {
    const QFileInfo fi(name);
    const QString base = fi.completeBaseName().isEmpty() ? name : fi.completeBaseName();
    const QString ext = fi.suffix().isEmpty() ? QString() : QLatin1Char('.') + fi.suffix();
    QString candidate = name;
    for (int i = 2; QFileInfo::exists(QDir(dir).filePath(candidate)); ++i) {
        candidate = QStringLiteral("%1 (%2)%3").arg(base).arg(i).arg(ext);
    }
    return candidate;
}

QString CloudManager::newTrashDir(const QString& remote) {
    if (remote.isEmpty()) {
        return QDir::toNativeSeparators(localTrashRoot() + QLatin1Char('/') + timestamp());
    }
    return remotePath(remote, syncTrashFolderName() + QLatin1Char('/') + timestamp());
}

// ------------------------------------------------------------------ the tool

QString CloudManager::locateRclone(bool* bundled) {
    if (bundled) {
        *bundled = false;
    }
    QSettings s(QSettings::NativeFormat, QSettings::UserScope, "Motiva", "Motiva");
    const QString custom = s.value(QStringLiteral("cloud/rclonePath")).toString();
    if (!custom.isEmpty() && QFileInfo(custom).isFile()) {
        return QDir::toNativeSeparators(custom);
    }
    const QString appDir = QCoreApplication::applicationDirPath();
    for (const QString& candidate : {appDir + QStringLiteral("/rclone/rclone.exe"), appDir + QStringLiteral("/rclone.exe")}) {
        if (QFileInfo(candidate).isFile()) {
            if (bundled) {
                *bundled = true;
            }
            return QDir::toNativeSeparators(candidate);
        }
    }
    return QStandardPaths::findExecutable(QStringLiteral("rclone"));
}

QProcessEnvironment CloudManager::environment(bool withConfigPassword) const {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // A user's own RCLONE_* variables must never redirect Motiva's rclone.
    for (const QString& k : env.keys()) {
        if (k.startsWith(QStringLiteral("RCLONE_"), Qt::CaseInsensitive)) {
            env.remove(k);
        }
    }
    if (withConfigPassword && !m_key.isEmpty()) {
        env.insert(QStringLiteral("RCLONE_CONFIG_PASS"), QString::fromLatin1(m_key));
    }
    return env;
}

QStringList CloudManager::commonArgs() const {
    const QString conf = m_configDirOverride.isEmpty() ? configPath() : QDir(m_configDirOverride).filePath(QStringLiteral("rclone.conf"));
    return {QStringLiteral("--config"), QDir::toNativeSeparators(conf), QStringLiteral("--ask-password=false")};
}

RcloneJob* CloudManager::makeJob(const QStringList& args, bool withConfigPassword) {
    return new RcloneJob(m_tool.path, commonArgs() + args, environment(withConfigPassword), this);
}

void CloudManager::checkTool(ToolCallback done) {
    if (m_toolChecked) {
        QTimer::singleShot(0, this, [this, done] { done(m_tool); });
        return;
    }
    CloudToolInfo info;
    bool bundled = false;
    info.path = m_rcloneOverride.isEmpty() ? locateRclone(&bundled) : m_rcloneOverride;
    info.bundled = bundled;
    if (info.path.isEmpty()) {
        info.error = tr("The cloud transfer tool (rclone) was not found. Reinstall Motiva, or put rclone.exe in the 'rclone' "
                        "folder next to Motiva's program files.");
        m_tool = info;
        QTimer::singleShot(0, this, [this, done] { done(m_tool); });
        return;
    }
    info.licenseFilePresent = QFileInfo(QFileInfo(info.path).absolutePath() + QStringLiteral("/LICENSE.txt")).isFile();
    m_tool = info;
    auto* job = new RcloneJob(info.path, {QStringLiteral("version")}, environment(false), this);
    job->setTimeouts(20 * 1000, 0);
    connect(job, &RcloneJob::finished, this, [this, done, job](const RcloneResult& r) {
        job->deleteLater();
        static const QRegularExpression re(QStringLiteral(R"(rclone v?(\d+\.\d+\.\d+))"));
        const QRegularExpressionMatch m = re.match(QString::fromUtf8(r.out));
        if (!r.ok() || !m.hasMatch()) {
            m_tool.error = tr("rclone could not be started (%1).").arg(QDir::toNativeSeparators(m_tool.path));
        } else {
            m_tool.version = m.captured(1);
            const QVersionNumber v = QVersionNumber::fromString(m_tool.version);
            if (QVersionNumber::compare(v, QVersionNumber(kMinMajor, kMinMinor)) < 0) {
                m_tool.error = tr("rclone %1 is too old; Motiva needs %2.%3 or newer.")
                                   .arg(m_tool.version)
                                   .arg(kMinMajor)
                                   .arg(kMinMinor);
            } else {
                m_tool.found = true;
            }
        }
        m_toolChecked = true;
        done(m_tool);
    });
    job->start();
}

// ------------------------------------------------------------------ config + key

QByteArray CloudManager::loadKey() const {
    const QByteArray blob = QByteArray::fromBase64(m_settings.value(QStringLiteral("cloud/rcloneKey")).toByteArray());
    return blob.isEmpty() ? QByteArray() : dpapiUnprotect(blob);
}

void CloudManager::storeKey(const QByteArray& key) {
    m_settings.setValue(QStringLiteral("cloud/rcloneKey"), dpapiProtect(key).toBase64());
    m_settings.sync();
}

void CloudManager::clearKey() {
    m_settings.remove(QStringLiteral("cloud/rcloneKey"));
    m_settings.sync();
}

bool CloudManager::isConfigEncrypted() const {
    const QString conf = m_configDirOverride.isEmpty() ? configPath() : QDir(m_configDirOverride).filePath(QStringLiteral("rclone.conf"));
    QFile f(conf);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    return f.read(512).contains("RCLONE_ENCRYPT_V0:");
}

void CloudManager::ensureReady(ReadyCallback done) {
    if (m_ready) {
        QTimer::singleShot(0, this, [done] { done(true, QString()); });
        return;
    }
    m_readyWaiters << done;
    if (m_readyBusy) {
        return;
    }
    m_readyBusy = true;
    checkTool([this](const CloudToolInfo& info) {
        if (!info.found) {
            finishReady(false, info.error);
            return;
        }
        prepareConfig();
    });
}

void CloudManager::finishReady(bool ok, const QString& error) {
    m_ready = ok;
    m_readyBusy = false;
    const QList<ReadyCallback> waiters = m_readyWaiters;
    m_readyWaiters.clear();
    for (const ReadyCallback& cb : waiters) {
        cb(ok, error);
    }
}

void CloudManager::prepareConfig() {
    const QString conf = m_configDirOverride.isEmpty() ? configPath() : QDir(m_configDirOverride).filePath(QStringLiteral("rclone.conf"));
    QDir().mkpath(QFileInfo(conf).absolutePath());
    m_configUnreadable = false;

    m_key = loadKey();
    if (m_key.isEmpty()) {
        QByteArray raw(16, 0);
        QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(raw.data()), 4);
        m_key = raw.toHex(); // hex only: safe to pass through the password command
        storeKey(m_key);
    }
    const QString unreadable = tr("Motiva's saved cloud accounts can't be read (the encryption key is missing or changed). "
                                  "Use \"Reset cloud accounts\" and connect again.");

    auto verify = [this, unreadable] {
        auto* job = makeJob({QStringLiteral("listremotes")});
        job->setTimeouts(30 * 1000, 0);
        connect(job, &RcloneJob::finished, this, [this, job, unreadable](const RcloneResult& r) {
            job->deleteLater();
            if (r.ok()) {
                finishReady(true, QString());
            } else {
                m_configUnreadable = true;
                finishReady(false, unreadable);
            }
        });
        job->start();
    };
    auto encrypt = [this, verify, unreadable] {
        if (isConfigEncrypted()) {
            verify();
            return;
        }
        // The password never appears on a command line: rclone runs `cmd /c echo %VAR%`
        // and reads the key from that child's output.
        QProcessEnvironment env = environment(false);
        env.insert(QStringLiteral("MOTIVA_RCLONE_KEY"), QString::fromLatin1(m_key));
        env.insert(QStringLiteral("RCLONE_PASSWORD_COMMAND"), QStringLiteral("cmd /c echo %MOTIVA_RCLONE_KEY%"));
        auto* job = new RcloneJob(m_tool.path, commonArgs() + QStringList{QStringLiteral("config"), QStringLiteral("encryption"), QStringLiteral("set")},
                                  env, this);
        job->setTimeouts(30 * 1000, 0);
        connect(job, &RcloneJob::finished, this, [this, job, verify, unreadable](const RcloneResult& r) {
            job->deleteLater();
            if (r.ok() && isConfigEncrypted()) {
                verify();
            } else {
                m_configUnreadable = true;
                finishReady(false, unreadable);
            }
        });
        job->start();
    };

    if (QFileInfo::exists(conf)) {
        encrypt();
        return;
    }
    auto* touch = makeJob({QStringLiteral("config"), QStringLiteral("touch")}, false);
    touch->setTimeouts(30 * 1000, 0);
    connect(touch, &RcloneJob::finished, this, [this, touch, encrypt](const RcloneResult& r) {
        touch->deleteLater();
        if (!r.ok()) {
            finishReady(false, r.userMessage());
            return;
        }
        encrypt();
    });
    touch->start();
}

bool CloudManager::resetAccounts() {
    if (m_planActive || m_connectActive) {
        return false;
    }
    const QString conf = m_configDirOverride.isEmpty() ? configPath() : QDir(m_configDirOverride).filePath(QStringLiteral("rclone.conf"));
    bool ok = true;
    if (QFileInfo::exists(conf)) {
        ok = QFile::remove(conf);
    }
    clearKey();
    m_key.clear();
    m_ready = false;
    m_configUnreadable = false;
    return ok;
}

// ------------------------------------------------------------------ credentials

namespace {
QSettings& bundledDefaults() {
    static QSettings s(QCoreApplication::applicationDirPath() + QStringLiteral("/rclone/motiva-cloud.ini"), QSettings::IniFormat);
    return s;
}
} // namespace

QString CloudManager::clientId(const QString& providerId) const {
    const QString saved = m_settings.value(QStringLiteral("cloud/%1/clientId").arg(providerId)).toString().trimmed();
    if (!saved.isEmpty()) {
        return saved;
    }
    return bundledDefaults().value(QStringLiteral("%1/client_id").arg(providerId)).toString().trimmed();
}

QString CloudManager::clientSecret(const QString& providerId) const {
    const QByteArray blob = QByteArray::fromBase64(m_settings.value(QStringLiteral("cloud/%1/clientSecret").arg(providerId)).toByteArray());
    if (!blob.isEmpty()) {
        const QByteArray plain = dpapiUnprotect(blob);
        if (!plain.isEmpty()) {
            return QString::fromUtf8(plain);
        }
    }
    if (!m_settings.value(QStringLiteral("cloud/%1/clientId").arg(providerId)).toString().trimmed().isEmpty()) {
        return QString(); // own client id without a secret (Dropbox/OneDrive public clients)
    }
    return bundledDefaults().value(QStringLiteral("%1/client_secret").arg(providerId)).toString().trimmed();
}

void CloudManager::setClientCredentials(const QString& providerId, const QString& id, const QString& secret) {
    const QString idKey = QStringLiteral("cloud/%1/clientId").arg(providerId);
    const QString secretKey = QStringLiteral("cloud/%1/clientSecret").arg(providerId);
    if (id.trimmed().isEmpty()) {
        m_settings.remove(idKey);
        m_settings.remove(secretKey);
    } else {
        m_settings.setValue(idKey, id.trimmed());
        if (secret.trimmed().isEmpty()) {
            m_settings.remove(secretKey);
        } else {
            m_settings.setValue(secretKey, dpapiProtect(secret.trimmed().toUtf8()).toBase64());
        }
    }
    m_settings.sync();
}

// ------------------------------------------------------------------ accounts

bool CloudManager::isValidRemoteName(const QString& name, QString* why) {
    static const QRegularExpression ok(QStringLiteral(R"(^[A-Za-z0-9_.+@][A-Za-z0-9 _.+@-]*[A-Za-z0-9_.+@-]$)"));
    auto fail = [why](const QString& m) {
        if (why) {
            *why = m;
        }
        return false;
    };
    if (name.size() < 2) {
        return fail(QObject::tr("The account name needs at least 2 characters."));
    }
    if (name.size() > 40) {
        return fail(QObject::tr("The account name is too long (40 characters at most)."));
    }
    if (!ok.match(name).hasMatch()) {
        return fail(QObject::tr("Use only letters, digits, spaces and . _ + @ - in the account name."));
    }
    return true;
}

void CloudManager::listRemotes(RemotesCallback done) {
    ensureReady([this, done](bool ok, const QString& error) {
        if (!ok) {
            done({}, error);
            return;
        }
        auto* job = makeJob({QStringLiteral("listremotes"), QStringLiteral("--long")});
        job->setTimeouts(30 * 1000, 0);
        connect(job, &RcloneJob::finished, this, [this, job, done](const RcloneResult& r) {
            job->deleteLater();
            if (!r.ok()) {
                done({}, r.userMessage());
                return;
            }
            QList<CloudRemote> remotes;
            const QStringList lines = QString::fromUtf8(r.out).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const QString& raw : lines) {
                const int colon = raw.indexOf(QLatin1Char(':'));
                if (colon <= 0) {
                    continue;
                }
                CloudRemote rem;
                rem.name = raw.left(colon).trimmed();
                rem.type = raw.mid(colon + 1).trimmed();
                for (const CloudProviderInfo& p : providers()) {
                    if (p.rcloneType == rem.type) {
                        rem.providerId = p.id;
                    }
                }
                if (!rem.providerId.isEmpty()) { // Motiva only manages the three cloud services
                    remotes << rem;
                }
            }
            done(remotes, QString());
        });
        job->start();
    });
}

bool CloudManager::connectAccount(const QString& providerId, const QString& name, bool readOnly) {
    if (m_connectActive) {
        return false;
    }
    const CloudProviderInfo info = provider(providerId);
    if (info.id.isEmpty()) {
        return false;
    }
    m_connectActive = true;
    m_connectCancelled = false;
    emit connectStatus(tr("Preparing…"));
    ensureReady([this, info, name, readOnly](bool ok, const QString& error) {
        if (m_connectCancelled) {
            m_connectActive = false;
            emit connectFinished(name, false, tr("Cancelled."));
            return;
        }
        if (!ok) {
            m_connectActive = false;
            emit connectFinished(name, false, error);
            return;
        }
        QStringList args{QStringLiteral("config"), QStringLiteral("create"), name, info.rcloneType};
        const QString id = clientId(info.id);
        if (!id.isEmpty()) {
            args << QStringLiteral("client_id=%1").arg(id);
            const QString secret = clientSecret(info.id);
            if (!secret.isEmpty()) {
                args << QStringLiteral("client_secret=%1").arg(secret);
            }
        }
        if (readOnly && info.id == QLatin1String("gdrive")) {
            args << QStringLiteral("scope=drive.readonly");
        }
        args << QStringLiteral("--non-interactive");
        emit connectStatus(tr("Opening your browser - sign in there, then return to Motiva…"));
        runConnectJob(name, args, 0, true);
    });
    return true;
}

bool CloudManager::reconnectAccount(const QString& name) {
    if (m_connectActive) {
        return false;
    }
    m_connectActive = true;
    m_connectCancelled = false;
    emit connectStatus(tr("Preparing…"));
    ensureReady([this, name](bool ok, const QString& error) {
        if (!ok || m_connectCancelled) {
            m_connectActive = false;
            emit connectFinished(name, false, ok ? tr("Cancelled.") : error);
            return;
        }
        emit connectStatus(tr("Opening your browser - sign in there, then return to Motiva…"));
        runConnectJob(name,
                      {QStringLiteral("config"), QStringLiteral("update"), name, QStringLiteral("config_refresh_token=true"),
                       QStringLiteral("--non-interactive")},
                      0, false);
    });
    return true;
}

void CloudManager::runConnectJob(const QString& name, const QStringList& args, int iteration, bool createdByUs) {
    auto* job = makeJob(args);
    job->setTimeouts(kSignInTimeoutMs, 0); // sign-in happens in the browser; bounded to 5 minutes
    m_connectJob = job;
    connect(job, &RcloneJob::finished, this, [this, job, name, iteration, createdByUs](const RcloneResult& r) {
        job->deleteLater();
        if (m_connectCancelled || r.status == RcloneResult::Status::Cancelled) {
            failConnect(name, tr("Sign-in was cancelled."), createdByUs);
            return;
        }
        if (r.status == RcloneResult::Status::TimedOut) {
            failConnect(name, tr("Sign-in took too long and was stopped. Try again."), createdByUs);
            return;
        }
        if (!r.ok()) {
            failConnect(name, r.userMessage(), createdByUs);
            return;
        }
        configStep(name, r.out, iteration, createdByUs);
    });
    job->start();
}

// `config create/update --non-interactive` answers with the next question (a
// "State") until the account is complete; every question is answered with its
// default (or first example), and the browser sign-in itself happens inside
// rclone when "use web browser" is answered.
void CloudManager::configStep(const QString& name, const QByteArray& jsonOut, int iteration, bool createdByUs) {
    const QJsonObject o = QJsonDocument::fromJson(jsonOut).object();
    const QString err = o.value(QStringLiteral("Error")).toString();
    if (!err.isEmpty()) {
        failConnect(name, RcloneJob::scrub(err), createdByUs);
        return;
    }
    const QString state = o.value(QStringLiteral("State")).toString();
    if (state.isEmpty()) {
        m_connectActive = false;
        emit connectFinished(name, true, tr("Connected."));
        return;
    }
    if (iteration >= 12) {
        failConnect(name, tr("The sign-in did not finish (too many steps)."), createdByUs);
        return;
    }
    const QString answer = answerFor(o.value(QStringLiteral("Option")).toObject());
    runConnectJob(name,
                  {QStringLiteral("config"), QStringLiteral("update"), name, QStringLiteral("--continue"), QStringLiteral("--state"),
                   state, QStringLiteral("--result"), answer, QStringLiteral("--non-interactive")},
                  iteration + 1, createdByUs);
}

void CloudManager::failConnect(const QString& name, const QString& message, bool deleteRemote) {
    m_connectActive = false;
    if (deleteRemote) { // a half-created account must not linger without a token
        auto* job = makeJob({QStringLiteral("config"), QStringLiteral("delete"), name});
        job->setTimeouts(30 * 1000, 0);
        connect(job, &RcloneJob::finished, job, &QObject::deleteLater);
        job->start();
    }
    emit connectFinished(name, false, message);
}

void CloudManager::cancelConnect() {
    m_connectCancelled = true;
    if (m_connectJob) {
        m_connectJob->cancel();
    }
}

void CloudManager::removeAccount(const QString& name, std::function<void(bool, const QString&)> done) {
    ensureReady([this, name, done](bool ok, const QString& error) {
        if (!ok) {
            done(false, error);
            return;
        }
        auto* job = makeJob({QStringLiteral("config"), QStringLiteral("delete"), name});
        job->setTimeouts(30 * 1000, 0);
        connect(job, &RcloneJob::finished, this, [job, done](const RcloneResult& r) {
            job->deleteLater();
            done(r.ok(), r.userMessage());
        });
        job->start();
    });
}

void CloudManager::testAccount(const QString& name, std::function<void(bool, bool, const QString&)> done) {
    ensureReady([this, name, done](bool ok, const QString& error) {
        if (!ok) {
            done(false, false, error);
            return;
        }
        auto* job = makeJob({QStringLiteral("lsjson"), remotePath(name, QString()), QStringLiteral("--dirs-only"),
                             QStringLiteral("--no-mimetype"), QStringLiteral("--no-modtime")});
        job->setTimeouts(kListTimeoutMs, 0);
        connect(job, &RcloneJob::finished, this, [job, done](const RcloneResult& r) {
            job->deleteLater();
            done(r.ok(), r.kind == RcloneError::Auth, r.userMessage());
        });
        job->start();
    });
}

// ------------------------------------------------------------------ browsing

void CloudManager::list(const QString& remote, const QString& path, ListCallback done) {
    ensureReady([this, remote, path, done](bool ok, const QString& error) {
        if (!ok) {
            done(false, {}, error, false);
            return;
        }
        auto* job = makeJob({QStringLiteral("lsjson"), remotePath(remote, path)});
        job->setTimeouts(kListTimeoutMs, 0);
        connect(job, &RcloneJob::finished, this, [job, path, done](const RcloneResult& r) {
            job->deleteLater();
            if (!r.ok()) {
                done(false, {}, r.userMessage(), r.kind == RcloneError::Auth);
                return;
            }
            QJsonParseError pe;
            const QJsonDocument doc = QJsonDocument::fromJson(r.out, &pe);
            if (pe.error != QJsonParseError::NoError || !doc.isArray()) {
                done(false, {}, QObject::tr("The cloud service returned a listing Motiva could not read."), false);
                return;
            }
            QList<CloudEntry> entries;
            for (const QJsonValue& v : doc.array()) {
                const QJsonObject o = v.toObject();
                CloudEntry e;
                e.name = o.value(QStringLiteral("Name")).toString();
                if (e.name.isEmpty()) {
                    continue;
                }
                e.path = joinPath(path, e.name);
                e.isFolder = o.value(QStringLiteral("IsDir")).toBool();
                e.size = e.isFolder ? 0 : qint64(o.value(QStringLiteral("Size")).toDouble());
                e.mime = o.value(QStringLiteral("MimeType")).toString();
                e.modified = parseRcloneTime(o.value(QStringLiteral("ModTime")).toString());
                entries << e;
            }
            done(true, entries, QString(), false);
        });
        job->start();
    });
}

void CloudManager::makeFolder(const QString& remote, const QString& path, std::function<void(bool, const QString&)> done) {
    ensureReady([this, remote, path, done](bool ok, const QString& error) {
        if (!ok) {
            done(false, error);
            return;
        }
        auto* job = makeJob({QStringLiteral("mkdir"), remotePath(remote, path)});
        job->setTimeouts(kListTimeoutMs, 0);
        connect(job, &RcloneJob::finished, this, [job, done](const RcloneResult& r) {
            job->deleteLater();
            done(r.ok(), r.userMessage());
        });
        job->start();
    });
}

// ------------------------------------------------------------------ plans

CloudPlan CloudManager::planCacheDownload(const QString& remote, const QList<CloudEntry>& files) const {
    CloudPlan plan;
    plan.title = tr("Downloading %n file(s) from %1", nullptr, files.size()).arg(remote);
    for (const CloudEntry& e : files) {
        const QString local = cachePathFor(remote, e);
        CloudStep s;
        s.label = e.name;
        s.localResult = local;
        s.args = {QStringLiteral("copyto"), remotePath(remote, e.path), QDir::toNativeSeparators(local)};
        plan.steps << s;
        plan.partialDirs << QFileInfo(local).absolutePath();
    }
    return plan;
}

// NOTE: `copyto` / copy-of-a-single-file ignore --immutable and --backup-dir, so a single
// file is copied as "copy <its folder> <destination> --include /<exact name>" instead,
// which goes through rclone's normal compare pipeline (verified by the test harness).
CloudPlan CloudManager::planDownloadTo(const QString& remote, const QList<CloudEntry>& entries, const QString& localDir) const {
    CloudPlan plan;
    plan.title = tr("Downloading %n item(s) from %1", nullptr, entries.size()).arg(remote);
    for (const CloudEntry& e : entries) {
        CloudStep s;
        s.label = e.name;
        // --immutable: an existing, different local file is reported, never replaced.
        if (e.isFolder) {
            s.args = {QStringLiteral("copy"), remotePath(remote, e.path),
                      QDir::toNativeSeparators(QDir(localDir).filePath(e.name)), QStringLiteral("--immutable")};
        } else {
            s.args = {QStringLiteral("copy"), remotePath(remote, parentOf(e.path)), QDir::toNativeSeparators(localDir),
                      QStringLiteral("--include"), QStringLiteral("/") + filterLiteral(e.name), QStringLiteral("--max-depth"),
                      QStringLiteral("1"), QStringLiteral("--immutable")};
        }
        plan.steps << s;
    }
    plan.partialDirs << localDir;
    return plan;
}

CloudPlan CloudManager::planUpload(const QString& remote, const QString& remoteDir, const QStringList& localPaths, bool overwrite) const {
    CloudPlan plan;
    plan.title = tr("Uploading %n item(s) to %1", nullptr, localPaths.size()).arg(remote);
    const QString trash = overwrite ? newTrashDir(remote) : QString();
    for (const QString& p : localPaths) {
        const QFileInfo fi(p);
        CloudStep s;
        s.label = fi.fileName();
        if (fi.isDir()) {
            s.args = {QStringLiteral("copy"), QDir::toNativeSeparators(p), remotePath(remote, joinPath(remoteDir, fi.fileName()))};
        } else {
            s.args = {QStringLiteral("copy"), QDir::toNativeSeparators(fi.absolutePath()), remotePath(remote, remoteDir),
                      QStringLiteral("--include"), QStringLiteral("/") + filterLiteral(fi.fileName()), QStringLiteral("--max-depth"),
                      QStringLiteral("1")};
        }
        if (overwrite) {
            s.args << QStringLiteral("--backup-dir") << trash; // replaced remote files are moved aside, not lost
        } else {
            s.args << QStringLiteral("--immutable");           // never replace a different remote file
        }
        plan.steps << s;
    }
    return plan;
}

CloudPlan CloudManager::planCopyFolder(const QString& title, const QString& src, const QString& dst, const QString& trashDir) const {
    CloudPlan plan;
    plan.title = title;
    CloudStep s;
    s.label = title;
    s.args = {QStringLiteral("copy"), src, dst, QStringLiteral("--backup-dir"), trashDir};
    plan.steps << s;
    return plan;
}

CloudPlan CloudManager::planTwoWay(const QString& localDir, const QString& remoteSpec, const QStringList& conflicts) const {
    CloudPlan plan;
    plan.title = tr("Two-way synchronising");
    const QString local = QDir::toNativeSeparators(localDir);
    CloudStep up;
    up.label = tr("Add new files to the cloud");
    up.args = {QStringLiteral("copy"), local, remoteSpec, QStringLiteral("--ignore-existing")};
    CloudStep down;
    down.label = tr("Add new files to this PC");
    down.args = {QStringLiteral("copy"), remoteSpec, local, QStringLiteral("--ignore-existing")};
    plan.steps << up << down;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HHmmss"));
    for (const QString& rel : conflicts) {
        const QFileInfo fi(rel);
        const QString base = fi.completeBaseName();
        const QString ext = fi.suffix().isEmpty() ? QString() : QStringLiteral(".") + fi.suffix();
        QString relDir = fi.path() == QLatin1String(".") ? QString() : fi.path();
        CloudStep c;
        c.label = tr("Keep both versions of %1").arg(rel);
        const QString target = QDir(localDir).filePath(joinPath(relDir, base + QStringLiteral(" (cloud copy ") + stamp + QStringLiteral(")") + ext));
        c.args = {QStringLiteral("copyto"), remoteSpec + (remoteSpec.endsWith(QLatin1Char(':')) ? QString() : QStringLiteral("/")) + rel,
                  QDir::toNativeSeparators(target), QStringLiteral("--immutable")};
        plan.steps << c;
    }
    plan.partialDirs << localDir;
    return plan;
}

CloudPlan CloudManager::planMirror(const QString& src, const QString& dst, const QString& trashDir, int maxDelete) const {
    CloudPlan plan;
    plan.title = tr("Synchronising");
    CloudStep s;
    s.label = tr("Mirror %1 to %2").arg(src, dst);
    s.args = {QStringLiteral("sync"), src, dst, QStringLiteral("--backup-dir"), trashDir, QStringLiteral("--max-delete"),
              QString::number(qMax(0, maxDelete))};
    plan.steps << s;
    return plan;
}

bool CloudManager::planBackupLibrary(const QString& remote, const QString& libraryFile, CloudPlan* plan, QString* error) const {
    if (!QFileInfo(libraryFile).isFile()) {
        if (error) {
            *error = tr("The Motiva library file was not found.");
        }
        return false;
    }
    const QString tmpDir = QDir::temp().filePath(QStringLiteral("motiva-cloud-") + QUuid::createUuid().toString(QUuid::Id128).left(8));
    if (!QDir().mkpath(tmpDir)) {
        if (error) {
            *error = tr("Couldn't create a temporary folder for the backup.");
        }
        return false;
    }
    const QString name = QStringLiteral("MotivaLibrary-%1.mtv").arg(timestamp());
    const QString snapshot = QDir(tmpDir).filePath(name);
    {
        // A consistent copy even while Motiva has the library open.
        const QString conn = QStringLiteral("motiva_cloud_snapshot_") + QUuid::createUuid().toString(QUuid::Id128);
        bool ok = false;
        QString why;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(libraryFile);
            db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
            if (db.open()) {
                QSqlQuery q(db);
                QString target = QDir::toNativeSeparators(snapshot);
                target.replace(QLatin1Char('\''), QStringLiteral("''"));
                ok = q.exec(QStringLiteral("VACUUM INTO '%1'").arg(target));
                if (!ok) {
                    why = q.lastError().text();
                }
            } else {
                why = db.lastError().text();
            }
        }
        QSqlDatabase::removeDatabase(conn);
        if (!ok) {
            QFile::remove(snapshot);
            QDir().rmdir(tmpDir);
            if (error) {
                *error = tr("Couldn't snapshot the library: %1").arg(why);
            }
            return false;
        }
    }
    plan->title = tr("Backing up the Motiva library to %1").arg(remote);
    CloudStep s;
    s.label = name;
    s.args = {QStringLiteral("copyto"), QDir::toNativeSeparators(snapshot),
              remotePath(remote, backupFolderName() + QStringLiteral("/Library/") + name), QStringLiteral("--immutable")};
    plan->steps = {s};
    plan->cleanup = [snapshot, tmpDir] {
        QFile::remove(snapshot);
        QDir().rmdir(tmpDir);
    };
    return true;
}

CloudPlan CloudManager::planBackupFolder(const QString& remote, const QString& localDir) const {
    const QString leaf = QFileInfo(localDir).fileName().isEmpty() ? QStringLiteral("Folder") : QFileInfo(localDir).fileName();
    return planCopyFolder(tr("Backing up %1 to %2").arg(leaf, remote), QDir::toNativeSeparators(localDir),
                          remotePath(remote, backupFolderName() + QStringLiteral("/Files/") + leaf), newTrashDir(remote));
}

CloudPlan CloudManager::planRestore(const QString& remote, const CloudEntry& backup, const QString& localDir, QString* savedAs) const {
    const QString name = uniqueLocalName(localDir, sanitizedFileName(backup.name));
    if (savedAs) {
        *savedAs = QDir(localDir).filePath(name);
    }
    CloudPlan plan;
    plan.title = tr("Restoring %1").arg(backup.name);
    CloudStep s;
    s.label = backup.name;
    s.args = {QStringLiteral("copyto"), remotePath(remote, backup.path), QDir::toNativeSeparators(QDir(localDir).filePath(name)),
              QStringLiteral("--immutable")};
    plan.steps << s;
    plan.partialDirs << localDir;
    return plan;
}

// ------------------------------------------------------------------ running plans

bool CloudManager::startPlan(const CloudPlan& plan) {
    if (m_planActive || plan.steps.isEmpty()) {
        return false;
    }
    m_planActive = true;
    m_cancelRequested = false;
    m_plan = plan;
    m_stepIndex = 0;
    m_outcome = CloudOutcome();
    m_outcome.title = plan.title;
    emit transferStarted(plan.title);
    ensureReady([this](bool ok, const QString& error) {
        if (!ok) {
            m_outcome.errors << error;
            finishPlan();
            return;
        }
        runNextStep();
    });
    return true;
}

void CloudManager::runNextStep() {
    if (m_cancelRequested) {
        m_outcome.cancelled = true;
        finishPlan();
        return;
    }
    if (m_stepIndex >= m_plan.steps.size()) {
        finishPlan();
        return;
    }
    const CloudStep step = m_plan.steps.at(m_stepIndex);
    emit transferLog(tr("Starting: %1").arg(step.label));
    QStringList args = step.args;
    args << QStringLiteral("--use-json-log") << QStringLiteral("-v") << QStringLiteral("--stats") << QStringLiteral("500ms")
         << QStringLiteral("--retries") << QStringLiteral("3") << QStringLiteral("--low-level-retries") << QStringLiteral("6")
         << QStringLiteral("--timeout") << QStringLiteral("60s") << QStringLiteral("--contimeout") << QStringLiteral("30s")
         << QStringLiteral("--transfers") << QStringLiteral("2") << QStringLiteral("--checkers") << QStringLiteral("4");
    auto* job = makeJob(args);
    job->setTimeouts(0, kTransferIdleMs);
    m_stepJob = job;
    connect(job, &RcloneJob::progress, this, [this, step](const RcloneProgress& p) {
        emit transferProgress(p, m_stepIndex, int(m_plan.steps.size()), step.label);
    });
    connect(job, &RcloneJob::finished, this, [this, job, step](const RcloneResult& r) {
        job->deleteLater();
        if (r.status == RcloneResult::Status::Cancelled) {
            m_outcome.cancelled = true;
            emit transferLog(tr("Cancelled: %1").arg(step.label));
            finishPlan();
            return;
        }
        if (r.ok()) {
            if (!step.localResult.isEmpty()) {
                m_outcome.paths << step.localResult;
            }
            emit transferLog(tr("Done: %1").arg(step.label));
        } else {
            const QString msg = r.userMessage();
            m_outcome.errors << QStringLiteral("%1: %2").arg(step.label, msg);
            emit transferLog(tr("Failed: %1 - %2").arg(step.label, msg));
            if (r.kind == RcloneError::Auth) {
                m_outcome.needsReconnect = true;
                finishPlan(); // the same sign-in problem would hit every remaining step
                return;
            }
        }
        ++m_stepIndex;
        runNextStep();
    });
    job->start();
}

void CloudManager::cancelTransfer() {
    if (!m_planActive) {
        return;
    }
    m_cancelRequested = true;
    if (m_stepJob) {
        m_stepJob->cancel();
    }
}

void CloudManager::finishPlan() {
    if (m_outcome.cancelled) {
        // rclone stops cleanly on Ctrl+C; sweep any "*.partial" leftovers only in the
        // destination folders of this plan.
        for (const QString& dir : m_plan.partialDirs) {
            const QStringList partials = QDir(dir).entryList({QStringLiteral("*.partial")}, QDir::Files);
            for (const QString& f : partials) {
                QFile::remove(QDir(dir).filePath(f));
            }
        }
    }
    if (m_plan.cleanup) {
        m_plan.cleanup();
    }
    m_outcome.ok = m_outcome.errors.isEmpty() && !m_outcome.cancelled;
    m_planActive = false;
    const CloudOutcome out = m_outcome;
    emit transferFinished(out);
}

// ------------------------------------------------------------------ previews

void CloudManager::previewTransfer(bool mirror, const QString& src, const QString& dst, PreviewCallback done) {
    ensureReady([this, mirror, src, dst, done](bool ok, const QString& error) {
        if (!ok) {
            CloudSyncPreview p;
            p.error = error;
            done(p);
            return;
        }
        auto* job = makeJob({mirror ? QStringLiteral("sync") : QStringLiteral("copy"), src, dst, QStringLiteral("--dry-run"),
                             QStringLiteral("--combined"), QStringLiteral("-")});
        job->setTimeouts(kPreviewTimeoutMs, 0);
        connect(job, &RcloneJob::finished, this, [job, done](const RcloneResult& r) {
            job->deleteLater();
            CloudSyncPreview p;
            if (!r.ok()) {
                p.error = r.userMessage();
                done(p);
                return;
            }
            const QStringList lines = QString::fromUtf8(r.out).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (QString line : lines) {
                line = line.trimmed();
                if (line.size() < 3 || line.at(1) != QLatin1Char(' ')) {
                    continue;
                }
                const QString path = line.mid(2);
                switch (line.at(0).unicode()) {
                case '+': p.added << path; break;
                case '*': p.changed << path; break;
                case '-': p.removed << path; break;
                case '=': ++p.unchanged; break;
                case '!': ++p.problems; break;
                default: break;
                }
            }
            p.ok = true;
            done(p);
        });
        job->start();
    });
}

void CloudManager::verify(const QString& src, const QString& dst, PreviewCallback done) {
    ensureReady([this, src, dst, done](bool ok, const QString& error) {
        if (!ok) {
            CloudSyncPreview p;
            p.error = error;
            done(p);
            return;
        }
        auto* job = makeJob({QStringLiteral("check"), src, dst, QStringLiteral("--combined"), QStringLiteral("-")});
        job->setTimeouts(kPreviewTimeoutMs, 0);
        connect(job, &RcloneJob::finished, this, [job, done](const RcloneResult& r) {
            job->deleteLater();
            CloudSyncPreview p;
            // `check` exits non-zero when it finds differences; that is a valid answer.
            if (r.out.trimmed().isEmpty() && !r.ok()) {
                p.error = r.userMessage();
                done(p);
                return;
            }
            const QStringList lines = QString::fromUtf8(r.out).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (QString line : lines) {
                line = line.trimmed();
                if (line.size() < 3 || line.at(1) != QLatin1Char(' ')) {
                    continue;
                }
                const QString path = line.mid(2);
                switch (line.at(0).unicode()) {
                case '+': p.added << path; break;   // only on the source: missing at the destination (checked with rclone 1.75.2)
                case '-': p.removed << path; break; // only at the destination
                case '*': p.changed << path; break;
                case '=': ++p.unchanged; break;
                case '!': ++p.problems; break;
                default: break;
                }
            }
            p.ok = true;
            done(p);
        });
        job->start();
    });
}
