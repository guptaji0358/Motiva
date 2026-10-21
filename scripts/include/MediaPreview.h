#pragma once

#include <QString>

class QScreen;
class QWidget;

// Full-screen "big view" of one image or video file (Shift+click in the
// Playlist and Flow windows). Read-only: the file is only opened for viewing.
namespace MediaPreview {

// Returns an empty string on success, otherwise a short user-facing reason.
// The viewer deletes itself when closed (click, Esc or any key; Space pauses a video).
QString show(const QString& path, QWidget* anchor);

} // namespace MediaPreview
