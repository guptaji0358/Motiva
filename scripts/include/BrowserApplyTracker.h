#pragma once
// Pure (Qt Core only) bookkeeping for ONE pending browser "set as wallpaper" request.
// MainWindow owns the timer and the wiring; this class owns the rules, so they can be
// tested without a desktop:
//   * at most one request is pending; a newer one answers the older one "replaced" first;
//   * a request is answered exactly once (confirm, failure, replacement or timeout);
//   * a verification/battery signal confirms the request only while the request's own file
//     is the active wallpaper source (stale signal, manual change, newer request cannot confirm it);
//   * a playback error fails the request only under the same condition (an error that belongs
//     to some other file must not fail it).
#include <QString>
#include <functional>

class BrowserApplyTracker {
public:
    using Reply = std::function<void(bool ok, const QString& detail)>;

    bool pending() const { return bool(m_reply); }
    QString path() const { return m_path; }

    // Registers a request. A still-pending older request is answered (false, replacedDetail) first.
    // Returns true when an older request was replaced.
    bool begin(const QString& path, Reply reply, const QString& replacedDetail);

    // True when this request's file is `activePath` (case-insensitive, normalised separators).
    bool matchesActive(const QString& activePath) const;

    // Each returns true only when it answered the pending request (so the caller can stop its timer).
    bool confirmIfActive(const QString& activePath, const QString& detail = QString()); // verified / battery
    bool failIfActive(const QString& activePath, const QString& detail);               // playback error
    bool settle(bool ok, const QString& detail);                                        // timeout / rejection of THIS request

    // The wallpaper source changed to `newActivePath` (manual choice, playlist advance, any other load): a
    // pending request for a DIFFERENT file can no longer be satisfied, so it is answered (false, detail) now
    // instead of idling until its timeout. A request for that same file (its own load) is left untouched.
    bool supersedeUnlessActive(const QString& newActivePath, const QString& detail);
    // The wallpaper was removed / the selection cleared: the pending request can no longer be satisfied.
    bool cancel(const QString& detail) { return settle(false, detail); }

private:
    QString m_path;
    Reply m_reply;
};
