#define _USE_MATH_DEFINES
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <fftw3.h> // библиотека для FFT


static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);


struct FFTData {
    std::vector<float> frequencies;
    std::vector<float> magnitudes;
};

//  генерация и подсчет FFT 
FFTData GenerateFFTData() {
    FFTData data;
    const double sampleRate = 10000000.0; // 10 МГц
    const int N = 65536;

    const double freq1 = -1000000.0;  // -1 МГц 
    const double freq2 = 2000000.0;   // 2 МГц 

    fftw_complex* in = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * N);
    fftw_complex* out = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * N);

    fftw_plan plan = fftw_plan_dft_1d(N, in, out, FFTW_FORWARD, FFTW_ESTIMATE);

    // Генерируем сигнал
    for (int i = 0; i < N; ++i) {
        double t = i / sampleRate;

        // первый сигнал (-1 МГц)
        double real1 = 1.0 * std::cos(2.0 * M_PI * freq1 * t);
        double imag1 = 1.0 * std::sin(2.0 * M_PI * freq1 * t); // тоже 1.0

        // второй сигнал (2 МГц)
        double real2 = 1.5 * std::cos(2.0 * M_PI * freq2 * t);
        double imag2 = 1.5 * std::sin(2.0 * M_PI * freq2 * t); // тоже 1.5

        in[i][0] = real1 + real2;
        in[i][1] = imag1 + imag2;
    }

    // Считаем FFT
    fftw_execute(plan);

    // Отрицательные частоты
    for (int i = N / 2; i < N; ++i) {
        double currentFreq = -((N - i) * sampleRate) / N;

        double realPart = out[i][0];
        double imagPart = out[i][1];
        double magnitude = std::sqrt(realPart * realPart + imagPart * imagPart) / N;

        data.frequencies.push_back((float)currentFreq);
        data.magnitudes.push_back((float)magnitude);
    }

    // Положительные частоты
    for (int i = 0; i < N / 2; ++i) {
        double currentFreq = (i * sampleRate) / N;

        double realPart = out[i][0];
        double imagPart = out[i][1];
        double magnitude = std::sqrt(realPart * realPart + imagPart * imagPart) / N;

        data.frequencies.push_back((float)currentFreq);
        data.magnitudes.push_back((float)magnitude);
    }

    fftw_destroy_plan(plan);
    fftw_free(in);
    fftw_free(out);

    return data;
}

// отрисовка графика ImGui 
void DrawFFTWindow(const FFTData& fftData)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15.0f, 15.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));

    ImGui::Begin("График FFT спектра");

    if (!fftData.magnitudes.empty()) {
        int maxPoints = (int)fftData.magnitudes.size();

        static int start_idx = 0;
        static int visible_points = maxPoints;

        static float view_max_db = 0.0f;
        static float view_min_db = -90.0f;

        auto ResetScale = [&]() {
            start_idx = 0;
            visible_points = maxPoints;
            view_max_db = 0.0f;
            view_min_db = -90.0f;
            };

        static bool init = true;
        if (init) {
            ResetScale();
            init = false;
        }

        if (ImGui::Button("Весь спектр: -1 МГц .. +2 МГц")) {
            ResetScale();
        }

        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 plot_size = ImVec2(avail.x, avail.y - 35.0f);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + plot_size.x, p0.y + plot_size.y);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();

        ImGui::SetCursorScreenPos(p0);
        ImGui::InvisibleButton("##FFTPlotArea", plot_size);
        bool is_hovered = ImGui::IsItemHovered();
        bool is_active = ImGui::IsItemActive();

        if (is_hovered) {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                if (ImGui::GetIO().KeyCtrl) {
                    float zoom_factor_y = 1.2f;
                    float range_db = view_max_db - view_min_db;
                    float mouse_y_t = 1.0f - ((ImGui::GetMousePos().y - p0.y) / plot_size.y);
                    if (mouse_y_t < 0.0f) mouse_y_t = 0.0f;
                    if (mouse_y_t > 1.0f) mouse_y_t = 1.0f;
                    float db_at_mouse = view_min_db + mouse_y_t * range_db;
                    float new_range_db = (wheel > 0) ? (range_db / zoom_factor_y) : (range_db * zoom_factor_y);
                    if (new_range_db < 5.0f) new_range_db = 5.0f;
                    view_min_db = db_at_mouse - mouse_y_t * new_range_db;
                    view_max_db = db_at_mouse + (1.0f - mouse_y_t) * new_range_db;
                }
                else {
                    float zoom_factor = 1.2f;
                    int old_visible = visible_points;
                    if (wheel > 0) visible_points = (int)(visible_points / zoom_factor);
                    else visible_points = (int)(visible_points * zoom_factor);
                    if (visible_points < 10) visible_points = 10;
                    if (visible_points > maxPoints) visible_points = maxPoints;
                    float mouse_x_t = (ImGui::GetMousePos().x - p0.x) / plot_size.x;
                    if (mouse_x_t < 0.0f) mouse_x_t = 0.0f;
                    if (mouse_x_t > 1.0f) mouse_x_t = 1.0f;
                    start_idx += (int)((old_visible - visible_points) * mouse_x_t);
                }
            }
        }

        if (is_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            float delta_x = ImGui::GetIO().MouseDelta.x;
            float delta_y = ImGui::GetIO().MouseDelta.y;
            float points_per_pixel = (float)visible_points / plot_size.x;
            start_idx -= (int)(delta_x * points_per_pixel);
            float db_per_pixel = (view_max_db - view_min_db) / plot_size.y;
            view_max_db += delta_y * db_per_pixel;
            view_min_db += delta_y * db_per_pixel;
        }

        if (start_idx < 0) start_idx = 0;
        if (start_idx + visible_points > maxPoints) start_idx = maxPoints - visible_points;

        draw_list->PushClipRect(p0, p1, true);

        if (visible_points > 0 && plot_size.x > 0.0f) {
            float points_per_pixel = (float)visible_points / plot_size.x;
            ImVec2 last_p;
            bool has_last = false;
            for (int px = 0; px < (int)plot_size.x; ++px) {
                int idx_start = start_idx + (int)(px * points_per_pixel);
                int idx_end = start_idx + (int)((px + 1) * points_per_pixel);
                if (idx_start >= maxPoints) break;
                if (idx_end > maxPoints) idx_end = maxPoints;
                if (idx_end <= idx_start) idx_end = idx_start + 1;

                float max_val = 0.0f;
                for (int i = idx_start; i < idx_end; ++i) {
                    if (fftData.magnitudes[i] > max_val) {
                        max_val = fftData.magnitudes[i];
                    }
                }

                float db_val = 20.0f * std::log10(max_val > 1e-10f ? max_val : 1e-10f);
                float t_y = (db_val - view_min_db) / (view_max_db - view_min_db);
                float x = p0.x + px;
                float y = p1.y - t_y * plot_size.y;

                ImVec2 current_p(x, y);
                if (has_last) {
                    draw_list->AddLine(last_p, current_p, IM_COL32(255, 255, 255, 255), 1.0f);
                }
                last_p = current_p;
                has_last = true;
            }
        }
        draw_list->PopClipRect();

        int num_x_ticks = 10;
        int num_y_ticks = 4;

        for (int i = 0; i <= num_y_ticks; ++i) {
            float t = (float)i / num_y_ticks;
            float y = p1.y - t * (p1.y - p0.y);
            draw_list->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(255, 255, 255, 30));
            float db_label_val = view_min_db + t * (view_max_db - view_min_db);
            char labelY[16];
            snprintf(labelY, sizeof(labelY), "%.1f dB", db_label_val);
            draw_list->AddText(ImVec2(p0.x + 5, y - 15), IM_COL32(200, 200, 200, 255), labelY);
        }

        for (int i = 0; i <= num_x_ticks; ++i) {
            float t = (float)i / num_x_ticks;
            float x = p0.x + t * (p1.x - p0.x);
            draw_list->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(255, 255, 255, 30));

            float start_freq = fftData.frequencies[start_idx];
            int end_idx = start_idx + visible_points - 1;
            if (end_idx >= maxPoints) end_idx = maxPoints - 1;
            float end_freq = fftData.frequencies[end_idx];
            float freq = start_freq + t * (end_freq - start_freq);

            if (std::abs(freq) < 200.0f) freq = 0.0f;

            char labelX[32];
            if (freq >= 1000000.0f || freq <= -1000000.0f) snprintf(labelX, sizeof(labelX), "%.2f МГц", freq / 1000000.0f);
            else if (freq >= 1000.0f || freq <= -1000.0f) snprintf(labelX, sizeof(labelX), "%.1f кГц", freq / 1000.0f);
            else snprintf(labelX, sizeof(labelX), "%.0f Гц", freq);

            ImVec2 textSize = ImGui::CalcTextSize(labelX);
            float textX = x - textSize.x / 2.0f;
            if (i == 0) textX = x + 5.0f;
            else if (i == num_x_ticks) textX = x - textSize.x - 5.0f;

            draw_list->AddText(ImVec2(textX, p1.y + 8.0f), IM_COL32(200, 200, 200, 255), labelX);
        }

        float current_start_freq = fftData.frequencies[start_idx];
        int current_end_idx = start_idx + visible_points - 1;
        if (current_end_idx >= maxPoints) current_end_idx = maxPoints - 1;
        float current_end_freq = fftData.frequencies[current_end_idx];

        if (current_start_freq <= 0.0f && current_end_freq >= 0.0f) {
            float t_zero = (0.0f - current_start_freq) / (current_end_freq - current_start_freq);
            float x_zero = p0.x + t_zero * plot_size.x;
            draw_list->AddLine(ImVec2(x_zero, p0.y), ImVec2(x_zero, p1.y), IM_COL32(255, 80, 80, 200), 1.5f);
            const char* centerLabel = "0 Гц Центр";
            ImVec2 centerTextSize = ImGui::CalcTextSize(centerLabel);
            draw_list->AddText(ImVec2(x_zero - centerTextSize.x / 2.0f, p1.y + 22.0f), IM_COL32(255, 100, 100, 255), centerLabel);
        }

        if (is_hovered) {
            ImVec2 mousePos = ImGui::GetMousePos();
            float percentage = (mousePos.x - p0.x) / (p1.x - p0.x);
            if (percentage >= 0.0f && percentage <= 1.0f) {
                int hoveredIndex = start_idx + (int)(percentage * visible_points);
                if (hoveredIndex >= 0 && hoveredIndex < maxPoints) {
                    float freq = fftData.frequencies[hoveredIndex];
                    float mag = fftData.magnitudes[hoveredIndex];
                    float db = 20.0f * std::log10(mag > 1e-10f ? mag : 1e-10f);

                    char freqStr[64];
                    if (freq >= 1000000.0f || freq <= -1000000.0f) snprintf(freqStr, sizeof(freqStr), "%.2f МГц", freq / 1000000.0f);
                    else if (freq >= 1000.0f || freq <= -1000.0f) snprintf(freqStr, sizeof(freqStr), "%.2f кГц", freq / 1000.0f);
                    else snprintf(freqStr, sizeof(freqStr), "%.0f Гц", freq);

                    ImGui::BeginTooltip();
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Частота: %s", freqStr);
                    ImGui::Text("Уровень: %.2f dBFS", db);
                    ImGui::Text("Амплитуда (лин): %.6f", mag);
                    ImGui::EndTooltip();
                    draw_list->AddLine(ImVec2(mousePos.x, p0.y), ImVec2(mousePos.x, p1.y), IM_COL32(255, 255, 0, 150), 1.0f);
                }
            }
        }
    }
    else {
        ImGui::SetCursorPos(ImVec2(10, 10));
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Данные не загружены!");
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// --- Main loop ---
int main(int, char**)
{
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ImGui Example", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Dear ImGui DirectX11 Example", WS_OVERLAPPEDWINDOW, 100, 100, (int)(1280 * main_scale), (int)(800 * main_scale), nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\arial.ttf", 18.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
    if (font == nullptr) {
        io.Fonts->AddFontDefault();
    }


    FFTData myFFT = GenerateFFTData();

    bool done = false;
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Отрисовка графика
        DrawFFTWindow(myFFT);

        ImGui::Render();
        const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

// --- DirectX 11 Helper Functions ---
bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
