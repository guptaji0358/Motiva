#pragma once

#include <QSize>
#include <QString>

// Builds the ready-to-paste ChatGPT prompt behind the ChatGPT button in the Playlists title bar: find the
// best original of the picture on the web, upscale it, and hand back a wallpaper
// that stays sharp when zoomed. Motiva does not talk to ChatGPT itself - the user
// attaches the picture and pastes this text.
namespace WallpaperEnhancePrompt {

// screenSize: the target display in physical pixels.
QString build(const QSize& screenSize);

// Largest display (physical pixels) among the connected screens.
QSize largestScreenSize();

} // namespace WallpaperEnhancePrompt
