#pragma once

#include <QImage>
#include <QSize>
#include <cstdint>
#include <memory>
#include <vector>

#include "BinaryRainConfig.h"

// The single shared renderer behind every Binary Rain preset: a CPU raster
// simulation of falling 0/1 streams. Deterministic given a seed (own xorshift
// RNG), no Qt GUI/fonts/Win32 use (glyphs are drawn as vector shapes), and no
// per-frame heap allocation: all state and glyph masks are sized in resize()/
// setParams() and reused. VideoPlayer owns one and publishes its frames
// through the existing frameReady contract.
class BinaryRainEngine {
public:
    BinaryRainEngine();

    void seed(uint32_t value);
    // Validated copy is stored; invalid values never reach the simulation.
    // Cell-size changes rebuild the grid/masks (allocation); other changes
    // only update scalars.
    void setParams(const BinaryRain::Params& params);
    const BinaryRain::Params& params() const { return m_params; }
    // Frame size in pixels (clamped to >= 16x16). No-op if unchanged.
    void resize(const QSize& frameSize);
    QSize size() const { return m_size; }

    // Advance the simulation; dt is clamped to 0.1 s so a stall never makes
    // streams jump.
    void advance(double dtSeconds);
    // Draws the current state into img (must be Format_RGB32 at size()).
    // Returns false (and draws nothing) on a mismatched image.
    bool render(QImage& img) const;

    int columns() const { return m_cols; }
    int rows() const { return m_rows; }
    int cellHeight() const { return m_cellH; }
    int activeStreamCount() const;

private:
    struct Stream {
        float head = 0;     // row position of the leading glyph (fractional)
        float rank = 0;     // fixed [0,1): column carries a stream while rank < density
        float speed = 0;    // rows per second at 100% speed
        float length = 0;   // trail length in rows
        float delay = 0;    // seconds until (re)spawn while inactive
        bool active = false;
    };

    uint32_t nextU32();
    float nextF(); // [0,1)
    void rebuildGrid();
    void rebuildMasks();
    void spawn(Stream& s, bool atTop);
    void scheduleRespawn(Stream& s);
    void randomizeAll();

    BinaryRain::Params m_params;
    QSize m_size{1280, 720};
    int m_cellW = 0, m_cellH = 0, m_cols = 0, m_rows = 0, m_xOff = 0, m_yOff = 0;
    uint32_t m_rng = 0x9E3779B9u;
    float m_density = 0.6f;    // 0..1
    float m_speedScale = 1.0f; // speed % / 100
    std::vector<Stream> m_streams;      // one per column
    std::vector<uint8_t> m_glyphs;      // cols*rows, 0 or 1
    std::vector<uint8_t> m_mask[2];     // cellW*cellH coverage
    std::vector<uint8_t> m_glowMask[2]; // 3cellW*3cellH soft halo
};

// A tiny recycling pool of frame buffers. A buffer is reusable once no
// consumer (preview, WallpaperWindow render threads) still holds it, so a
// steady animation allocates nothing after the first few frames.
class BinaryRainFramePool {
public:
    // Returns a buffer of the requested size, or null when every buffer is
    // still held by a consumer (caller should skip this frame).
    std::shared_ptr<QImage> acquire(const QSize& size);
    void clear() { m_buffers.clear(); }
    int bufferCount() const { return static_cast<int>(m_buffers.size()); }

    // Enough for the player's current frame, the preview and one in-flight
    // frame per monitor window; beyond that frames are skipped, not queued.
    static constexpr int kMaxBuffers = 8;

private:
    std::vector<std::shared_ptr<QImage>> m_buffers;
};
