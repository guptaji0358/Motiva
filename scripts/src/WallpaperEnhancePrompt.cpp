#include "WallpaperEnhancePrompt.h"

#include <QGuiApplication>
#include <QScreen>

namespace WallpaperEnhancePrompt {

QSize largestScreenSize() {
    QSize best;
    for (QScreen* s : QGuiApplication::screens()) {
        const QSize px = (s->size() * s->devicePixelRatio());
        if (px.width() * px.height() > best.width() * best.height()) {
            best = px;
        }
    }
    return best.isValid() ? best : QSize(1920, 1080);
}

QString build(const QSize& screenSize) {
    const QString screen = QStringLiteral("%1 x %2 px").arg(screenSize.width()).arg(screenSize.height());
    // Aim for 2x the screen (4K floor) so a zoomed-in wallpaper still has real pixels to show.
    const int targetW = qMax(3840, screenSize.width() * 2);
    const int targetH = qMax(2160, screenSize.height() * 2);
    const QString target = QStringLiteral("%1 x %2 px").arg(targetW).arg(targetH);

    return QStringLiteral(
        "I attached an image. I want to use it as my desktop wallpaper on a "
        "%1 display, and I will zoom into it, so it must stay sharp and never look blurry or pixelated.\n"
        "\n"
        "Please do this in order:\n"
        "\n"
        "1. WEB RESEARCH. Use web search to identify the image: what it shows, the artist or photographer, "
        "and where it comes from. Look for the same image (or the same artwork or scene) at a higher "
        "original resolution, preferably the artist's or official source, wallpaper libraries, or "
        "the photographer's page. Give me direct links to the best sources you find and tell me their "
        "resolution. Do not invent links, and say so if you cannot find a better version.\n"
        "\n"
        "Also search for close-up photos of the same subject (same place, object, material, face or "
        "texture) and list the links; use them only as references for real fine detail.\n"
        "\n"
        "2. REPAIR AND UPSCALE. If the image is damaged (scratches, tears, blur, noise, missing or faded "
        "areas), repair those areas first, using the close-up references to rebuild believable real "
        "detail that matches my image's lighting, perspective and colour. Never paste another photo "
        "in, and tell me which areas you could not verify. Then produce the highest-quality version at "
        "about %2 (or the largest you can), keeping the same aspect ratio and composition. Use "
        "detail-preserving AI upscaling, not plain stretching: reconstruct real edges, textures, hair, "
        "text and fine lines, remove JPEG artifacts and noise, and avoid halos, waxy skin, "
        "over-smoothing or over-sharpening. Do not change the subject, colours, lighting or style.\n"
        "\n"
        "3. WALLPAPER FIT. If the aspect ratio differs from %1, tell me what is cropped or extended "
        "and prefer extending the background naturally over cropping the subject. Keep the main "
        "subject clear of the screen edges and the area where desktop icons sit.\n"
        "\n"
        "4. ZOOM CHECK. Inspect 100% and 200% crops of the centre, an edge and the most damaged area. "
        "If any crop looks soft, blurry or artificial, fix it and check again before answering.\n"
        "\n"
        "5. DELIVERY. Return the final result as a lossless PNG (or the highest-quality JPEG) at full "
        "resolution, not a compressed preview, and tell me its exact pixel size. If you cannot reach "
        "that size, give the closest you can and what limited it.")
        .arg(screen, target);
}

} // namespace WallpaperEnhancePrompt
