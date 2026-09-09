#include "D3DWallpaperRenderer.h"
#include <QDebug>
#include <d3dcompiler.h>

namespace {

// Full-screen triangle-strip quad generated entirely in the vertex shader
// from SV_VertexID - no vertex buffer needed. uv is then transformed by a
// per-frame (scale, offset) pair (uploaded via cbuffer) to implement
// Fill/Fit/Stretch/Original scaling without touching the quad geometry:
// letterbox/pillarbox bars are produced by sampling outside [0,1] and
// returning black for those texels in the pixel shader.
const char* kShaderSource = R"(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint vid : SV_VertexID) {
    float2 pos[4] = { float2(-1,-1), float2(-1,1), float2(1,-1), float2(1,1) };
    float2 uv[4]  = { float2(0,1), float2(0,0), float2(1,1), float2(1,0) };
    VSOut o;
    o.pos = float4(pos[vid], 0, 1);
    o.uv = uv[vid];
    return o;
}

Texture2D tex : register(t0);
SamplerState samp : register(s0);
cbuffer UVTransform : register(b0) { float2 scale; float2 offset; };

float4 PSMain(VSOut i) : SV_TARGET {
    float2 uv = i.uv * scale + offset;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return float4(0, 0, 0, 1);
    }
    return tex.Sample(samp, uv);
}
)";

void SafeRelease(IUnknown** obj) {
    if (*obj) {
        (*obj)->Release();
        *obj = nullptr;
    }
}

} // namespace

D3DWallpaperRenderer::D3DWallpaperRenderer(HWND hwnd, QObject* parent)
    : QObject(parent), m_hwnd(hwnd) {}

D3DWallpaperRenderer::~D3DWallpaperRenderer() {
    // Safety net only - callers are expected to call shutdown() explicitly
    // on this object's own thread before destruction/thread exit, since
    // COM release must happen on the thread that created these objects.
    if (m_initialized) {
        qWarning() << "[D3DWallpaperRenderer] destroyed without explicit shutdown() - "
                       "releasing here, but this may run on the wrong thread.";
        shutdown();
    }
}

bool D3DWallpaperRenderer::initialize(int width, int height) {
    if (m_initialized) {
        return true;
    }
    if (width <= 0 || height <= 0) {
        qWarning() << "[D3DWallpaperRenderer] initialize: invalid size" << width << height;
        return false;
    }
    m_width = width;
    m_height = height;

    if (!createDeviceAndSwapChain(width, height)) {
        return false;
    }
    if (!createShaderPipeline()) {
        return false;
    }

    m_initialized = true;
    qInfo() << "[D3DWallpaperRenderer] initialized successfully at" << width << "x" << height;
    return true;
}

bool D3DWallpaperRenderer::createDeviceAndSwapChain(int width, int height) {
    static const D3D_FEATURE_LEVEL kFeatureLevels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL obtainedLevel{};

    // D3D11_CREATE_DEVICE_BGRA_SUPPORT is required for DirectComposition
    // interop (the swap chain format DXGI_FORMAT_B8G8R8A8_UNORM needs it).
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        kFeatureLevels, ARRAYSIZE(kFeatureLevels), D3D11_SDK_VERSION,
        &m_device, &obtainedLevel, &m_context);
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] D3D11CreateDevice(HARDWARE) failed, hr=0x"
                    << Qt::hex << (unsigned)hr << "- retrying with WARP (software) driver.";
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            kFeatureLevels, ARRAYSIZE(kFeatureLevels), D3D11_SDK_VERSION,
            &m_device, &obtainedLevel, &m_context);
        if (FAILED(hr)) {
            qWarning() << "[D3DWallpaperRenderer] D3D11CreateDevice(WARP) also failed, hr=0x"
                        << Qt::hex << (unsigned)hr;
            return false;
        }
    }

    hr = m_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&m_dxgiDevice));
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] QueryInterface(IDXGIDevice) failed, hr=0x" << Qt::hex << (unsigned)hr;
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    hr = m_dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr) || !adapter) {
        qWarning() << "[D3DWallpaperRenderer] IDXGIDevice::GetAdapter failed, hr=0x" << Qt::hex << (unsigned)hr;
        return false;
    }
    IDXGIFactory2* factory = nullptr;
    hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
    adapter->Release();
    if (FAILED(hr) || !factory) {
        qWarning() << "[D3DWallpaperRenderer] IDXGIAdapter::GetParent(IDXGIFactory2) failed, hr=0x"
                    << Qt::hex << (unsigned)hr;
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width = static_cast<UINT>(width);
    scDesc.Height = static_cast<UINT>(height);
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 2;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE; // opaque wallpaper, no transparency needed
    // DXGI_SCALING_STRETCH: required for CreateSwapChainForComposition (it
    // has no target HWND to size against directly).
    scDesc.Scaling = DXGI_SCALING_STRETCH;

    hr = factory->CreateSwapChainForComposition(m_device, &scDesc, nullptr, &m_swapChain);
    factory->Release();
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] CreateSwapChainForComposition failed, hr=0x"
                    << Qt::hex << (unsigned)hr;
        return false;
    }

    hr = DCompositionCreateDevice(m_dxgiDevice, __uuidof(IDCompositionDevice),
                                   reinterpret_cast<void**>(&m_dcompDevice));
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] DCompositionCreateDevice failed, hr=0x"
                    << Qt::hex << (unsigned)hr;
        return false;
    }

    hr = m_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, &m_dcompTarget);
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] CreateTargetForHwnd failed, hr=0x" << Qt::hex << (unsigned)hr
                    << "hwnd=" << reinterpret_cast<quintptr>(m_hwnd);
        return false;
    }

    hr = m_dcompDevice->CreateVisual(&m_dcompVisual);
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] CreateVisual failed, hr=0x" << Qt::hex << (unsigned)hr;
        return false;
    }

    hr = m_dcompVisual->SetContent(m_swapChain);
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] IDCompositionVisual::SetContent failed, hr=0x"
                    << Qt::hex << (unsigned)hr;
        return false;
    }

    hr = m_dcompTarget->SetRoot(m_dcompVisual);
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] IDCompositionTarget::SetRoot failed, hr=0x"
                    << Qt::hex << (unsigned)hr;
        return false;
    }

    hr = m_dcompDevice->Commit();
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] IDCompositionDevice::Commit failed, hr=0x"
                    << Qt::hex << (unsigned)hr;
        return false;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        qWarning() << "[D3DWallpaperRenderer] swapChain->GetBuffer failed, hr=0x" << Qt::hex << (unsigned)hr;
        return false;
    }
    hr = m_device->CreateRenderTargetView(backBuffer, nullptr, &m_rtv);
    backBuffer->Release();
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] CreateRenderTargetView failed, hr=0x" << Qt::hex << (unsigned)hr;
        return false;
    }

    return true;
}

bool D3DWallpaperRenderer::createShaderPipeline() {
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* errBlob = nullptr;
    HRESULT hr = D3DCompile(kShaderSource, strlen(kShaderSource), nullptr, nullptr, nullptr,
                             "VSMain", "vs_4_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] D3DCompile(VSMain) failed, hr=0x" << Qt::hex << (unsigned)hr
                    << (errBlob ? QString::fromLatin1(static_cast<const char*>(errBlob->GetBufferPointer()),
                                                        (int)errBlob->GetBufferSize())
                                : QString());
        if (errBlob) errBlob->Release();
        return false;
    }
    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader);
    vsBlob->Release();
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] CreateVertexShader failed, hr=0x" << Qt::hex << (unsigned)hr;
        return false;
    }

    ID3DBlob* psBlob = nullptr;
    hr = D3DCompile(kShaderSource, strlen(kShaderSource), nullptr, nullptr, nullptr,
                     "PSMain", "ps_4_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] D3DCompile(PSMain) failed, hr=0x" << Qt::hex << (unsigned)hr
                    << (errBlob ? QString::fromLatin1(static_cast<const char*>(errBlob->GetBufferPointer()),
                                                        (int)errBlob->GetBufferSize())
                                : QString());
        if (errBlob) errBlob->Release();
        return false;
    }
    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader);
    psBlob->Release();
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] CreatePixelShader failed, hr=0x" << Qt::hex << (unsigned)hr;
        return false;
    }

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    hr = m_device->CreateSamplerState(&sampDesc, &m_sampler);
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] CreateSamplerState failed, hr=0x" << Qt::hex << (unsigned)hr;
        return false;
    }

    struct UvTransform { float scale[2]; float offset[2]; };
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(UvTransform);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_uvTransformBuffer);
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] CreateBuffer(UvTransform cbuffer) failed, hr=0x"
                    << Qt::hex << (unsigned)hr;
        return false;
    }

    return true;
}

void D3DWallpaperRenderer::setScalingMode(ScalingMode mode) {
    m_scalingMode = mode;
}

void D3DWallpaperRenderer::releaseSizeDependentResources() {
    if (m_rtv) { m_rtv->Release(); m_rtv = nullptr; }
}

void D3DWallpaperRenderer::resize(int width, int height) {
    if (!m_initialized || width <= 0 || height <= 0) {
        return;
    }
    if (width == m_width && height == m_height) {
        return;
    }
    m_width = width;
    m_height = height;

    releaseSizeDependentResources();

    HRESULT hr = m_swapChain->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
                                             DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] ResizeBuffers failed, hr=0x" << Qt::hex << (unsigned)hr;
        return;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        qWarning() << "[D3DWallpaperRenderer] resize: GetBuffer failed, hr=0x" << Qt::hex << (unsigned)hr;
        return;
    }
    hr = m_device->CreateRenderTargetView(backBuffer, nullptr, &m_rtv);
    backBuffer->Release();
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] resize: CreateRenderTargetView failed, hr=0x"
                    << Qt::hex << (unsigned)hr;
    }
}

void D3DWallpaperRenderer::computeUvTransform(const QSize& imgSize, float outScale[2], float outOffset[2]) const {
    if (imgSize.width() <= 0 || imgSize.height() <= 0 || m_width <= 0 || m_height <= 0) {
        outScale[0] = outScale[1] = 1.0f;
        outOffset[0] = outOffset[1] = 0.0f;
        return;
    }
    const float imgAR = static_cast<float>(imgSize.width()) / static_cast<float>(imgSize.height());
    const float widgetAR = static_cast<float>(m_width) / static_cast<float>(m_height);

    switch (m_scalingMode) {
        case ScalingMode::Stretch:
            outScale[0] = 1.0f; outScale[1] = 1.0f;
            outOffset[0] = 0.0f; outOffset[1] = 0.0f;
            break;
        case ScalingMode::Fill:
            // Cover: crop whichever axis has excess.
            if (widgetAR > imgAR) {
                outScale[0] = 1.0f;
                outScale[1] = imgAR / widgetAR;
            } else {
                outScale[0] = widgetAR / imgAR;
                outScale[1] = 1.0f;
            }
            outOffset[0] = (1.0f - outScale[0]) / 2.0f;
            outOffset[1] = (1.0f - outScale[1]) / 2.0f;
            break;
        case ScalingMode::Fit:
            // Contain: letterbox/pillarbox whichever axis has slack -
            // achieved by letting the UV range exceed [0,1] on that axis,
            // which the pixel shader renders as black.
            if (widgetAR > imgAR) {
                outScale[0] = widgetAR / imgAR;
                outScale[1] = 1.0f;
            } else {
                outScale[0] = 1.0f;
                outScale[1] = imgAR / widgetAR;
            }
            outOffset[0] = (1.0f - outScale[0]) / 2.0f;
            outOffset[1] = (1.0f - outScale[1]) / 2.0f;
            break;
        case ScalingMode::Original:
        default:
            outScale[0] = static_cast<float>(m_width) / static_cast<float>(imgSize.width());
            outScale[1] = static_cast<float>(m_height) / static_cast<float>(imgSize.height());
            outOffset[0] = (1.0f - outScale[0]) / 2.0f;
            outOffset[1] = (1.0f - outScale[1]) / 2.0f;
            break;
    }
}

void D3DWallpaperRenderer::updateSourceTexture(const QImage& img) {
    if (img.size() != m_sourceTextureSize) {
        if (m_sourceSrv) { m_sourceSrv->Release(); m_sourceSrv = nullptr; }
        if (m_sourceTexture) { m_sourceTexture->Release(); m_sourceTexture = nullptr; }

        D3D11_TEXTURE2D_DESC texDesc{};
        texDesc.Width = static_cast<UINT>(img.width());
        texDesc.Height = static_cast<UINT>(img.height());
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // matches QImage::Format_RGB32 byte layout
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_sourceTexture);
        if (FAILED(hr)) {
            qWarning() << "[D3DWallpaperRenderer] CreateTexture2D(source) failed, hr=0x" << Qt::hex << (unsigned)hr;
            m_sourceTextureSize = QSize();
            return;
        }
        hr = m_device->CreateShaderResourceView(m_sourceTexture, nullptr, &m_sourceSrv);
        if (FAILED(hr)) {
            qWarning() << "[D3DWallpaperRenderer] CreateShaderResourceView failed, hr=0x"
                        << Qt::hex << (unsigned)hr;
            m_sourceTextureSize = QSize();
            return;
        }
        m_sourceTextureSize = img.size();
    }

    // A CPU->GPU copy is unavoidable here: VideoPlayer only exposes
    // software-decoded QImage frames (Qt Multimedia's FFmpeg backend on
    // this system was not observed to expose a GPU-resident frame handle
    // usable here - see CLAUDE.md). UpdateSubresource on a DEFAULT-usage
    // texture is the practical, standard upload path for this case.
    m_context->UpdateSubresource(m_sourceTexture, 0, nullptr, img.constBits(),
                                  static_cast<UINT>(img.bytesPerLine()), 0);
}

void D3DWallpaperRenderer::presentFrame(std::shared_ptr<const QImage> frame) {
    if (!m_initialized || !frame || frame->isNull()) {
        return;
    }

    updateSourceTexture(*frame);
    if (!m_sourceSrv) {
        return;
    }

    float scale[2], offset[2];
    computeUvTransform(frame->size(), scale, offset);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = m_context->Map(m_uvTransformBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        struct UvTransform { float scale[2]; float offset[2]; };
        auto* dst = static_cast<UvTransform*>(mapped.pData);
        dst->scale[0] = scale[0]; dst->scale[1] = scale[1];
        dst->offset[0] = offset[0]; dst->offset[1] = offset[1];
        m_context->Unmap(m_uvTransformBuffer, 0);
    }

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &viewport);

    m_context->OMSetRenderTargets(1, &m_rtv, nullptr);
    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_context->ClearRenderTargetView(m_rtv, clearColor);

    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_context->VSSetShader(m_vertexShader, nullptr, 0);
    m_context->PSSetShader(m_pixelShader, nullptr, 0);
    m_context->PSSetShaderResources(0, 1, &m_sourceSrv);
    m_context->PSSetSamplers(0, 1, &m_sampler);
    m_context->PSSetConstantBuffers(0, 1, &m_uvTransformBuffer);
    m_context->Draw(4, 0);

    // Sync interval 1: present synced to vblank. This call blocks on this
    // object's own dedicated render thread (see D3DWallpaperRenderer.h) -
    // never on the GUI thread - so it cannot affect UI responsiveness.
    hr = m_swapChain->Present(1, 0);
    if (FAILED(hr) && hr != DXGI_STATUS_OCCLUDED) {
        qWarning() << "[D3DWallpaperRenderer] Present failed, hr=0x" << Qt::hex << (unsigned)hr;
        return;
    }
    presentedFrameCount.fetch_add(1, std::memory_order_relaxed);
}

void D3DWallpaperRenderer::rebindToWindow(HWND newHwnd) {
    if (!m_initialized || !m_dcompDevice) {
        qWarning() << "[D3DWallpaperRenderer] rebindToWindow called before initialize() - nothing to rebind.";
        emit rebindFinished(false);
        return;
    }

    // The old target was bound to a now-destroyed HWND; release it before
    // creating a new one. The visual (and everything it contains - swap
    // chain, D3D11 device/context, shaders, textures) is untouched and
    // reused as-is. This method runs on this object's own render thread
    // (queued call), so it naturally serializes with any presentFrame()
    // call already in progress or still pending - no separate locking
    // needed for that.
    SafeRelease(reinterpret_cast<IUnknown**>(&m_dcompTarget));
    m_hwnd = newHwnd;

    HRESULT hr = m_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, &m_dcompTarget);
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] rebindToWindow: CreateTargetForHwnd failed, hr=0x"
                    << Qt::hex << (unsigned)hr << "hwnd=" << reinterpret_cast<quintptr>(m_hwnd);
        emit rebindFinished(false);
        return;
    }
    hr = m_dcompTarget->SetRoot(m_dcompVisual);
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] rebindToWindow: SetRoot failed, hr=0x" << Qt::hex << (unsigned)hr;
        emit rebindFinished(false);
        return;
    }
    hr = m_dcompDevice->Commit();
    if (FAILED(hr)) {
        qWarning() << "[D3DWallpaperRenderer] rebindToWindow: Commit failed, hr=0x" << Qt::hex << (unsigned)hr;
        emit rebindFinished(false);
        return;
    }

    qInfo() << "[DComp] Composition target rebound to new hwnd=" << reinterpret_cast<quintptr>(m_hwnd)
            << "- composition committed.";
    emit rebindFinished(true);
}

void D3DWallpaperRenderer::recommitAfterReparent() {
    if (!m_dcompDevice) {
        qWarning() << "[DComp] recommitAfterReparent: no DComp device - nothing to commit.";
        return;
    }
    HRESULT hr = m_dcompDevice->Commit();
    qInfo() << "[DComp] recommitAfterReparent: Commit() after SetParent into desktop hierarchy, hr=0x"
            << Qt::hex << (unsigned)hr << Qt::dec << "presentedFrameCount=" << presentedFrameCount.load();
}

void D3DWallpaperRenderer::shutdown() {
    if (!m_initialized && !m_device) {
        return;
    }
    SafeRelease(reinterpret_cast<IUnknown**>(&m_sourceSrv));
    SafeRelease(reinterpret_cast<IUnknown**>(&m_sourceTexture));
    SafeRelease(reinterpret_cast<IUnknown**>(&m_uvTransformBuffer));
    SafeRelease(reinterpret_cast<IUnknown**>(&m_sampler));
    SafeRelease(reinterpret_cast<IUnknown**>(&m_pixelShader));
    SafeRelease(reinterpret_cast<IUnknown**>(&m_vertexShader));
    SafeRelease(reinterpret_cast<IUnknown**>(&m_rtv));
    if (m_dcompDevice) {
        m_dcompDevice->Commit();
    }
    SafeRelease(reinterpret_cast<IUnknown**>(&m_dcompVisual));
    SafeRelease(reinterpret_cast<IUnknown**>(&m_dcompTarget));
    SafeRelease(reinterpret_cast<IUnknown**>(&m_dcompDevice));
    SafeRelease(reinterpret_cast<IUnknown**>(&m_swapChain));
    SafeRelease(reinterpret_cast<IUnknown**>(&m_dxgiDevice));
    SafeRelease(reinterpret_cast<IUnknown**>(&m_context));
    SafeRelease(reinterpret_cast<IUnknown**>(&m_device));
    m_initialized = false;
    m_sourceTextureSize = QSize();
    qInfo() << "[D3DWallpaperRenderer] shutdown complete, all D3D/DComp resources released.";
}
