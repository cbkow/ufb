// D3D11DeviceManager — Phase F.2.1.
//
// Centralized ID3D11Device + ID3D11DeviceContext + IDXGIFactory2 for
// the native player surface on Windows. Singleton — one D3D11 device
// per process, matching MetalDeviceManager's shape (Guide 19 §2.1).
//
// Unlike Metal, the D3D11 device is HWND-independent: the renderer
// creates its own child HWND + DComp swapchain on top of this device.
// So initialize() takes no QWindow argument.
//
// Public API returns ID3D11* / IDXGI* types as void* so this header
// is usable from plain .cpp without dragging in <d3d11.h>. The
// implementation in d3d11_device_manager.cpp does the casts.
//
// Windows-only file. Compiled via the `if(WIN32)` block in
// src/render/CMakeLists.txt.

#pragma once

#include <QString>

#include <memory>
#include <mutex>

namespace ufbplayer {

class D3D11DeviceManager {
public:
    static D3D11DeviceManager &instance();

    D3D11DeviceManager(const D3D11DeviceManager &)            = delete;
    D3D11DeviceManager &operator=(const D3D11DeviceManager &) = delete;

    // Creates the D3D11 device (hardware driver, BGRA support enabled
    // because DComp requires it), immediate context, and DXGI factory.
    // Idempotent: returns true if already initialized. Returns false
    // on first-time creation failure.
    bool initialize();

    // UFB injection path. Unlike QCView (which owns a native D3D11
    // swapchain on its own device), UFB renders the lightbox into a
    // Qt QQuickRhiItem whose QRhi already owns an ID3D11Device. The
    // Vulkan→D3D11 bridge's output texture MUST live on that same
    // device for QRhiTexture::createFrom to wrap it, so we adopt Qt's
    // device/context instead of creating our own. `device` and
    // `context` are ID3D11Device* / ID3D11DeviceContext* passed as
    // void* (from QRhi's nativeHandles). AddRef'd for our lifetime;
    // the DXGI factory is derived from the device. Idempotent.
    bool initializeWithDevice(void *device, void *context);

    void shutdown();
    bool isInitialized() const { return m_initialized; }

    // Raw COM types returned as void* so this header doesn't drag in
    // the Windows headers. Cast on the .cpp side:
    //   ID3D11Device        *d = static_cast<ID3D11Device*>(mgr.device());
    //   ID3D11DeviceContext *c = static_cast<ID3D11DeviceContext*>(mgr.context());
    //   IDXGIFactory2       *f = static_cast<IDXGIFactory2*>(mgr.factory());
    void *device()  const;
    void *context() const;
    void *factory() const;

    // Diagnostics — populated during initialize().
    QString adapterDescription() const;
    unsigned int featureLevel()  const;   // D3D_FEATURE_LEVEL as uint
    bool         hasDebugLayer() const;

private:
    D3D11DeviceManager();
    ~D3D11DeviceManager();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool                  m_initialized = false;
    mutable std::mutex    m_mutex;
};

} // namespace ufbplayer
