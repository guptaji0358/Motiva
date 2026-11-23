#include "BrowserApplyTracker.h"

#include <QDir>

namespace {
QString norm(const QString& p) {
    return QDir::cleanPath(QDir::fromNativeSeparators(p)).toLower();
}
} // namespace

bool BrowserApplyTracker::begin(const QString& path, Reply reply, const QString& replacedDetail) {
    const bool replaced = pending();
    if (replaced) {
        settle(false, replacedDetail);
    }
    m_path = path;
    m_reply = std::move(reply);
    return replaced;
}

bool BrowserApplyTracker::matchesActive(const QString& activePath) const {
    return !m_path.isEmpty() && norm(m_path) == norm(activePath);
}

bool BrowserApplyTracker::settle(bool ok, const QString& detail) {
    if (!m_reply) {
        return false; // nothing pending, or already answered: no duplicate final replies
    }
    Reply reply = std::move(m_reply);
    m_reply = nullptr; // cleared before the call: exactly-once even if the reply re-enters
    reply(ok, detail);
    return true;
}

bool BrowserApplyTracker::supersedeUnlessActive(const QString& newActivePath, const QString& detail) {
    return pending() && !matchesActive(newActivePath) && settle(false, detail);
}

bool BrowserApplyTracker::confirmIfActive(const QString& activePath, const QString& detail) {
    return matchesActive(activePath) && settle(true, detail);
}

bool BrowserApplyTracker::failIfActive(const QString& activePath, const QString& detail) {
    return matchesActive(activePath) && settle(false, detail);
}
