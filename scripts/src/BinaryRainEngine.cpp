#include "BinaryRainEngine.h"

#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace {

constexpr double kMaxStep = 0.1; // seconds; longer stalls are clamped

// Draws glyph '0' or '1' as vector shapes inside the w x h box at (ox, oy).
void drawGlyph(QPainter& p, int which, qreal ox, qreal oy, qreal w, qreal h) {
    QPen pen(Qt::white, qMax<qreal>(1.0, h * 0.10), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    if (which == 0) {
        p.drawEllipse(QRectF(ox + w * 0.22, oy + h * 0.14, w * 0.56, h * 0.72));
    } else {
        QPainterPath path;
        path.moveTo(ox + w * 0.30, oy + h * 0.30);
        path.lineTo(ox + w * 0.52, oy + h * 0.14);
        path.lineTo(ox + w * 0.52, oy + h * 0.86);
        path.moveTo(ox + w * 0.30, oy + h * 0.86);
        path.lineTo(ox + w * 0.74, oy + h * 0.86);
        p.drawPath(path);
    }
}

std::vector<uint8_t> alphaOf(const QImage& img) {
    std::vector<uint8_t> out(static_cast<size_t>(img.width()) * img.height());
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            out[static_cast<size_t>(y) * img.width() + x] = static_cast<uint8_t>(qAlpha(line[x]));
        }
    }
    return out;
}

// One separable box-blur pass over a w x h 8-bit plane (setup only).
void boxBlur(std::vector<uint8_t>& plane, int w, int h, int radius) {
    if (radius < 1) return;
    std::vector<uint8_t> tmp(plane.size());
    const int span = radius * 2 + 1;
    for (int y = 0; y < h; ++y) { // horizontal
        int sum = 0;
        for (int x = -radius; x <= radius; ++x) sum += plane[y * w + std::clamp(x, 0, w - 1)] * (x >= 0 && x < w);
        for (int x = 0; x < w; ++x) {
            tmp[y * w + x] = static_cast<uint8_t>(sum / span);
            const int add = x + radius + 1, sub = x - radius;
            sum += (add < w ? plane[y * w + add] : 0) - (sub >= 0 ? plane[y * w + sub] : 0);
        }
    }
    for (int x = 0; x < w; ++x) { // vertical
        int sum = 0;
        for (int y = -radius; y <= radius; ++y) sum += tmp[std::clamp(y, 0, h - 1) * w + x] * (y >= 0 && y < h);
        for (int y = 0; y < h; ++y) {
            plane[y * w + x] = static_cast<uint8_t>(sum / span);
            const int add = y + radius + 1, sub = y - radius;
            sum += (add < h ? tmp[add * w + x] : 0) - (sub >= 0 ? tmp[sub * w + x] : 0);
        }
    }
}

} // namespace

BinaryRainEngine::BinaryRainEngine() {
    m_params = BinaryRain::presetParams(BinaryRain::Preset::Classic);
    m_density = m_params.density / 100.0f;
    m_speedScale = m_params.speed / 100.0f;
    rebuildGrid();
    rebuildMasks();
}

uint32_t BinaryRainEngine::nextU32() {
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return m_rng;
}

float BinaryRainEngine::nextF() {
    return static_cast<float>(nextU32() >> 8) * (1.0f / 16777216.0f);
}

void BinaryRainEngine::seed(uint32_t value) {
    m_rng = value ? value : 0x9E3779B9u;
    randomizeAll();
}

void BinaryRainEngine::setParams(const BinaryRain::Params& params) {
    const BinaryRain::Params next = BinaryRain::validated(params);
    const int oldCell = m_cellH;
    const float oldDensity = m_density;
    m_params = next;
    m_density = next.density / 100.0f;
    m_speedScale = next.speed / 100.0f;
    const int wantCell = std::max(6, static_cast<int>(std::lround(next.charSize * m_size.height() / 1080.0)));
    if (wantCell != oldCell) {
        rebuildGrid();
        rebuildMasks();
        return;
    }
    if (m_density > oldDensity) { // raising density: let idle columns start soon
        for (Stream& s : m_streams) {
            if (!s.active && s.rank < m_density) s.delay = std::min(s.delay, nextF() * 0.8f);
        }
    }
}

void BinaryRainEngine::resize(const QSize& frameSize) {
    const QSize clamped(std::max(16, frameSize.width()), std::max(16, frameSize.height()));
    if (clamped == m_size) return;
    m_size = clamped;
    rebuildGrid();
    rebuildMasks();
}

void BinaryRainEngine::rebuildGrid() {
    m_cellH = std::max(6, static_cast<int>(std::lround(m_params.charSize * m_size.height() / 1080.0)));
    m_cellW = std::max(4, static_cast<int>(std::lround(m_cellH * 0.6)));
    m_cols = std::max(1, m_size.width() / m_cellW);
    m_rows = std::max(1, m_size.height() / m_cellH);
    m_xOff = (m_size.width() - m_cols * m_cellW) / 2;
    m_yOff = (m_size.height() - m_rows * m_cellH) / 2;
    m_streams.assign(static_cast<size_t>(m_cols), Stream());
    m_glyphs.assign(static_cast<size_t>(m_cols) * m_rows, 0);
    randomizeAll();
}

void BinaryRainEngine::rebuildMasks() {
    for (int which = 0; which < 2; ++which) {
        QImage g(m_cellW, m_cellH, QImage::Format_ARGB32_Premultiplied);
        g.fill(Qt::transparent);
        {
            QPainter p(&g);
            p.setRenderHint(QPainter::Antialiasing, true);
            drawGlyph(p, which, 0, 0, m_cellW, m_cellH);
        }
        m_mask[which] = alphaOf(g);

        const int gw = m_cellW * 3, gh = m_cellH * 3;
        QImage big(gw, gh, QImage::Format_ARGB32_Premultiplied);
        big.fill(Qt::transparent);
        {
            QPainter p(&big);
            p.setRenderHint(QPainter::Antialiasing, true);
            drawGlyph(p, which, m_cellW, m_cellH, m_cellW, m_cellH);
        }
        std::vector<uint8_t> halo = alphaOf(big);
        const int radius = std::max(2, m_cellH / 3);
        boxBlur(halo, gw, gh, radius);
        boxBlur(halo, gw, gh, radius);
        for (uint8_t& v : halo) v = static_cast<uint8_t>(std::min(255, v * 4)); // blur lowers the peak
        m_glowMask[which] = std::move(halo);
    }
}

void BinaryRainEngine::scheduleRespawn(Stream& s) {
    s.active = false;
    s.delay = nextF() * (0.2f + (1.0f - m_density) * 6.0f);
}

void BinaryRainEngine::spawn(Stream& s, bool atTop) {
    const float pxPerSec = static_cast<float>(m_size.height()) * (0.18f + 0.30f * nextF());
    s.speed = pxPerSec / static_cast<float>(m_cellH);
    s.length = std::clamp(8.0f + nextF() * m_rows * 0.6f, 6.0f, static_cast<float>(m_rows));
    s.head = atTop ? -1.0f : nextF() * (m_rows + s.length) - s.length;
    s.active = true;
}

void BinaryRainEngine::randomizeAll() {
    for (uint8_t& g : m_glyphs) g = static_cast<uint8_t>(nextU32() & 1);
    for (Stream& s : m_streams) {
        s.rank = nextF();
        if (s.rank < m_density) {
            spawn(s, false); // staggered start: screen is populated immediately
        } else {
            s.active = false;
            s.delay = nextF() * (0.2f + (1.0f - m_density) * 6.0f);
        }
    }
}

int BinaryRainEngine::activeStreamCount() const {
    int n = 0;
    for (const Stream& s : m_streams) n += s.active ? 1 : 0;
    return n;
}

void BinaryRainEngine::advance(double dtSeconds) {
    const float dt = static_cast<float>(std::clamp(dtSeconds, 0.0, kMaxStep));
    if (dt <= 0.0f) return;
    for (int c = 0; c < m_cols; ++c) {
        Stream& s = m_streams[static_cast<size_t>(c)];
        uint8_t* col = &m_glyphs[static_cast<size_t>(c) * m_rows];
        if (!s.active) {
            s.delay -= dt;
            if (s.delay <= 0.0f && s.rank < m_density) spawn(s, true);
            else if (s.delay <= 0.0f) s.delay = 0.5f; // not allowed at this density; re-check later
            continue;
        }
        const float old = s.head;
        s.head += s.speed * m_speedScale * dt;
        for (int r = static_cast<int>(std::floor(old)) + 1; r <= static_cast<int>(std::floor(s.head)); ++r) {
            if (r >= 0 && r < m_rows) col[r] = static_cast<uint8_t>(nextU32() & 1);
        }
        if (nextF() < dt * 3.0f) { // occasional glyph flicker inside the trail
            const int r = static_cast<int>(s.head - nextF() * s.length);
            if (r >= 0 && r < m_rows) col[r] ^= 1;
        }
        if (s.head - s.length > m_rows) scheduleRespawn(s);
    }
}

namespace {

// Additively blends colour (cr,cg,cb) through an 8-bit mask into an RGB32
// buffer; strength is 0..256. Clipped to the buffer.
void blitAdd(uint32_t* base, int W, int H, const uint8_t* mask, int mw, int mh, int x0, int y0, int cr, int cg,
             int cb, int strength) {
    if (strength <= 0) return;
    const int xs = std::max(0, -x0), xe = std::min(mw, W - x0);
    const int ys = std::max(0, -y0), ye = std::min(mh, H - y0);
    for (int y = ys; y < ye; ++y) {
        uint32_t* dst = base + static_cast<size_t>(y0 + y) * W + x0;
        const uint8_t* m = mask + static_cast<size_t>(y) * mw;
        for (int x = xs; x < xe; ++x) {
            const int a = (m[x] * strength) >> 8;
            if (a == 0) continue;
            const uint32_t px = dst[x];
            const int r = std::min(255, static_cast<int>((px >> 16) & 0xFF) + ((cr * a) >> 8));
            const int g = std::min(255, static_cast<int>((px >> 8) & 0xFF) + ((cg * a) >> 8));
            const int b = std::min(255, static_cast<int>(px & 0xFF) + ((cb * a) >> 8));
            dst[x] = 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
                     static_cast<uint32_t>(b);
        }
    }
}

} // namespace

bool BinaryRainEngine::render(QImage& img) const {
    if (img.format() != QImage::Format_RGB32 || img.size() != m_size) return false;
    const int W = m_size.width(), H = m_size.height();
    const float dark = m_params.backgroundDarkness / 100.0f;
    const uint32_t bg = 0xFF000000u | (static_cast<uint32_t>(std::lround(20 * (1.0f - dark))) << 8) |
                        static_cast<uint32_t>(std::lround(8 * (1.0f - dark)));
    for (int y = 0; y < H; ++y) {
        uint32_t* line = reinterpret_cast<uint32_t*>(img.scanLine(y));
        std::fill(line, line + W, bg);
    }
    uint32_t* base = reinterpret_cast<uint32_t*>(img.bits());
    if (img.bytesPerLine() != W * 4) return false; // tightly packed rows expected from QImage(size, RGB32)

    const float bright = m_params.brightness / 100.0f;
    const int glowStrength = static_cast<int>(256 * (m_params.glow / 100.0f));
    for (int c = 0; c < m_cols; ++c) {
        const Stream& s = m_streams[static_cast<size_t>(c)];
        if (!s.active) continue;
        const uint8_t* col = &m_glyphs[static_cast<size_t>(c) * m_rows];
        const int rTop = std::max(0, static_cast<int>(std::ceil(s.head - s.length)));
        const int rBot = std::min(m_rows - 1, static_cast<int>(std::floor(s.head)));
        const int x0 = m_xOff + c * m_cellW;
        for (int r = rTop; r <= rBot; ++r) {
            const float dist = s.head - static_cast<float>(r);
            const float t = dist / s.length;
            if (t >= 1.0f) continue;
            const int y0 = m_yOff + r * m_cellH;
            const int glyph = col[r];
            if (dist < 1.0f) { // head: near-white green, strongest glow
                const int k = static_cast<int>(256 * bright);
                blitAdd(base, W, H, m_glowMask[glyph].data(), m_cellW * 3, m_cellH * 3, x0 - m_cellW, y0 - m_cellH,
                        60, 255, 90, (glowStrength * k) >> 8);
                blitAdd(base, W, H, m_mask[glyph].data(), m_cellW, m_cellH, x0, y0, 200, 255, 210, k);
            } else {
                const float fade = (1.0f - t) * (1.0f - t);
                const int k = static_cast<int>(256 * bright * fade);
                if (dist < 5.0f) { // soft halo behind the first few trail glyphs
                    blitAdd(base, W, H, m_glowMask[glyph].data(), m_cellW * 3, m_cellH * 3, x0 - m_cellW,
                            y0 - m_cellH, 0, 200, 60, (((glowStrength * k) >> 8) * (5 - static_cast<int>(dist))) / 10);
                }
                blitAdd(base, W, H, m_mask[glyph].data(), m_cellW, m_cellH, x0, y0, 20, 255, 70, k);
            }
        }
    }
    return true;
}

std::shared_ptr<QImage> BinaryRainFramePool::acquire(const QSize& size) {
    for (std::shared_ptr<QImage>& b : m_buffers) {
        if (b.use_count() == 1) { // only the pool holds it
            if (b->size() != size) *b = QImage(size, QImage::Format_RGB32);
            return b;
        }
    }
    if (static_cast<int>(m_buffers.size()) < kMaxBuffers) {
        m_buffers.push_back(std::make_shared<QImage>(size, QImage::Format_RGB32));
        return m_buffers.back();
    }
    return nullptr;
}
