#pragma once

#include <QFileInfo>
#include <QRegularExpression>
#include <QString>

// File-name rules for the web media cache (see WebMediaCache). Header-only and
// Qt Core only, so it can be unit tested on its own.
namespace WebMediaName {

constexpr int kMaxLength = 120;

// Turns a name suggested by a web page into one Windows can store: no folders,
// no reserved characters or device names, at most kMaxLength UTF-16 units.
// Returns an empty string when nothing usable is left.
inline QString sanitized(const QString& name) {
    static const QRegularExpression forbidden(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]"));
    // Windows device names cannot be used as file names, with or without an
    // extension, even with spaces before the dot ("com1 .gif"). ¹ ²
    // ³ are the superscript digits Windows also reserves after COM / LPT.
    static const QRegularExpression reserved(
        QStringLiteral("^(con|prn|aux|nul|conin\\$|conout\\$|(com|lpt)[1-9\\x{00b9}\\x{00b2}\\x{00b3}]) *(\\..*)?$"),
        QRegularExpression::CaseInsensitiveOption);

    // Windows drops trailing dots and spaces from a file name.
    auto trimEnd = [](QString& s) {
        while (!s.isEmpty() && (s.back() == QLatin1Char('.') || s.back().isSpace())) {
            s.chop(1);
        }
    };
    // Cuts an over-long name in the base, never through a short extension: a
    // CDN name of 120+ characters would otherwise lose ".jpg" and be refused.
    auto limit = [&trimEnd](QString& s) {
        if (s.size() <= kMaxLength) {
            return;
        }
        const QString suffix = QFileInfo(s).suffix();
        const int keep = suffix.isEmpty() || suffix.size() > 10 ? 0 : suffix.size() + 1;
        const QString tail = s.right(keep);
        s.truncate(kMaxLength - keep);
        if (s.back().isHighSurrogate()) {
            s.chop(1); // never leave half of a surrogate pair
        }
        if (keep) {
            s += tail;
        } else {
            trimEnd(s);
        }
    };

    QString clean = QFileInfo(name).fileName(); // no folders
    clean.replace(forbidden, QStringLiteral("_"));
    clean = clean.trimmed();
    trimEnd(clean);
    limit(clean);
    if (reserved.match(clean).hasMatch()) {
        clean.prepend(QLatin1Char('_'));
        limit(clean);
    }
    return clean;
}

} // namespace WebMediaName
