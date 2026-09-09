#include "WgcCapture.h"
#include "CaptureCallbackGate.h"
#include "platform/FrameProfile.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>

#include <windows.foundation.h>
#include <windows.graphics.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.h>

#include <atomic>
#include <mutex>
#include <utility>

namespace wf    = ABI::Windows::Foundation;
namespace wg    = ABI::Windows::Graphics;
namespace wgc   = ABI::Windows::Graphics::Capture;
namespace wgdx  = ABI::Windows::Graphics::DirectX;
namespace wgd3d = ABI::Windows::Graphics::DirectX::Direct3D11;

// Two pieces of the WinRT interop surface that this toolchain's headers do not
// carry. Both are stable published ABI: the function is exported by d3d11.dll
// and is in the import library already linked, and the interface is how a
// WinRT surface hands back the D3D texture underneath it. Declared here rather
// than worked around, because the alternative is a second device and a copy
// between them.
extern "C" HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice*, IInspectable**);

struct IDirect3DDxgiInterfaceAccess : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** object) = 0;
};
static const GUID kIID_IDirect3DDxgiInterfaceAccess =
    {0xA9B3D012, 0x3DF2, 0x4EE3, {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}};

// __uuidof here is a one-argument macro, so a template specialisation with a
// comma in it needs a name of its own before it can be asked for its IID.
using FrameArrivedHandler =
    wf::ITypedEventHandler<wgc::Direct3D11CaptureFramePool*, IInspectable*>;
using ItemClosedHandler =
    wf::ITypedEventHandler<wgc::GraphicsCaptureItem*, IInspectable*>;

namespace {

// Frames handed to the consumer but not yet released by it.
//
// Two lets one frame be in flight while the next arrives, without letting a
// backlog form. The same bound and the same reasoning as the DXGI backend: at
// roughly 8 MB a frame, an unbounded path costs memory instead of frames, and
// frames are the cheaper thing to lose.
constexpr int kMaxInFlightFrames = 2;

// Buffers in the frame pool. Two is what the platform samples use for a
// free-threaded pool: enough that the compositor can be filling one while this
// code reads the other.
constexpr int kFramePoolBuffers = 2;

template <typename T>
HRESULT activationFactory(const wchar_t* runtimeClass, T** out) {
    HSTRING_HEADER header;
    HSTRING name = nullptr;
    const HRESULT hr = WindowsCreateStringReference(
        runtimeClass, static_cast<UINT32>(wcslen(runtimeClass)), &header, &name);
    if (FAILED(hr)) return hr;
    return RoGetActivationFactory(name, __uuidof(T), reinterpret_cast<void**>(out));
}

void closeAndRelease(IUnknown* object) {
    if (!object) return;
    wf::IClosable* closable = nullptr;
    if (SUCCEEDED(object->QueryInterface(__uuidof(wf::IClosable),
                                         reinterpret_cast<void**>(&closable)))
        && closable) {
        closable->Close();
        closable->Release();
    }
}

QString hresultText(HRESULT hr) {
    return QStringLiteral("0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

}  // namespace

// ---------------------------------------------------------------------------

struct WgcCapture::Impl {
    // What the session is doing. Resize is a state and not an error: the
    // captured content changing size is ordinary, and treating it as a failure
    // to be recovered from is how a resize turns into a dropped session.
    //
    //   Running -> content size changed -> RecreatingPool -> Running
    //
    // The transition rebuilds the frame pool and the staging texture and
    // touches nothing else. Recording time, source statistics and the sink's
    // cadence all survive it, because none of them have anything to do with
    // how large the captured surface happens to be.
    enum class State { Idle, Running, RecreatingPool, Stopped };

    // What is being captured. Held as handles so the header stays free of
    // windows.h.
    HMONITOR monitor = nullptr;
    HWND     window  = nullptr;

    ID3D11Device*        device  = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D*     staging = nullptr;
    int                  stagingWidth  = 0;
    int                  stagingHeight = 0;

    wgd3d::IDirect3DDevice*             runtimeDevice = nullptr;
    wgc::IGraphicsCaptureItem*          item          = nullptr;
    wgc::IDirect3D11CaptureFramePool*   pool          = nullptr;
    wgc::IGraphicsCaptureSession*       session       = nullptr;

    EventRegistrationToken frameToken  = {};
    EventRegistrationToken closedToken = {};

    wg::SizeInt32 poolSize = {};

    FrameCallback onFrame;
    ErrorHandler  onError;
    ClosedHandler onClosed;

    std::atomic<State> state{State::Idle};

    // Sinks retain the gate until their final Invoke has returned. The gate
    // drains callbacks before resource teardown without retaining Impl itself.
    std::shared_ptr<CaptureCallbackGate<Impl>> callbacks;

    std::shared_ptr<std::atomic<int>> inFlight =
        std::make_shared<std::atomic<int>>(0);

    std::atomic<int> framesProduced{0};
    std::atomic<int> framesDropped{0};
    std::atomic<int> poolRecreations{0};

    // Whether the backend's own clock has been checked against this
    // application's. Checked once per session rather than per frame: the two
    // either share a base or they do not, and asking every frame would be
    // measuring the same fact sixty times a second.
    std::atomic<bool> delivering{true};
    std::atomic<bool> clockChecked{false};
    std::atomic<bool> clockUsable{true};

    QString lastError;
    mutable std::mutex errorMutex;

    bool  createDevice();
    bool  createItem();
    bool  createPoolAndSession();
    void  onFrameArrived(wgc::IDirect3D11CaptureFramePool* sender);
    bool  ensureStaging(int width, int height);
    QImage readBack(ID3D11Texture2D* texture, int width, int height);
    void  reportError(const QString& message);
    void  setLastError(const QString& message);
    void  releaseAll();
};

// ---------------------------------------------------------------------------
// The two event sinks. Both are agile so the free threaded pool can call them
// on whatever thread it likes, and both do nothing but forward: the decisions
// live in Impl, where they can be read in one place.
// ---------------------------------------------------------------------------

namespace {

class FrameArrivedSink final : public FrameArrivedHandler {
public:
    explicit FrameArrivedSink(std::shared_ptr<CaptureCallbackGate<WgcCapture::Impl>> gate)
        : m_gate(std::move(gate)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAgileObject) ||
            riid == __uuidof(FrameArrivedHandler)) {
            *out = static_cast<FrameArrivedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Invoke(wgc::IDirect3D11CaptureFramePool* sender,
                                     IInspectable*) override {
        m_gate->invoke([sender](WgcCapture::Impl& impl) { impl.onFrameArrived(sender); });
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refs{1};
    std::shared_ptr<CaptureCallbackGate<WgcCapture::Impl>> m_gate;
};

class ItemClosedSink final : public ItemClosedHandler {
public:
    explicit ItemClosedSink(std::shared_ptr<CaptureCallbackGate<WgcCapture::Impl>> gate)
        : m_gate(std::move(gate)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAgileObject) ||
            riid == __uuidof(ItemClosedHandler)) {
            *out = static_cast<ItemClosedHandler*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Invoke(wgc::IGraphicsCaptureItem*, IInspectable*) override {
        // The window closed or the monitor went away. Nothing failed, so this
        // is not an error path: the caller is told the source is gone and
        // decides what that means.
        m_gate->invoke([](WgcCapture::Impl& impl) {
            if (impl.state.load(std::memory_order_acquire) == WgcCapture::Impl::State::Running
                && impl.onClosed) {
                impl.onClosed();
            }
        });
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refs{1};
    std::shared_ptr<CaptureCallbackGate<WgcCapture::Impl>> m_gate;
};

}  // namespace

// ---------------------------------------------------------------------------

bool WgcCapture::isAvailable() {
    // Cached: this asks the system a question whose answer cannot change while
    // the process runs, and it is on the path of every capture start.
    static const bool available = [] {
        // A thread that has no apartment cannot activate anything. Whichever
        // mode this thread already has is left alone: Qt initialises the GUI
        // thread as a single threaded apartment, and activation works there.
        RoInitialize(RO_INIT_MULTITHREADED);

        wgc::IGraphicsCaptureSessionStatics* statics = nullptr;
        if (FAILED(activationFactory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession,
                                     &statics))
            || !statics) {
            return false;
        }
        boolean supported = false;
        const HRESULT hr = statics->IsSupported(&supported);
        statics->Release();
        if (FAILED(hr) || !supported) return false;

        // The free threaded frame pool is what lets arrival be announced
        // without a dispatcher queue on the calling thread. Without it this
        // backend would need a message loop of its own, so its absence counts
        // as unavailable rather than as a slower path.
        wgc::IDirect3D11CaptureFramePoolStatics2* poolStatics = nullptr;
        if (FAILED(activationFactory(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool,
                                     &poolStatics))
            || !poolStatics) {
            return false;
        }
        poolStatics->Release();
        return true;
    }();
    return available;
}

void* WgcCapture::monitorHandleFor(int adapterIndex, int outputIndex) {
    if (adapterIndex < 0 || outputIndex < 0) return nullptr;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(&factory)))) {
        return nullptr;
    }

    HMONITOR handle = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    if (SUCCEEDED(factory->EnumAdapters1(static_cast<UINT>(adapterIndex), &adapter))
        && adapter) {
        IDXGIOutput* output = nullptr;
        if (SUCCEEDED(adapter->EnumOutputs(static_cast<UINT>(outputIndex), &output))
            && output) {
            DXGI_OUTPUT_DESC desc = {};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop)
                handle = desc.Monitor;
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return handle;
}

bool WgcCapture::normaliseBackendTime(qint64 systemRelativeTime100ns,
                                      std::chrono::steady_clock::time_point now,
                                      std::chrono::steady_clock::time_point* out) {
    using namespace std::chrono;

    if (!out) return false;

    // Both counters run from boot off the performance counter, so this is a
    // change of unit and not an estimate.
    const auto candidate = steady_clock::time_point(
        duration_cast<steady_clock::duration>(nanoseconds(systemRelativeTime100ns * 100)));

    // A stamp that lands far from now means the premise does not hold on this
    // machine, and a confidently wrong timestamp is worse than an honest
    // approximate one: it would put frames in the wrong order and make a
    // duration that cannot be checked. Five seconds is far wider than any
    // plausible capture latency and far narrower than a different clock base.
    const auto skew = candidate > now ? candidate - now : now - candidate;
    if (systemRelativeTime100ns <= 0 || skew > seconds(5)) {
        *out = now;
        return false;
    }
    *out = candidate;
    return true;
}

// ---------------------------------------------------------------------------

WgcCapture::WgcCapture(void* monitorHandle) : m_impl(std::make_unique<Impl>()) {
    m_impl->monitor = static_cast<HMONITOR>(monitorHandle);
}

WgcCapture::WgcCapture(quintptr hwnd) : m_impl(std::make_unique<Impl>()) {
    m_impl->window = reinterpret_cast<HWND>(hwnd);
}

WgcCapture::~WgcCapture() {
    stop();
}

void WgcCapture::setDelivering(bool delivering) {
    m_impl->delivering.store(delivering, std::memory_order_relaxed);
}

void WgcCapture::setErrorHandler(ErrorHandler handler) {
    m_impl->onError = std::move(handler);
}

void WgcCapture::setClosedHandler(ClosedHandler handler) {
    m_impl->onClosed = std::move(handler);
}

CaptureStats WgcCapture::stats() const {
    CaptureStats out;
    out.framesProduced = m_impl->framesProduced.load(std::memory_order_relaxed);
    out.framesDropped  = m_impl->framesDropped.load(std::memory_order_relaxed);
    return out;
}

int WgcCapture::poolRecreations() const {
    return m_impl->poolRecreations.load(std::memory_order_relaxed);
}

QString WgcCapture::lastError() const {
    std::lock_guard<std::mutex> guard(m_impl->errorMutex);
    return m_impl->lastError;
}

bool WgcCapture::start(FrameCallback onFrame) {
    const auto state = m_impl->state.load();
    if (state == Impl::State::Running || state == Impl::State::RecreatingPool) return true;
    // A device-loss error can leave a stopped session with resources to retire.
    stop();
    if (!isAvailable()) {
        m_impl->setLastError(QStringLiteral("Windows.Graphics.Capture is not available on this system"));
        return false;
    }

    m_impl->onFrame = std::move(onFrame);
    m_impl->setLastError({});
    m_impl->callbacks = std::make_shared<CaptureCallbackGate<Impl>>(m_impl.get());

    if (!m_impl->createDevice() || !m_impl->createItem() || !m_impl->createPoolAndSession()) {
        m_impl->releaseAll();
        return false;
    }

    m_impl->state.store(Impl::State::Running);

    const HRESULT hr = m_impl->session->StartCapture();
    if (FAILED(hr)) {
        m_impl->state.store(Impl::State::Idle);
        m_impl->setLastError(QStringLiteral("StartCapture failed: ") + hresultText(hr));
        m_impl->releaseAll();
        return false;
    }
    return true;
}

void WgcCapture::stop() {
    // Revoking a WinRT event does not drain handlers already dispatched.
    // releaseAll closes the shared gate before detaching either event, so a
    // handler delayed before entering it cannot touch Impl after teardown.
    m_impl->releaseAll();
    m_impl->state.store(Impl::State::Idle);
}

// ---------------------------------------------------------------------------

bool WgcCapture::Impl::createDevice() {
    D3D_FEATURE_LEVEL level = {};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr)) {
        // A machine without a usable hardware device still has WARP, and a
        // software capture is worth more than a failed recording.
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &device, &level, &context);
    }
    if (FAILED(hr)) {
        setLastError(QStringLiteral("D3D11CreateDevice failed: ") + hresultText(hr));
        return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    hr = device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr) || !dxgiDevice) {
        setLastError(QStringLiteral("No IDXGIDevice on the D3D11 device: ") + hresultText(hr));
        return false;
    }

    IInspectable* inspectable = nullptr;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, &inspectable);
    dxgiDevice->Release();
    if (FAILED(hr) || !inspectable) {
        setLastError(QStringLiteral("CreateDirect3D11DeviceFromDXGIDevice failed: ") + hresultText(hr));
        return false;
    }

    hr = inspectable->QueryInterface(__uuidof(wgd3d::IDirect3DDevice),
                                     reinterpret_cast<void**>(&runtimeDevice));
    inspectable->Release();
    if (FAILED(hr) || !runtimeDevice) {
        setLastError(QStringLiteral("No IDirect3DDevice on the interop device: ") + hresultText(hr));
        return false;
    }
    return true;
}

bool WgcCapture::Impl::createItem() {
    IGraphicsCaptureItemInterop* interop = nullptr;
    HRESULT hr = activationFactory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem,
                                   &interop);
    if (FAILED(hr) || !interop) {
        setLastError(QStringLiteral("GraphicsCaptureItem factory failed: ") + hresultText(hr));
        return false;
    }

    if (monitor) {
        hr = interop->CreateForMonitor(monitor, __uuidof(wgc::IGraphicsCaptureItem),
                                       reinterpret_cast<void**>(&item));
    } else if (window) {
        hr = interop->CreateForWindow(window, __uuidof(wgc::IGraphicsCaptureItem),
                                      reinterpret_cast<void**>(&item));
    } else {
        interop->Release();
        setLastError(QStringLiteral("No capture target"));
        return false;
    }
    interop->Release();

    if (FAILED(hr) || !item) {
        setLastError((monitor ? QStringLiteral("CreateForMonitor failed: ")
                             : QStringLiteral("CreateForWindow failed: "))
                    + hresultText(hr));
        return false;
    }

    if (FAILED(item->get_Size(&poolSize)) || poolSize.Width <= 0 || poolSize.Height <= 0) {
        setLastError(QStringLiteral("Capture item reported no size"));
        return false;
    }

    // The item owns the subscription reference. An already dispatched event
    // can retain the sink and its closed gate after the subscription is gone.
    auto* sink = new ItemClosedSink(callbacks);
    hr = item->add_Closed(sink, &closedToken);
    sink->Release();
    if (FAILED(hr)) {
        setLastError(QStringLiteral("add_Closed failed: ") + hresultText(hr));
        return false;
    }
    return true;
}

bool WgcCapture::Impl::createPoolAndSession() {
    wgc::IDirect3D11CaptureFramePoolStatics2* statics = nullptr;
    HRESULT hr = activationFactory(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool,
                                   &statics);
    if (FAILED(hr) || !statics) {
        setLastError(QStringLiteral("Frame pool factory failed: ") + hresultText(hr));
        return false;
    }

    // Free threaded: FrameArrived is delivered on a pool thread rather than
    // needing a dispatcher queue on whichever thread happened to start the
    // capture. That is what makes arrival announced rather than polled without
    // dragging a message loop into the capture layer.
    hr = statics->CreateFreeThreaded(runtimeDevice,
                                     wgdx::DirectXPixelFormat_B8G8R8A8UIntNormalized,
                                     kFramePoolBuffers, poolSize, &pool);
    statics->Release();
    if (FAILED(hr) || !pool) {
        setLastError(QStringLiteral("CreateFreeThreaded failed: ") + hresultText(hr));
        return false;
    }

    auto* sink = new FrameArrivedSink(callbacks);
    hr = pool->add_FrameArrived(sink, &frameToken);
    sink->Release();            // the pool holds it for as long as it is subscribed
    if (FAILED(hr)) {
        setLastError(QStringLiteral("add_FrameArrived failed: ") + hresultText(hr));
        return false;
    }

    hr = pool->CreateCaptureSession(item, &session);
    if (FAILED(hr) || !session) {
        setLastError(QStringLiteral("CreateCaptureSession failed: ") + hresultText(hr));
        return false;
    }

    // The DXGI backend does not composite the cursor into its frames, so this
    // one must not either. Otherwise a comparison between the two would be
    // comparing pictures with different content in them and calling the
    // difference a backend difference. Both of these are optional across
    // Windows versions, so neither failure is fatal.
    wgc::IGraphicsCaptureSession2* cursorControl = nullptr;
    if (SUCCEEDED(session->QueryInterface(__uuidof(wgc::IGraphicsCaptureSession2),
                                          reinterpret_cast<void**>(&cursorControl)))
        && cursorControl) {
        cursorControl->put_IsCursorCaptureEnabled(false);
        cursorControl->Release();
    }
    wgc::IGraphicsCaptureSession3* borderControl = nullptr;
    if (SUCCEEDED(session->QueryInterface(__uuidof(wgc::IGraphicsCaptureSession3),
                                          reinterpret_cast<void**>(&borderControl)))
        && borderControl) {
        borderControl->put_IsBorderRequired(false);
        borderControl->Release();
    }
    return true;
}

bool WgcCapture::Impl::ensureStaging(int width, int height) {
    if (staging && stagingWidth == width && stagingHeight == height) return true;

    if (staging) {
        staging->Release();
        staging = nullptr;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = static_cast<UINT>(width);
    desc.Height           = static_cast<UINT>(height);
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)) || !staging) {
        staging = nullptr;
        return false;
    }
    stagingWidth  = width;
    stagingHeight = height;
    return true;
}

QImage WgcCapture::Impl::readBack(ID3D11Texture2D* texture, int width, int height) {
    // The GPU copy, the map and the memcpy, timed together: they are one
    // decision, since a backend that reads back at all pays for all three.
    FrameProfile::Scoped timing(FrameProfile::Stage::CaptureReadback);
    if (!ensureStaging(width, height)) return {};

    context->CopyResource(staging, texture);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) return {};

    // BGRA to QImage::Format_ARGB32 is a straight byte for byte match on
    // little endian, the same as the DXGI path: this backend differs in how it
    // gets a frame and in nothing else.
    QImage frame(width, height, QImage::Format_ARGB32);
    const auto* source = reinterpret_cast<const quint8*>(mapped.pData);
    for (int row = 0; row < height; ++row) {
        memcpy(frame.scanLine(row),
               source + static_cast<size_t>(row) * mapped.RowPitch,
               static_cast<size_t>(width) * 4);
    }
    context->Unmap(staging, 0);
    return frame;
}

void WgcCapture::Impl::onFrameArrived(wgc::IDirect3D11CaptureFramePool* sender) {
    // The sink's shared gate serializes this whole body with item closure
    // and keeps teardown outside it. Setup and stopped sessions do no work.
    if (state.load(std::memory_order_acquire) != State::Running) return;
    if (!sender) return;

    wgc::IDirect3D11CaptureFrame* frame = nullptr;
    if (FAILED(sender->TryGetNextFrame(&frame)) || !frame) return;

    // Nothing downstream wants frames. The frame is still taken and returned,
    // because the pool starves if its surfaces are not given back, but no
    // readback happens and nothing is counted: a frame nobody asked for was
    // not produced and not lost.
    if (!delivering.load(std::memory_order_relaxed)) {
        closeAndRelease(frame);
        frame->Release();
        return;
    }

    const int sequence = framesProduced.fetch_add(1, std::memory_order_relaxed) + 1;

    wg::SizeInt32 contentSize = {};
    frame->get_ContentSize(&contentSize);

    // Refuse the work before doing it, not after. A consumer that is already
    // at its limit gains nothing from a readback that will be thrown away, and
    // paying for one would move the cost of a backlog into the capture
    // callback, which is the thing this design is trying not to do.
    if (inFlight->load(std::memory_order_acquire) >= kMaxInFlightFrames) {
        framesDropped.fetch_add(1, std::memory_order_relaxed);
        closeAndRelease(frame);
        frame->Release();
        return;
    }

    wf::TimeSpan backendTime = {};
    frame->get_SystemRelativeTime(&backendTime);

    QImage image;
    wgd3d::IDirect3DSurface* surface = nullptr;
    if (SUCCEEDED(frame->get_Surface(&surface)) && surface) {
        IDirect3DDxgiInterfaceAccess* access = nullptr;
        if (SUCCEEDED(surface->QueryInterface(kIID_IDirect3DDxgiInterfaceAccess,
                                              reinterpret_cast<void**>(&access)))
            && access) {
            ID3D11Texture2D* texture = nullptr;
            if (SUCCEEDED(access->GetInterface(__uuidof(ID3D11Texture2D),
                                               reinterpret_cast<void**>(&texture)))
                && texture) {
                D3D11_TEXTURE2D_DESC desc = {};
                texture->GetDesc(&desc);
                image = readBack(texture, static_cast<int>(desc.Width),
                                 static_cast<int>(desc.Height));
                texture->Release();
            }
            access->Release();
        }
        surface->Release();
    }

    // Returning the frame to the pool is what keeps the pool from starving,
    // so it happens before anything is handed onward.
    closeAndRelease(frame);
    frame->Release();

    if (image.isNull()) {
        // A removed device fails the readback rather than announcing itself,
        // so this is where device loss is noticed.
        const HRESULT removed = device ? device->GetDeviceRemovedReason() : S_OK;
        if (removed != S_OK) {
            state.store(State::Stopped, std::memory_order_release);
            reportError(QStringLiteral("Capture device lost: ") + hresultText(removed));
        }
        return;
    }

    std::chrono::steady_clock::time_point capturedAt;
    const bool backendClockUsed = normaliseBackendTime(
        static_cast<qint64>(backendTime.Duration), std::chrono::steady_clock::now(), &capturedAt);
    if (!clockChecked.exchange(true, std::memory_order_relaxed)) {
        clockUsable.store(backendClockUsed, std::memory_order_relaxed);
        if (!backendClockUsed) {
            qWarning("WGC: backend frame times do not share this machine's clock base; "
                     "using arrival time instead");
        }
    }

    CapturedFrame captured;
    captured.image          = std::move(image);
    captured.sourceSequence = static_cast<quint64>(sequence);
    captured.capturedAt     = capturedAt;

    // Claim the slot and give the frame the release token, so the slot is held
    // for exactly as long as the frame object exists no matter which path it
    // takes or fails to take.
    inFlight->fetch_add(1, std::memory_order_release);
    captured.inFlightSlot = std::shared_ptr<void>(
        nullptr, [counter = inFlight](void*) {
            counter->fetch_sub(1, std::memory_order_release);
        });

    if (onFrame) onFrame(std::move(captured));

    // The pool hands out surfaces of the size it was created with, so a
    // content size change is only really adopted once the pool is rebuilt.
    // Doing it after the frame has been delivered means a resize costs no
    // frame, and doing it here rather than in an error path is what makes it
    // a state transition rather than a recovery.
    if (contentSize.Width > 0 && contentSize.Height > 0
        && (contentSize.Width != poolSize.Width || contentSize.Height != poolSize.Height)) {
        state.store(State::RecreatingPool, std::memory_order_release);
        const HRESULT hr = sender->Recreate(runtimeDevice,
                                            wgdx::DirectXPixelFormat_B8G8R8A8UIntNormalized,
                                            kFramePoolBuffers, contentSize);
        if (SUCCEEDED(hr)) {
            poolSize = contentSize;
            poolRecreations.fetch_add(1, std::memory_order_relaxed);
            state.store(State::Running, std::memory_order_release);
        } else {
            state.store(State::Stopped, std::memory_order_release);
            reportError(QStringLiteral("Frame pool could not be resized: ") + hresultText(hr));
        }
    }
}

void WgcCapture::Impl::reportError(const QString& message) {
    setLastError(message);
    if (onError) onError(message);
}

void WgcCapture::Impl::setLastError(const QString& message) {
    std::lock_guard<std::mutex> guard(errorMutex);
    lastError = message;
}

void WgcCapture::Impl::releaseAll() {
    if (callbacks) callbacks->close();
    state.store(State::Stopped, std::memory_order_release);
    onFrame = nullptr;
    if (pool && frameToken.value != 0) {
        pool->remove_FrameArrived(frameToken);
        frameToken = {};
    }
    if (item && closedToken.value != 0) {
        item->remove_Closed(closedToken);
        closedToken = {};
    }
    if (session) {
        closeAndRelease(session);
        session->Release();
        session = nullptr;
    }
    if (pool) {
        closeAndRelease(pool);
        pool->Release();
        pool = nullptr;
    }
    if (item) {
        item->Release();
        item = nullptr;
    }
    if (runtimeDevice) {
        runtimeDevice->Release();
        runtimeDevice = nullptr;
    }
    if (staging) {
        staging->Release();
        staging = nullptr;
        stagingWidth = stagingHeight = 0;
    }
    if (context) {
        context->Release();
        context = nullptr;
    }
    if (device) {
        device->Release();
        device = nullptr;
    }
}
