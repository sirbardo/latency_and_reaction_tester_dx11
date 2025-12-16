// DX11 Visual Reaction Time Tester
// Measures visual reaction time with minimal input-to-photon latency
// Uses WASAPI exclusive mode for low-latency audio

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
#include <random>
#include <hidusage.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::high_resolution_clock;

// Forward declarations
void ToggleFullscreen();
void StartNewRound();

// Configuration
constexpr float MIN_DELAY_MS = 1500.0f;  // Minimum wait before flash
constexpr float MAX_DELAY_MS = 5000.0f;  // Maximum wait before flash
constexpr size_t MAX_LOG_ENTRIES = 25;   // Max reaction times to display

// Audio configuration
constexpr float TONE_FREQ_HZ = 800.0f;   // Beep frequency
constexpr float TONE_DURATION_MS = 80.0f; // Beep duration
constexpr UINT32 AUDIO_SAMPLE_RATE = 48000;
constexpr UINT32 AUDIO_CHANNELS = 2;
constexpr UINT32 AUDIO_BITS = 16;

// Test state
enum class TestState
{
    Waiting,      // Black screen, waiting for random delay
    Flashing,     // White screen, waiting for click
    TooEarly      // Clicked before flash (false start)
};

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
    ComPtr<IDWriteTextFormat> textFormatLarge;
    ComPtr<ID2D1SolidColorBrush> textBrush;
    ComPtr<ID2D1SolidColorBrush> redBrush;

    // Test state
    TestState state = TestState::Waiting;
    Clock::time_point roundStartTime;
    Clock::time_point flashStartTime;
    float targetDelayMs = 0.0f;

    // Results
    std::vector<float> reactionTimes;
    float lastReactionTime = 0.0f;
    float averageTime = 0.0f;
    float bestTime = 0.0f;

    // Random number generator
    std::mt19937 rng{std::random_device{}()};

    // Window
    HWND hwnd = nullptr;
    int width = 1920;
    int height = 1080;
    bool running = true;
    bool isFullscreen = true;

    // Mode
    bool audioMode = false;        // F1 toggles: false=visual, true=audio
    bool beepPlayed = false;       // Track if beep was played this round

    // WASAPI audio (low-latency)
    IMMDeviceEnumerator* audioEnumerator = nullptr;
    IMMDevice* audioDevice = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioRenderClient* audioRenderClient = nullptr;
    WAVEFORMATEX* audioFormat = nullptr;
    UINT32 audioBufferFrames = 0;
    bool audioInitialized = false;
    float audioLatencyMs = 0.0f;   // Reported latency for display
} g_app;

float GetRandomDelay()
{
    std::uniform_real_distribution<float> dist(MIN_DELAY_MS, MAX_DELAY_MS);
    return dist(g_app.rng);
}

void StartNewRound()
{
    g_app.state = TestState::Waiting;
    g_app.roundStartTime = Clock::now();
    g_app.targetDelayMs = GetRandomDelay();
    g_app.beepPlayed = false;
}

void UpdateStats()
{
    if (g_app.reactionTimes.empty()) return;

    float sum = 0.0f;
    float best = g_app.reactionTimes[0];
    for (float t : g_app.reactionTimes)
    {
        sum += t;
        if (t < best) best = t;
    }
    g_app.averageTime = sum / g_app.reactionTimes.size();
    g_app.bestTime = best;
}

// Initialize WASAPI in exclusive mode for lowest latency
bool InitWASAPI()
{
    HRESULT hr;

    // Create device enumerator
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&g_app.audioEnumerator);
    if (FAILED(hr)) return false;

    // Get default audio output device
    hr = g_app.audioEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_app.audioDevice);
    if (FAILED(hr)) return false;

    // Activate audio client
    hr = g_app.audioDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&g_app.audioClient);
    if (FAILED(hr)) return false;

    // Get device's mix format as starting point
    hr = g_app.audioClient->GetMixFormat(&g_app.audioFormat);
    if (FAILED(hr)) return false;

    // Try exclusive mode with smallest buffer
    // Request 3ms buffer (30000 * 100ns = 3ms)
    REFERENCE_TIME requestedDuration = 30000; // 3ms in 100ns units

    // First try exclusive mode
    hr = g_app.audioClient->Initialize(
        AUDCLNT_SHAREMODE_EXCLUSIVE,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        requestedDuration,
        requestedDuration,
        g_app.audioFormat,
        nullptr);

    // If exclusive fails, fall back to shared mode with low latency
    if (FAILED(hr))
    {
        // Release and reactivate for shared mode
        g_app.audioClient->Release();
        g_app.audioDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&g_app.audioClient);

        // Shared mode with auto-convert for compatibility
        DWORD streamFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        requestedDuration = 100000; // 10ms fallback

        hr = g_app.audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            streamFlags,
            requestedDuration,
            0,
            g_app.audioFormat,
            nullptr);

        if (FAILED(hr)) return false;
    }

    // Get buffer size
    hr = g_app.audioClient->GetBufferSize(&g_app.audioBufferFrames);
    if (FAILED(hr)) return false;

    // Calculate latency
    REFERENCE_TIME latency;
    g_app.audioClient->GetStreamLatency(&latency);
    g_app.audioLatencyMs = (float)latency / 10000.0f;

    // Get render client
    hr = g_app.audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&g_app.audioRenderClient);
    if (FAILED(hr)) return false;

    g_app.audioInitialized = true;
    return true;
}

// Play a low-latency beep using WASAPI
void PlayBeepWASAPI()
{
    if (!g_app.audioInitialized) return;

    // Stop any previous playback
    g_app.audioClient->Stop();
    g_app.audioClient->Reset();

    // Calculate samples needed for tone
    UINT32 sampleRate = g_app.audioFormat->nSamplesPerSec;
    UINT32 channels = g_app.audioFormat->nChannels;
    UINT32 toneSamples = (UINT32)(sampleRate * TONE_DURATION_MS / 1000.0f);

    // Clamp to buffer size
    if (toneSamples > g_app.audioBufferFrames)
        toneSamples = g_app.audioBufferFrames;

    // Get buffer
    BYTE* buffer;
    HRESULT hr = g_app.audioRenderClient->GetBuffer(toneSamples, &buffer);
    if (FAILED(hr)) return;

    // Generate sine wave
    float phaseIncrement = 2.0f * 3.14159265f * TONE_FREQ_HZ / sampleRate;
    float phase = 0.0f;

    if (g_app.audioFormat->wBitsPerSample == 32)
    {
        // 32-bit float format (common for WASAPI)
        float* fBuffer = (float*)buffer;
        for (UINT32 i = 0; i < toneSamples; i++)
        {
            float sample = sinf(phase) * 0.5f; // 50% volume
            for (UINT32 ch = 0; ch < channels; ch++)
            {
                *fBuffer++ = sample;
            }
            phase += phaseIncrement;
        }
    }
    else if (g_app.audioFormat->wBitsPerSample == 16)
    {
        // 16-bit PCM
        INT16* iBuffer = (INT16*)buffer;
        for (UINT32 i = 0; i < toneSamples; i++)
        {
            INT16 sample = (INT16)(sinf(phase) * 16000.0f);
            for (UINT32 ch = 0; ch < channels; ch++)
            {
                *iBuffer++ = sample;
            }
            phase += phaseIncrement;
        }
    }

    // Release buffer and start playback immediately
    g_app.audioRenderClient->ReleaseBuffer(toneSamples, 0);
    g_app.audioClient->Start();
}

void CleanupWASAPI()
{
    if (g_app.audioClient)
    {
        g_app.audioClient->Stop();
    }
    if (g_app.audioRenderClient)
    {
        g_app.audioRenderClient->Release();
        g_app.audioRenderClient = nullptr;
    }
    if (g_app.audioFormat)
    {
        CoTaskMemFree(g_app.audioFormat);
        g_app.audioFormat = nullptr;
    }
    if (g_app.audioClient)
    {
        g_app.audioClient->Release();
        g_app.audioClient = nullptr;
    }
    if (g_app.audioDevice)
    {
        g_app.audioDevice->Release();
        g_app.audioDevice = nullptr;
    }
    if (g_app.audioEnumerator)
    {
        g_app.audioEnumerator->Release();
        g_app.audioEnumerator = nullptr;
    }
}

void ProcessRawInput(LPARAM lParam)
{
    UINT size = 0;
    GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
    if (size == 0) return;

    std::vector<BYTE> buffer(size);
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size)
        return;

    RAWINPUT *raw = (RAWINPUT *)buffer.data();

    // Only care about mouse button down events
    if (raw->header.dwType == RIM_TYPEMOUSE)
    {
        RAWMOUSE &mouse = raw->data.mouse;

        // Check for any button down
        bool buttonDown = (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) ||
                         (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) ||
                         (mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN);

        if (!buttonDown) return;

        auto now = Clock::now();

        if (g_app.state == TestState::Waiting)
        {
            // Clicked too early!
            g_app.state = TestState::TooEarly;
        }
        else if (g_app.state == TestState::Flashing)
        {
            // Record reaction time
            float reactionMs = std::chrono::duration<float, std::milli>(now - g_app.flashStartTime).count();
            g_app.lastReactionTime = reactionMs;
            g_app.reactionTimes.insert(g_app.reactionTimes.begin(), reactionMs);
            if (g_app.reactionTimes.size() > MAX_LOG_ENTRIES)
            {
                g_app.reactionTimes.pop_back();
            }
            UpdateStats();
            StartNewRound();
        }
        else if (g_app.state == TestState::TooEarly)
        {
            // Click to restart after false start
            StartNewRound();
        }
    }
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
        else if (wParam == VK_SPACE)
        {
            // Space to restart/clear
            g_app.reactionTimes.clear();
            g_app.averageTime = 0.0f;
            g_app.bestTime = 0.0f;
            g_app.lastReactionTime = 0.0f;
            StartNewRound();
        }
        else if (wParam == VK_F1)
        {
            // F1 to toggle audio/visual mode and clear
            g_app.audioMode = !g_app.audioMode;
            g_app.reactionTimes.clear();
            g_app.averageTime = 0.0f;
            g_app.bestTime = 0.0f;
            g_app.lastReactionTime = 0.0f;
            StartNewRound();
        }
        return 0;

    case WM_SYSKEYDOWN:
        if (wParam == VK_F10)
        {
            ToggleFullscreen();
            return 0;
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
    wc.lpszClassName = L"ReactionTesterClass";

    if (!RegisterClassExW(&wc))
        return false;

    g_app.width = GetSystemMetrics(SM_CXSCREEN);
    g_app.height = GetSystemMetrics(SM_CYSCREEN);

    g_app.hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        wc.lpszClassName,
        L"Reaction Time Tester - Press ESC to exit",
        WS_POPUP,
        0, 0, g_app.width, g_app.height,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!g_app.hwnd)
        return false;

    // Register for raw mouse input
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = HID_USAGE_PAGE_GENERIC;
    rid.usUsage = HID_USAGE_GENERIC_MOUSE;
    rid.dwFlags = 0;
    rid.hwndTarget = g_app.hwnd;

    if (!RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE)))
        return false;

    ShowWindow(g_app.hwnd, SW_SHOW);
    UpdateWindow(g_app.hwnd);

    return true;
}

bool InitD3D11()
{
    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createFlags, featureLevels, 1, D3D11_SDK_VERSION,
        &g_app.device, &featureLevel, &g_app.context);

    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice1> dxgiDevice;
    g_app.device.As(&dxgiDevice);
    dxgiDevice->SetMaximumFrameLatency(1);

    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);

    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width = g_app.width;
    scDesc.Height = g_app.height;
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 2;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc = {};
    fsDesc.Windowed = FALSE;

    hr = factory->CreateSwapChainForHwnd(
        g_app.device.Get(), g_app.hwnd, &scDesc, &fsDesc, nullptr, &g_app.swapChain);

    if (FAILED(hr)) return false;

    factory->MakeWindowAssociation(g_app.hwnd, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<ID3D11Texture2D> backBuffer;
    g_app.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    hr = g_app.device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_app.rtv);

    return SUCCEEDED(hr);
}

bool InitD2D()
{
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_app.d2dFactory.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown **>(g_app.dwriteFactory.GetAddressOf()));
    if (FAILED(hr)) return false;

    // Regular text format
    hr = g_app.dwriteFactory->CreateTextFormat(
        L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        24.0f, L"en-us", &g_app.textFormat);
    if (FAILED(hr)) return false;

    // Large text format for center display
    hr = g_app.dwriteFactory->CreateTextFormat(
        L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        48.0f, L"en-us", &g_app.textFormatLarge);
    if (FAILED(hr)) return false;
    g_app.textFormatLarge->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_app.textFormatLarge->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    ComPtr<IDXGISurface> surface;
    hr = g_app.swapChain->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (FAILED(hr)) return false;

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    hr = g_app.d2dFactory->CreateDxgiSurfaceRenderTarget(surface.Get(), &props, &g_app.d2dRT);
    if (FAILED(hr)) return false;

    hr = g_app.d2dRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 1.0f, 0.0f, 1.0f), &g_app.textBrush);
    if (FAILED(hr)) return false;

    hr = g_app.d2dRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.2f, 0.2f, 1.0f), &g_app.redBrush);
    return SUCCEEDED(hr);
}

void ToggleFullscreen()
{
    g_app.textBrush.Reset();
    g_app.redBrush.Reset();
    g_app.d2dRT.Reset();
    g_app.rtv.Reset();
    g_app.context->ClearState();
    g_app.context->Flush();

    g_app.isFullscreen = !g_app.isFullscreen;

    HRESULT hr = g_app.swapChain->SetFullscreenState(g_app.isFullscreen ? TRUE : FALSE, nullptr);
    if (FAILED(hr))
    {
        g_app.isFullscreen = !g_app.isFullscreen;
    }

    if (!g_app.isFullscreen)
    {
        SetWindowLongPtrW(g_app.hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowLongPtrW(g_app.hwnd, GWL_EXSTYLE, 0);

        int winWidth = 1280, winHeight = 720;
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
        SetWindowLongPtrW(g_app.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongPtrW(g_app.hwnd, GWL_EXSTYLE, WS_EX_TOPMOST);

        g_app.width = GetSystemMetrics(SM_CXSCREEN);
        g_app.height = GetSystemMetrics(SM_CYSCREEN);

        SetWindowPos(g_app.hwnd, HWND_TOPMOST, 0, 0, g_app.width, g_app.height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    hr = g_app.swapChain->ResizeBuffers(0, g_app.width, g_app.height, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);

    ComPtr<ID3D11Texture2D> backBuffer;
    g_app.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    g_app.device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_app.rtv);

    ComPtr<IDXGISurface> surface;
    g_app.swapChain->GetBuffer(0, IID_PPV_ARGS(&surface));

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    g_app.d2dFactory->CreateDxgiSurfaceRenderTarget(surface.Get(), &props, &g_app.d2dRT);
    g_app.d2dRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 1.0f, 0.0f, 1.0f), &g_app.textBrush);
    g_app.d2dRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.2f, 0.2f, 1.0f), &g_app.redBrush);
}

void Render()
{
    auto now = Clock::now();

    // Check if we should transition from Waiting to Flashing
    if (g_app.state == TestState::Waiting)
    {
        float elapsed = std::chrono::duration<float, std::milli>(now - g_app.roundStartTime).count();
        if (elapsed >= g_app.targetDelayMs)
        {
            g_app.state = TestState::Flashing;
            g_app.flashStartTime = Clock::now();

            // Play beep in audio mode (do it right after capturing time for accuracy)
            if (g_app.audioMode && !g_app.beepPlayed)
            {
                g_app.beepPlayed = true;
                PlayBeepWASAPI(); // Low-latency WASAPI beep
            }
        }
    }

    // Determine background color
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (g_app.state == TestState::Flashing && !g_app.audioMode)
    {
        // Only flash white in visual mode
        clearColor[0] = clearColor[1] = clearColor[2] = 1.0f;
    }
    else if (g_app.state == TestState::TooEarly)
    {
        clearColor[0] = 0.8f; // Red-ish for false start
        clearColor[1] = 0.1f;
        clearColor[2] = 0.1f;
    }

    g_app.context->ClearRenderTargetView(g_app.rtv.Get(), clearColor);

    // Draw overlay
    g_app.d2dRT->BeginDraw();

    // Draw reaction times log on left
    float logY = 80.0f;

    // Header with mode indicator
    std::wstring headerText = g_app.audioMode ? L"AUDIO REACTION" : L"VISUAL REACTION";
    D2D1_RECT_F headerRect = D2D1::RectF(20.0f, 20.0f, 400.0f, 60.0f);
    g_app.d2dRT->DrawText(headerText.c_str(), (UINT32)headerText.length(), g_app.textFormat.Get(), headerRect, g_app.textBrush.Get());

    // Stats
    if (!g_app.reactionTimes.empty())
    {
        wchar_t statsBuffer[128];
        swprintf_s(statsBuffer, L"Avg: %.1f ms  Best: %.1f ms", g_app.averageTime, g_app.bestTime);
        D2D1_RECT_F statsRect = D2D1::RectF(20.0f, 45.0f, 600.0f, 80.0f);
        g_app.d2dRT->DrawText(statsBuffer, (UINT32)wcslen(statsBuffer), g_app.textFormat.Get(), statsRect, g_app.textBrush.Get());
    }

    // Log entries
    for (size_t i = 0; i < g_app.reactionTimes.size(); ++i)
    {
        wchar_t buffer[64];
        swprintf_s(buffer, L"%2zu. %.1f ms", i + 1, g_app.reactionTimes[i]);
        D2D1_RECT_F logRect = D2D1::RectF(20.0f, logY, 250.0f, logY + 26.0f);
        g_app.d2dRT->DrawText(buffer, (UINT32)wcslen(buffer), g_app.textFormat.Get(), logRect, g_app.textBrush.Get());
        logY += 26.0f;
    }

    // Center message based on state
    D2D1_RECT_F centerRect = D2D1::RectF(0.0f, 0.0f, (float)g_app.width, (float)g_app.height);

    if (g_app.state == TestState::Waiting)
    {
        g_app.d2dRT->DrawText(L"Wait for it...", 14, g_app.textFormatLarge.Get(), centerRect, g_app.textBrush.Get());
    }
    else if (g_app.state == TestState::Flashing)
    {
        // Use darker color on white background (visual mode), green on black (audio mode)
        auto brush = g_app.audioMode ? g_app.textBrush.Get() : g_app.redBrush.Get();
        g_app.d2dRT->DrawText(L"CLICK!", 6, g_app.textFormatLarge.Get(), centerRect, brush);
    }
    else if (g_app.state == TestState::TooEarly)
    {
        g_app.d2dRT->DrawText(L"TOO EARLY!\nClick to retry", 24, g_app.textFormatLarge.Get(), centerRect, g_app.textBrush.Get());
    }

    // Instructions at bottom
    std::wstring modeStr = g_app.audioMode ? L"AUDIO" : L"VISUAL";
    if (g_app.audioMode && g_app.audioInitialized)
    {
        wchar_t latencyStr[32];
        swprintf_s(latencyStr, L"AUDIO ~%.1fms", g_app.audioLatencyMs);
        modeStr = latencyStr;
    }
    else if (g_app.audioMode && !g_app.audioInitialized)
    {
        modeStr = L"AUDIO (N/A)";
    }
    std::wstring instructions = L"ESC=Exit | SPACE=Clear | F1=[" + modeStr +
                                L"] | F10=" + std::wstring(g_app.isFullscreen ? L"FSE" : L"WIN");
    D2D1_RECT_F instrRect = D2D1::RectF(20.0f, (float)g_app.height - 40.0f, (float)g_app.width - 20.0f, (float)g_app.height - 10.0f);
    g_app.d2dRT->DrawText(instructions.c_str(), (UINT32)instructions.length(), g_app.textFormat.Get(), instrRect, g_app.textBrush.Get());

    g_app.d2dRT->EndDraw();

    g_app.swapChain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
}

void Cleanup()
{
    CleanupWASAPI();

    if (g_app.swapChain)
    {
        g_app.swapChain->SetFullscreenState(FALSE, nullptr);
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    // Initialize COM for WASAPI
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

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

    // Initialize WASAPI for low-latency audio (non-fatal if fails)
    if (!InitWASAPI())
    {
        // Audio won't work but visual mode still will
        g_app.audioInitialized = false;
    }

    StartNewRound();

    MSG msg = {};
    while (g_app.running)
    {
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
    CoUninitialize();
    return 0;
}
