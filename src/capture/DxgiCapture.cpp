#include "DxgiCapture.h"
#include "MonitorInfo.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

// ---- enumerateMonitors (used by MonitorPickerDialog too) --------------------

QList<MonitorInfo> enumerateMonitors() {
    QList<MonitorInfo> result;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(&factory))))
        return result;

    IDXGIAdapter1* adapter = nullptr;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
        IDXGIOutput* output = nullptr;
        for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi) {
            DXGI_OUTPUT_DESC desc = {};
            output->GetDesc(&desc);
            if (desc.AttachedToDesktop) {
                MonitorInfo mi;
                mi.name = QString::fromWCharArray(desc.DeviceName);
                mi.geometry = QRect(
                    desc.DesktopCoordinates.left,
                    desc.DesktopCoordinates.top,
                    desc.DesktopCoordinates.right  - desc.DesktopCoordinates.left,
                    desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
                mi.adapterIndex = static_cast<int>(ai);
                mi.outputIndex  = static_cast<int>(oi);
                result.append(mi);
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return result;
}

// ---- DxgiCapture ------------------------------------------------------------

DxgiCapture::DxgiCapture(int adapterIndex, int outputIndex, QObject* parent)
    : QThread(parent), m_adapterIndex(adapterIndex), m_outputIndex(outputIndex) {}

DxgiCapture::~DxgiCapture() {
    requestStop();
    wait(4000);
}

void DxgiCapture::requestStop() {
    m_running.store(false, std::memory_order_relaxed);
}

void DxgiCapture::run() {
    m_running.store(true, std::memory_order_relaxed);

    // --- Create DXGI factory and select the right adapter ---
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(&factory)))) {
        emit captureError(QStringLiteral("CreateDXGIFactory1 failed"));
        return;
    }

    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(static_cast<UINT>(m_adapterIndex), &adapter)
        == DXGI_ERROR_NOT_FOUND) {
        factory->Release();
        emit captureError(QStringLiteral("Adapter not found"));
        return;
    }
    factory->Release();

    // --- Create D3D11 device on that adapter ---
    D3D_FEATURE_LEVEL featureLevel;
    ID3D11Device*        device  = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDevice(
        adapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION,
        &device, &featureLevel, &context);
    adapter->Release();

    if (FAILED(hr)) {
        emit captureError(QStringLiteral("D3D11CreateDevice failed: 0x%1").arg(hr, 8, 16));
        return;
    }

    // Re-acquire the adapter through the device so we can reach its outputs.
    IDXGIDevice*  dxgiDev  = nullptr;
    IDXGIAdapter* dxgiAdap = nullptr;
    device->QueryInterface(__uuidof(IDXGIDevice),  reinterpret_cast<void**>(&dxgiDev));
    dxgiDev->GetParent(  __uuidof(IDXGIAdapter), reinterpret_cast<void**>(&dxgiAdap));
    dxgiDev->Release();

    // --- Grab the chosen output ---
    IDXGIOutput* output = nullptr;
    if (dxgiAdap->EnumOutputs(static_cast<UINT>(m_outputIndex), &output)
        == DXGI_ERROR_NOT_FOUND) {
        dxgiAdap->Release();
        device->Release(); context->Release();
        emit captureError(QStringLiteral("Output not found"));
        return;
    }
    dxgiAdap->Release();

    IDXGIOutput1* output1 = nullptr;
    output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
    output->Release();

    // --- Create the duplication ---
    IDXGIOutputDuplication* dup = nullptr;
    hr = output1->DuplicateOutput(device, &dup);
    output1->Release();

    if (FAILED(hr)) {
        device->Release(); context->Release();
        QString reason;
        if (hr == static_cast<HRESULT>(DXGI_ERROR_UNSUPPORTED))
            reason = QStringLiteral("unsupported (another process may own this output)");
        else if (hr == E_ACCESSDENIED)
            reason = QStringLiteral("access denied (desktop locked?)");
        else
            reason = QStringLiteral("0x%1").arg(static_cast<quint32>(hr), 8, 16);
        emit captureError(QStringLiteral("DuplicateOutput failed: ") + reason);
        return;
    }

    // --- Determine resolution and create a CPU-readable staging texture ---
    DXGI_OUTDUPL_DESC dupDesc = {};
    dup->GetDesc(&dupDesc);
    const int W = static_cast<int>(dupDesc.ModeDesc.Width);
    const int H = static_cast<int>(dupDesc.ModeDesc.Height);

    D3D11_TEXTURE2D_DESC stageDesc = {};
    stageDesc.Width            = static_cast<UINT>(W);
    stageDesc.Height           = static_cast<UINT>(H);
    stageDesc.MipLevels        = 1;
    stageDesc.ArraySize        = 1;
    stageDesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    stageDesc.SampleDesc.Count = 1;
    stageDesc.Usage            = D3D11_USAGE_STAGING;
    stageDesc.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* staging = nullptr;
    device->CreateTexture2D(&stageDesc, nullptr, &staging);

    // --- Capture loop (~30 fps via 33 ms timeout) ---
    while (m_running.load(std::memory_order_relaxed)) {
        DXGI_OUTDUPL_FRAME_INFO info = {};
        IDXGIResource* resource = nullptr;
        hr = dup->AcquireNextFrame(33, &info, &resource);

        if (hr == static_cast<HRESULT>(DXGI_ERROR_WAIT_TIMEOUT)) continue;

        if (hr == static_cast<HRESULT>(DXGI_ERROR_ACCESS_LOST)) {
            // Monitor config changed — signal caller to recreate.
            emit captureError(QStringLiteral("Access lost — monitor config changed"));
            break;
        }
        if (FAILED(hr)) break;

        // Only copy when a new desktop frame was actually presented.
        if (info.LastPresentTime.QuadPart != 0) {
            ID3D11Texture2D* tex = nullptr;
            resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                     reinterpret_cast<void**>(&tex));

            context->CopyResource(staging, tex);
            tex->Release();

            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
                // BGRA → QImage::Format_ARGB32 is a straight byte-for-byte match
                // on little-endian (x86): BGRA memory == 0xAARRGGBB integer.
                QImage frame(W, H, QImage::Format_ARGB32);
                const auto* src = reinterpret_cast<const quint8*>(mapped.pData);
                for (int row = 0; row < H; ++row) {
                    memcpy(frame.scanLine(row),
                           src + row * mapped.RowPitch,
                           static_cast<size_t>(W) * 4);
                }
                context->Unmap(staging, 0);
                emit frameReady(frame);
            }
        }

        resource->Release();
        dup->ReleaseFrame();
    }

    // --- Cleanup ---
    if (staging) staging->Release();
    dup->Release();
    context->Release();
    device->Release();
}
