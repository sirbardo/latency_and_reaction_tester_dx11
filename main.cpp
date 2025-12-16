// DX11 Full Screen Exclusive Latency Tester
// Minimal computation path for accurate input-to-photon latency measurement

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwrite.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <chrono>
#include <hidusage.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::high_resolution_clock;

// Forward declarations
void ToggleFullscreen();

// Configuration
constexpr bool VSYNC_ENABLED = false;  // Disable for lowest latency
constexpr size_t MAX_LOG_ENTRIES = 30; // Max log entries to display

// Global state
struct AppState
{
    // DX11 resources
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11RenderTargetView> rtv;

    // D2D/DWrite for text overlay
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1RenderTarget> d2dRT;
    ComPtr<IDWriteFactory> dwriteFactory;
    ComPtr<IDWriteTextFormat> textFormat;
    ComPtr<IDWriteTextFormat> textFormatRight; // Right-aligned for FPS
    ComPtr<ID2D1SolidColorBrush> textBrush;

    // Flash state
    bool isFlashing = false;
    Clock::time_point flashStartTime;
    float flashDurationMs = 50.0f; // Adjustable with F5/F6

    // Timing for log timestamps
    Clock::time_point appStartTime = Clock::now();
    double lastEventTimeMs = 0.0;

    // Last input info for display
    std::wstring lastInputText = L"Waiting for input...";
    std::wstring lastDeviceText = L"";

    // Frame timing
    Clock::time_point lastFrameTime = Clock::now();
    float frameTimeMs = 0.0f;
    float fps = 0.0f;
    // Smoothing for display (optional, keeps numbers readable)
    float smoothedFrameTimeMs = 0.0f;
    float smoothedFps = 0.0f;

    // Input toggles (F1-F4, F7-F8)
    bool enableMouseButtons = true; // F1 toggles
    bool enableKeyboard = true;     // F2 toggles
    bool enableMouseDelta = true;   // F3 toggles
    bool enableLog = false;         // F4 toggles
    bool enableUpEvents = true;     // F7 toggles (when OFF, only DOWN events register)
    bool enableMouseHz = false;     // F8 toggles mouse polling rate display
    bool enableOverlay = true;      // F9 toggles text overlay (disable for minimal latency)
    bool isFullscreen = true;       // F10 toggles FSE/Windowed

    // Mouse Hz tracking
    std::vector<Clock::time_point> mouseDeltaTimes;
    float mouseHz = 0.0f;

    // Log history (newest first)
    std::vector<std::wstring> logEntries;

    // Window
    HWND hwnd = nullptr;
    int width = 1920;
    int height = 1080;
    bool running = true;
} g_app;

void TriggerFlash(const std::wstring &inputInfo, const std::wstring &deviceInfo)
{
    auto now = Clock::now();
    g_app.isFlashing = true;
    g_app.flashStartTime = now;
    g_app.lastInputText = inputInfo;
    g_app.lastDeviceText = deviceInfo;

    // Add to log (newest first) with timestamp and delta
    if (g_app.enableLog)
    {
        double currentTimeMs = std::chrono::duration<double, std::milli>(now - g_app.appStartTime).count();
        double deltaMs = currentTimeMs - g_app.lastEventTimeMs;

        // Format: "123.45ms +12.34Δ | InputInfo | Device"
        wchar_t timeStr[64];
        swprintf_s(timeStr, L"%.2fms %+.2f\u0394", currentTimeMs, deltaMs);

        std::wstring logEntry = std::wstring(timeStr) + L" | " + inputInfo + L" | " + deviceInfo;
        g_app.logEntries.insert(g_app.logEntries.begin(), logEntry);
        if (g_app.logEntries.size() > MAX_LOG_ENTRIES)
        {
            g_app.logEntries.pop_back();
        }

        g_app.lastEventTimeMs = currentTimeMs;
    }
}

void ProcessRawInput(LPARAM lParam)
{
    UINT size = 0;
    GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));

    if (size == 0)
        return;

    std::vector<BYTE> buffer(size);
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size)
        return;

    RAWINPUT *raw = (RAWINPUT *)buffer.data();

    // Get device name
    UINT deviceNameSize = 0;
    GetRawInputDeviceInfoW(raw->header.hDevice, RIDI_DEVICENAME, nullptr, &deviceNameSize);
    std::wstring deviceName(deviceNameSize, L'\0');
    GetRawInputDeviceInfoW(raw->header.hDevice, RIDI_DEVICENAME, &deviceName[0], &deviceNameSize);

    // Extract just the device part for cleaner display
    size_t lastSlash = deviceName.rfind(L'#');
    if (lastSlash != std::wstring::npos && lastSlash > 0)
    {
        size_t prevSlash = deviceName.rfind(L'#', lastSlash - 1);
        if (prevSlash != std::wstring::npos)
        {
            deviceName = deviceName.substr(prevSlash + 1, lastSlash - prevSlash - 1);
        }
    }

    std::wstring inputInfo;
    std::wstring deviceType;

    if (raw->header.dwType == RIM_TYPEMOUSE)
    {
        deviceType = L"MOUSE";
        RAWMOUSE &mouse = raw->data.mouse;

        bool isButtonEvent = (mouse.usButtonFlags != 0);
        bool isDeltaEvent = (mouse.lLastX != 0 || mouse.lLastY != 0);

        // Track mouse Hz if enabled (track all delta events regardless of filter)
        if (isDeltaEvent && g_app.enableMouseHz)
        {
            g_app.mouseDeltaTimes.push_back(Clock::now());
        }

        // Filter based on toggles
        if (isButtonEvent && !g_app.enableMouseButtons)
            return;
        if (isDeltaEvent && !isButtonEvent && !g_app.enableMouseDelta)
            return;

        // Check what triggered this input
        if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)
            inputInfo = L"Left Click DOWN";
        else if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)
        {
            if (!g_app.enableUpEvents)
                return;
            inputInfo = L"Left Click UP";
        }
        else if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN)
            inputInfo = L"Right Click DOWN";
        else if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)
        {
            if (!g_app.enableUpEvents)
                return;
            inputInfo = L"Right Click UP";
        }
        else if (mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN)
            inputInfo = L"Middle Click DOWN";
        else if (mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP)
        {
            if (!g_app.enableUpEvents)
                return;
            inputInfo = L"Middle Click UP";
        }
        else if (mouse.usButtonFlags & RI_MOUSE_BUTTON_4_DOWN)
            inputInfo = L"Button 4 DOWN";
        else if (mouse.usButtonFlags & RI_MOUSE_BUTTON_4_UP)
        {
            if (!g_app.enableUpEvents)
                return;
            inputInfo = L"Button 4 UP";
        }
        else if (mouse.usButtonFlags & RI_MOUSE_BUTTON_5_DOWN)
            inputInfo = L"Button 5 DOWN";
        else if (mouse.usButtonFlags & RI_MOUSE_BUTTON_5_UP)
        {
            if (!g_app.enableUpEvents)
                return;
            inputInfo = L"Button 5 UP";
        }
        else if (mouse.usButtonFlags & RI_MOUSE_WHEEL)
            inputInfo = L"Wheel: " + std::to_wstring((SHORT)mouse.usButtonData);
        else if (isDeltaEvent)
            inputInfo = L"Move: dX=" + std::to_wstring(mouse.lLastX) + L" dY=" + std::to_wstring(mouse.lLastY);
        else
            return; // No meaningful input
    }
    else if (raw->header.dwType == RIM_TYPEKEYBOARD)
    {
        // Filter keyboard if disabled
        if (!g_app.enableKeyboard)
            return;

        deviceType = L"KEYBOARD";
        RAWKEYBOARD &kb = raw->data.keyboard;

        bool isDown = !(kb.Flags & RI_KEY_BREAK);

        // Filter UP events if disabled
        if (!isDown && !g_app.enableUpEvents)
            return;

        UINT vk = kb.VKey;
        UINT sc = kb.MakeCode;

        // Get key name
        UINT scanCode = sc;
        if (kb.Flags & RI_KEY_E0)
            scanCode |= 0x100;

        wchar_t keyName[64] = {};
        GetKeyNameTextW(scanCode << 16, keyName, 64);

        inputInfo = std::wstring(keyName) + L" (VK=" + std::to_wstring(vk) +
                    L" SC=" + std::to_wstring(sc) + L") " + (isDown ? L"DOWN" : L"UP");
    }
    else
    {
        return; // HID device, ignore for now
    }

    std::wstring deviceInfo = deviceType + L": " + deviceName;
    TriggerFlash(inputInfo, deviceInfo);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INPUT:
        ProcessRawInput(lParam);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            g_app.running = false;
        }
        else if (wParam == VK_F1)
        {
            g_app.enableMouseButtons = !g_app.enableMouseButtons;
        }
        else if (wParam == VK_F2)
        {
            g_app.enableKeyboard = !g_app.enableKeyboard;
        }
        else if (wParam == VK_F3)
        {
            g_app.enableMouseDelta = !g_app.enableMouseDelta;
        }
        else if (wParam == VK_F4)
        {
            g_app.enableLog = !g_app.enableLog;
            if (!g_app.enableLog)
            {
                g_app.logEntries.clear(); // Clear log when disabled
            }
        }
        else if (wParam == VK_F5)
        {
            g_app.flashDurationMs += 10.0f;
        }
        else if (wParam == VK_F6)
        {
            g_app.flashDurationMs = (g_app.flashDurationMs > 10.0f) ? g_app.flashDurationMs - 10.0f : 10.0f;
        }
        else if (wParam == VK_F7)
        {
            g_app.enableUpEvents = !g_app.enableUpEvents;
        }
        else if (wParam == VK_F8)
        {
            g_app.enableMouseHz = !g_app.enableMouseHz;
            if (!g_app.enableMouseHz)
            {
                g_app.mouseDeltaTimes.clear();
                g_app.mouseHz = 0.0f;
            }
        }
        else if (wParam == VK_F9)
        {
            g_app.enableOverlay = !g_app.enableOverlay;
        }
        return 0;

    case WM_SYSKEYDOWN:
        // F10 is a system key, so it comes through WM_SYSKEYDOWN
        if (wParam == VK_F10)
        {
            ToggleFullscreen();
            return 0; // Prevent default F10 behavior (menu activation)
        }
        break;

    case WM_DESTROY:
        g_app.running = false;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool InitWindow()
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"LatencyTesterClass";

    if (!RegisterClassExW(&wc))
        return false;

    // Get primary monitor dimensions for true fullscreen
    g_app.width = GetSystemMetrics(SM_CXSCREEN);
    g_app.height = GetSystemMetrics(SM_CYSCREEN);

    // Create borderless window covering full screen
    g_app.hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        wc.lpszClassName,
        L"Latency Tester - Press ESC to exit",
        WS_POPUP,
        0, 0, g_app.width, g_app.height,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!g_app.hwnd)
        return false;

    // Register for raw input
    RAWINPUTDEVICE rid[2] = {};

    // Mouse - no RIDEV_INPUTSINK to avoid background coalescing
    rid[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
    rid[0].usUsage = HID_USAGE_GENERIC_MOUSE;
    rid[0].dwFlags = 0; // Foreground only, no coalescing
    rid[0].hwndTarget = g_app.hwnd;

    // Keyboard - no RIDEV_INPUTSINK to avoid background coalescing
    rid[1].usUsagePage = HID_USAGE_PAGE_GENERIC;
    rid[1].usUsage = HID_USAGE_GENERIC_KEYBOARD;
    rid[1].dwFlags = 0; // Foreground only, no coalescing
    rid[1].hwndTarget = g_app.hwnd;

    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE)))
    {
        return false;
    }

    ShowWindow(g_app.hwnd, SW_SHOW);
    UpdateWindow(g_app.hwnd);

    return true;
}

bool InitD3D11()
{
    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    // Need BGRA support for D2D interop
    createFlags |= D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createFlags,
        featureLevels, 1,
        D3D11_SDK_VERSION,
        &g_app.device,
        &featureLevel,
        &g_app.context);

    if (FAILED(hr))
        return false;

    // Get DXGI factory
    ComPtr<IDXGIDevice1> dxgiDevice;
    g_app.device.As(&dxgiDevice);

    // Set maximum frame latency to 1 for lowest input lag
    dxgiDevice->SetMaximumFrameLatency(1);

    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);

    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    // Create swap chain - FLIP_DISCARD for FSE
    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width = g_app.width;
    scDesc.Height = g_app.height;
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // BGRA for D2D compatibility
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 2;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    // Fullscreen exclusive for lowest latency
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc = {};
    fsDesc.RefreshRate.Numerator = 0;
    fsDesc.RefreshRate.Denominator = 0;
    fsDesc.Windowed = FALSE; // TRUE FSE!

    hr = factory->CreateSwapChainForHwnd(
        g_app.device.Get(),
        g_app.hwnd,
        &scDesc,
        &fsDesc,
        nullptr,
        &g_app.swapChain);

    if (FAILED(hr))
        return false;

    // Disable Alt+Enter handling by DXGI (we handle fullscreen ourselves)
    factory->MakeWindowAssociation(g_app.hwnd, DXGI_MWA_NO_ALT_ENTER);

    // Create render target view
    ComPtr<ID3D11Texture2D> backBuffer;
    g_app.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    hr = g_app.device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_app.rtv);

    if (FAILED(hr))
        return false;

    return true;
}

bool InitD2D()
{
    // Create D2D factory
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_app.d2dFactory.GetAddressOf());
    if (FAILED(hr))
        return false;

    // Create DWrite factory
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown **>(g_app.dwriteFactory.GetAddressOf()));
    if (FAILED(hr))
        return false;

    // Create text format (left-aligned)
    hr = g_app.dwriteFactory->CreateTextFormat(
        L"Consolas",
        nullptr,
        DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        24.0f,
        L"en-us",
        &g_app.textFormat);
    if (FAILED(hr))
        return false;

    // Create right-aligned text format for FPS counter
    hr = g_app.dwriteFactory->CreateTextFormat(
        L"Consolas",
        nullptr,
        DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        24.0f,
        L"en-us",
        &g_app.textFormatRight);
    if (FAILED(hr))
        return false;
    g_app.textFormatRight->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);

    // Get DXGI surface from swap chain for D2D
    ComPtr<IDXGISurface> surface;
    hr = g_app.swapChain->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (FAILED(hr))
        return false;

    // Create D2D render target from DXGI surface
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    hr = g_app.d2dFactory->CreateDxgiSurfaceRenderTarget(surface.Get(), &props, &g_app.d2dRT);
    if (FAILED(hr))
        return false;

    // Create text brush (green for visibility on both black and white)
    hr = g_app.d2dRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 1.0f, 0.0f, 1.0f), &g_app.textBrush);
    if (FAILED(hr))
        return false;

    return true;
}

void ToggleFullscreen()
{
    // Release D2D render target first (holds reference to swap chain buffer)
    g_app.textBrush.Reset();
    g_app.d2dRT.Reset();

    // Release render target view
    g_app.rtv.Reset();
    g_app.context->ClearState();
    g_app.context->Flush();

    // Toggle fullscreen state
    g_app.isFullscreen = !g_app.isFullscreen;

    HRESULT hr = g_app.swapChain->SetFullscreenState(g_app.isFullscreen ? TRUE : FALSE, nullptr);
    if (FAILED(hr))
    {
        // Revert state on failure
        g_app.isFullscreen = !g_app.isFullscreen;
    }

    // Get new window size after mode switch
    DXGI_SWAP_CHAIN_DESC1 desc;
    g_app.swapChain->GetDesc1(&desc);

    // For windowed mode, use a reasonable window size
    if (!g_app.isFullscreen)
    {
        // Set windowed size and style
        SetWindowLongPtrW(g_app.hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowLongPtrW(g_app.hwnd, GWL_EXSTYLE, 0);

        // Center window at 1280x720
        int winWidth = 1280;
        int winHeight = 720;
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        int x = (screenW - winWidth) / 2;
        int y = (screenH - winHeight) / 2;

        SetWindowPos(g_app.hwnd, HWND_NOTOPMOST, x, y, winWidth, winHeight, SWP_FRAMECHANGED | SWP_SHOWWINDOW);

        RECT clientRect;
        GetClientRect(g_app.hwnd, &clientRect);
        g_app.width = clientRect.right - clientRect.left;
        g_app.height = clientRect.bottom - clientRect.top;
    }
    else
    {
        // Restore fullscreen popup style
        SetWindowLongPtrW(g_app.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongPtrW(g_app.hwnd, GWL_EXSTYLE, WS_EX_TOPMOST);

        g_app.width = GetSystemMetrics(SM_CXSCREEN);
        g_app.height = GetSystemMetrics(SM_CYSCREEN);

        SetWindowPos(g_app.hwnd, HWND_TOPMOST, 0, 0, g_app.width, g_app.height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    // Resize swap chain buffers
    hr = g_app.swapChain->ResizeBuffers(0, g_app.width, g_app.height, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);

    // Recreate render target view
    ComPtr<ID3D11Texture2D> backBuffer;
    g_app.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    g_app.device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_app.rtv);

    // Recreate D2D render target
    ComPtr<IDXGISurface> surface;
    g_app.swapChain->GetBuffer(0, IID_PPV_ARGS(&surface));

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    g_app.d2dFactory->CreateDxgiSurfaceRenderTarget(surface.Get(), &props, &g_app.d2dRT);
    g_app.d2dRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 1.0f, 0.0f, 1.0f), &g_app.textBrush);
}

void Render()
{
    // MINIMAL PATH: When overlay is disabled, skip ALL unnecessary computation for lowest latency
    if (!g_app.enableOverlay)
    {
        // Only check flash state - minimal work
        if (g_app.isFlashing)
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               Clock::now() - g_app.flashStartTime)
                               .count();
            if (elapsed >= g_app.flashDurationMs)
            {
                g_app.isFlashing = false;
            }
        }

        // Direct clear and present - no D2D, no frame timing overhead
        float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        if (g_app.isFlashing)
        {
            clearColor[0] = clearColor[1] = clearColor[2] = 1.0f;
        }
        g_app.context->ClearRenderTargetView(g_app.rtv.Get(), clearColor);

        // Use DO_NOT_WAIT to avoid blocking - spin instead for lower latency
        HRESULT hr = g_app.swapChain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
        // If queue is full (WAS_STILL_DRAWING), that's fine - we'll try again next iteration
        (void)hr;
        return;
    }

    // FULL PATH: With overlay enabled, do all the work
    auto now = Clock::now();
    g_app.frameTimeMs = std::chrono::duration<float, std::milli>(now - g_app.lastFrameTime).count();
    g_app.lastFrameTime = now;
    g_app.fps = (g_app.frameTimeMs > 0.0f) ? 1000.0f / g_app.frameTimeMs : 0.0f;

    // Smooth the values for readability (exponential moving average)
    constexpr float smoothing = 0.9f;
    g_app.smoothedFrameTimeMs = g_app.smoothedFrameTimeMs * smoothing + g_app.frameTimeMs * (1.0f - smoothing);
    g_app.smoothedFps = g_app.smoothedFps * smoothing + g_app.fps * (1.0f - smoothing);

    // Calculate mouse Hz (events in last 1 second)
    if (g_app.enableMouseHz)
    {
        auto oneSecondAgo = now - std::chrono::seconds(1);
        // Remove events older than 1 second
        while (!g_app.mouseDeltaTimes.empty() && g_app.mouseDeltaTimes.front() < oneSecondAgo)
        {
            g_app.mouseDeltaTimes.erase(g_app.mouseDeltaTimes.begin());
        }
        g_app.mouseHz = static_cast<float>(g_app.mouseDeltaTimes.size());
    }

    // Check if flash should end
    if (g_app.isFlashing)
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           Clock::now() - g_app.flashStartTime)
                           .count();

        if (elapsed >= g_app.flashDurationMs)
        {
            g_app.isFlashing = false;
        }
    }

    // Clear to white if flashing, black otherwise
    float clearColor[4];
    if (g_app.isFlashing)
    {
        clearColor[0] = clearColor[1] = clearColor[2] = 1.0f;
        clearColor[3] = 1.0f;
    }
    else
    {
        clearColor[0] = clearColor[1] = clearColor[2] = 0.0f;
        clearColor[3] = 1.0f;
    }

    g_app.context->ClearRenderTargetView(g_app.rtv.Get(), clearColor);

    // Draw text overlay with D2D (skip if overlay disabled for minimal latency)
    if (g_app.enableOverlay)
    {
        g_app.d2dRT->BeginDraw();

        // Draw input info in top-left corner
        D2D1_RECT_F textRect = D2D1::RectF(20.0f, 20.0f, (float)g_app.width - 20.0f, 100.0f);
        g_app.d2dRT->DrawText(
            g_app.lastInputText.c_str(),
            (UINT32)g_app.lastInputText.length(),
            g_app.textFormat.Get(),
            textRect,
            g_app.textBrush.Get());

        // Draw device info below
        textRect.top = 50.0f;
        textRect.bottom = 130.0f;
        g_app.d2dRT->DrawText(
            g_app.lastDeviceText.c_str(),
            (UINT32)g_app.lastDeviceText.length(),
            g_app.textFormat.Get(),
            textRect,
            g_app.textBrush.Get());

        // Draw FPS counter in top-right corner (and mouse Hz if enabled)
        wchar_t fpsBuffer[128];
        if (g_app.enableMouseHz)
        {
            swprintf_s(fpsBuffer, L"%.1f FPS\n%.2f ms\n%.0f Hz", g_app.smoothedFps, g_app.smoothedFrameTimeMs, g_app.mouseHz);
        }
        else
        {
            swprintf_s(fpsBuffer, L"%.1f FPS\n%.2f ms", g_app.smoothedFps, g_app.smoothedFrameTimeMs);
        }
        D2D1_RECT_F fpsRect = D2D1::RectF((float)g_app.width - 200.0f, 20.0f, (float)g_app.width - 20.0f, 110.0f);
        g_app.d2dRT->DrawText(
            fpsBuffer,
            (UINT32)wcslen(fpsBuffer),
            g_app.textFormatRight.Get(),
            fpsRect,
            g_app.textBrush.Get());

        // Draw log if enabled (left side, below device info)
        if (g_app.enableLog && !g_app.logEntries.empty())
        {
            float logY = 100.0f;
            for (size_t i = 0; i < g_app.logEntries.size() && logY < g_app.height - 80.0f; ++i)
            {
                D2D1_RECT_F logRect = D2D1::RectF(20.0f, logY, (float)g_app.width / 2.0f, logY + 24.0f);
                g_app.d2dRT->DrawText(
                    g_app.logEntries[i].c_str(),
                    (UINT32)g_app.logEntries[i].length(),
                    g_app.textFormat.Get(),
                    logRect,
                    g_app.textBrush.Get());
                logY += 26.0f;
            }
        }

        // Draw instructions at bottom with toggle states
        std::wstring instructions = L"ESC | F1=Mouse[" + std::wstring(g_app.enableMouseButtons ? L"+" : L"-") +
                                    L"] F2=KB[" + std::wstring(g_app.enableKeyboard ? L"+" : L"-") +
                                    L"] F3=Dlt[" + std::wstring(g_app.enableMouseDelta ? L"+" : L"-") +
                                    L"] F4=Log[" + std::wstring(g_app.enableLog ? L"+" : L"-") +
                                    L"] F7=Up[" + std::wstring(g_app.enableUpEvents ? L"+" : L"-") +
                                    L"] F8=Hz[" + std::wstring(g_app.enableMouseHz ? L"+" : L"-") +
                                    L"] F9=OL[" + std::wstring(g_app.enableOverlay ? L"+" : L"-") +
                                    L"] F10=[" + std::wstring(g_app.isFullscreen ? L"FSE" : L"WIN") +
                                    L"] F5/6=" + std::to_wstring((int)g_app.flashDurationMs) + L"ms";
        textRect.top = (float)g_app.height - 50.0f;
        textRect.bottom = (float)g_app.height - 10.0f;
        g_app.d2dRT->DrawText(
            instructions.c_str(),
            (UINT32)instructions.length(),
            g_app.textFormat.Get(),
            textRect,
            g_app.textBrush.Get());

        g_app.d2dRT->EndDraw();
    }

    // Present - use DO_NOT_WAIT to avoid blocking for lower latency
    g_app.swapChain->Present(VSYNC_ENABLED ? 1 : 0, VSYNC_ENABLED ? 0 : DXGI_PRESENT_DO_NOT_WAIT);
}

void Cleanup()
{
    // Exit fullscreen before releasing swap chain
    if (g_app.swapChain)
    {
        g_app.swapChain->SetFullscreenState(FALSE, nullptr);
    }

    // ComPtr handles release automatically
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    if (!InitWindow())
    {
        MessageBoxW(nullptr, L"Failed to create window", L"Error", MB_OK);
        return 1;
    }

    if (!InitD3D11())
    {
        MessageBoxW(nullptr, L"Failed to initialize Direct3D 11", L"Error", MB_OK);
        return 1;
    }

    if (!InitD2D())
    {
        MessageBoxW(nullptr, L"Failed to initialize Direct2D", L"Error", MB_OK);
        return 1;
    }

    // Main loop - minimal overhead
    MSG msg = {};
    while (g_app.running)
    {
        // Process all pending messages immediately (non-blocking)
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                g_app.running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        Render();
    }

    Cleanup();
    return 0;
}
