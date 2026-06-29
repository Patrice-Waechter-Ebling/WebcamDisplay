#include <windows.h>
#include <dshow.h>
#include <commctrl.h>
#include <vector>
#include <string>
#include <oaidl.h>
#include <ocidl.h>
#include <amstream.h>
#include <msxml.h>

DEFINE_GUID(CLSID_SampleGrabber,0xC1F400A0,0x3F08,0x11D3,0x9F0B,0x006008039E37);
DEFINE_GUID(IID_ISampleGrabber, 0x6b652fff, 0x11fe, 0x4fce, 0x92, 0xad, 0x02, 0x66, 0xb5, 0xd7, 0xc7, 0x8f);

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "quartz.lib")
#pragma comment(lib, "comctl32.lib")

// IDs
#define ID_TIMER_STATUS   1
#define ID_BUTTON_CHRONO  1001
#define ID_STATUSBAR      2001
#define ID_MENU_CAMERA    3000
#define ID_MENU_CAPTURE   4000
#define ID_MENU_RECORD    5000

HINSTANCE g_hInst;
HWND g_hMain = nullptr;
HWND g_hVideo = nullptr;
HWND g_hStatus = nullptr;
HWND g_hButton = nullptr;

IGraphBuilder* g_pGraph = nullptr;
ICaptureGraphBuilder2* g_pBuilder = nullptr;
IMediaControl* g_pControl = nullptr;
IVideoWindow* g_pVidWin = nullptr;
IBaseFilter* g_pCap = nullptr;

IBaseFilter* g_pMux = nullptr;
IBaseFilter* g_pWriter = nullptr;
bool g_recording = false;

bool g_chrono = false;
ULONGLONG g_chronoStart = 0;

struct CameraInfo {
    std::wstring name;
    IMoniker* moniker;
};
extern "C" {
    MIDL_INTERFACE("0579154a-2b53-4994-b0d0-e773148eff85")ISampleGrabberCB : public IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE SampleCB(double SampleTime,IMediaSample * pSample) = 0;
        virtual HRESULT STDMETHODCALLTYPE BufferCB(double SampleTime,BYTE* pBuffer,LONG BufferLen) = 0;
    };
    MIDL_INTERFACE("6b652fff-11fe-4fce-92ad-0266b5d7c78f")ISampleGrabber : public IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* pType) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* pType) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(LONG* pBufferSize,LONG* pBuffer) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** ppSample) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* pCallback,LONG WhichMethodToCallback) = 0;
    };
}
std::vector<CameraInfo> g_cams;
static std::wstring FormatChrono()
{
    if (!g_chrono) return L"00:00.0";
    ULONGLONG now = GetTickCount64();
    ULONGLONG diff = now - g_chronoStart;
    int sec = (int)(diff / 1000);
    int ms = (int)((diff % 1000) / 100);
    int min = sec / 60;
    sec %= 60;
    wchar_t buf[32];
    swprintf(buf, 32, L"%02d:%02d.%d", min, sec, ms);
    return buf;
}
static std::wstring FormatDateHeureLocale()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf(buf, 64, L"%02d/%02d/%04d %02d:%02d:%02d",st.wDay, st.wMonth, st.wYear,st.wHour, st.wMinute, st.wSecond);
    return buf;
}
static void MiseJourStatut()
{
    int parts[3] = { 120, 320, -1 };
    SendMessage(g_hStatus, SB_SETPARTS, 3, (LPARAM)parts);
    SendMessage(g_hStatus, SB_SETTEXT, 0, (LPARAM)L"WebcamDisplay");
    SendMessage(g_hStatus, SB_SETTEXT, 1, (LPARAM)FormatDateHeureLocale().c_str());
    SendMessage(g_hStatus, SB_SETTEXT, 2, (LPARAM)FormatChrono().c_str());
}
static void LibererCameras()
{
    for (auto& c : g_cams)
        if (c.moniker) c.moniker->Release();
    g_cams.clear();
}
static void EnumererCameras()
{
    LibererCameras();
    ICreateDevEnum* pDevEnum = nullptr;
    IEnumMoniker* pEnum = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,IID_ICreateDevEnum, (void**)&pDevEnum)))return;
    pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    if (!pEnum) {pDevEnum->Release();return;}
    IMoniker* pMoniker = nullptr;
    while (pEnum->Next(1, &pMoniker, nullptr) == S_OK)
    {
        IPropertyBag* pBag = nullptr;
        pMoniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)&pBag);
        VARIANT var;
        VariantInit(&var);
        pBag->Read(L"FriendlyName", &var, nullptr);
        CameraInfo ci;
        ci.name = var.bstrVal;
        ci.moniker = pMoniker;
        g_cams.push_back(ci);
        VariantClear(&var);
        pBag->Release();
    }
    pEnum->Release();
    pDevEnum->Release();
}
static void CreerMenuCamera(HMENU hMenuBar)
{
    HMENU hCamMenu = CreateMenu();
    for (size_t i = 0; i < g_cams.size(); ++i)
    {
        AppendMenu(hCamMenu, MF_STRING, ID_MENU_CAMERA + (UINT)i, g_cams[i].name.c_str());
    }
    AppendMenu(hCamMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hCamMenu, MF_STRING, ID_MENU_CAPTURE, L"Capture photo");
    AppendMenu(hCamMenu, MF_STRING, ID_MENU_RECORD, L"Start/Stop video");
    AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hCamMenu, L"Camera");
}
static void NettoyerGraphe()
{
    if (g_pControl) { g_pControl->Stop(); g_pControl->Release(); g_pControl = nullptr; }
    if (g_pVidWin) { g_pVidWin->put_Visible(OAFALSE); g_pVidWin->put_Owner(NULL); g_pVidWin->Release(); g_pVidWin = nullptr; }
    if (g_pCap) { g_pGraph->RemoveFilter(g_pCap); g_pCap->Release(); g_pCap = nullptr; }
    if (g_pMux) { g_pGraph->RemoveFilter(g_pMux); g_pMux->Release(); g_pMux = nullptr; }
    if (g_pWriter) { g_pGraph->RemoveFilter(g_pWriter); g_pWriter->Release(); g_pWriter = nullptr; }
    g_recording = false;
}
static HRESULT InitialiserGraphe()
{
    if (!g_pGraph)
    {
        if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,IID_IGraphBuilder, (void**)&g_pGraph)))return E_FAIL;
    }
    if (!g_pBuilder)
    {
        if (FAILED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2, (void**)&g_pBuilder)))return E_FAIL;
        g_pBuilder->SetFiltergraph(g_pGraph);
    }

    return S_OK;
}

static HRESULT InitialiserCamera(size_t index)
{
    if (index >= g_cams.size()) return E_INVALIDARG;
    NettoyerGraphe();
    InitialiserGraphe();
    IBaseFilter* pCap = nullptr;
    if (FAILED(g_cams[index].moniker->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)&pCap))) return E_FAIL;
    g_pGraph->AddFilter(pCap, L"Capture");
    g_pCap = pCap;
    if (FAILED(g_pBuilder->RenderStream(&PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video,pCap, nullptr, nullptr)))return E_FAIL;
    g_pGraph->QueryInterface(IID_IMediaControl, (void**)&g_pControl);
    g_pGraph->QueryInterface(IID_IVideoWindow, (void**)&g_pVidWin);
    g_pVidWin->put_Owner((OAHWND)g_hVideo);
    g_pVidWin->put_WindowStyle(WS_CHILD | WS_CLIPSIBLINGS);
    RECT rc;
    GetClientRect(g_hVideo, &rc);
    g_pVidWin->SetWindowPosition(0, 0, rc.right, rc.bottom);
    g_pVidWin->put_Visible(OATRUE);
    g_pControl->Run();
    return S_OK;
}
static HRESULT SauvegarderFrameBMP(const wchar_t* filename)
{
    if (!g_pGraph) return E_FAIL;
    IBaseFilter* pGrabberFilter = nullptr;
    ISampleGrabber* pGrabber = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER,IID_IBaseFilter, (void**)&pGrabberFilter)))return E_FAIL;
    g_pGraph->AddFilter(pGrabberFilter, L"Grabber");
    pGrabberFilter->QueryInterface(IID_ISampleGrabber, (void**)&pGrabber);
    AM_MEDIA_TYPE mt = {};
    mt.majortype = MEDIATYPE_Video;
    mt.subtype = MEDIASUBTYPE_RGB24;
    pGrabber->SetMediaType(&mt);
    pGrabber->SetBufferSamples(TRUE);
    Sleep(200);
    long size = 0;
    if (FAILED(pGrabber->GetCurrentBuffer(&size, nullptr)))
    {
        g_pGraph->RemoveFilter(pGrabberFilter);
        pGrabber->Release();
        pGrabberFilter->Release();
        return E_FAIL;
    }
    std::vector<BYTE> buffer(size);
    if (FAILED(pGrabber->GetCurrentBuffer(&size, (long*)buffer.data())))
    {
        g_pGraph->RemoveFilter(pGrabberFilter);
        pGrabber->Release();
        pGrabberFilter->Release();
        return E_FAIL;
    }
    /// image format SVGA
    int width = 800;
    int height = 600;
    int bpp = 24;
    int stride = width * (bpp / 8);
    BITMAPFILEHEADER bfh = {};
    BITMAPINFOHEADER bih = {};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = width;
    bih.biHeight = -height; // inverse
    bih.biPlanes = 1;
    bih.biBitCount = bpp;
    bih.biCompression = BI_RGB;
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + size;
    HANDLE hFile = CreateFile(filename, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        g_pGraph->RemoveFilter(pGrabberFilter);
        pGrabber->Release();
        pGrabberFilter->Release();
        return E_FAIL;
    }
    DWORD written;
    WriteFile(hFile, &bfh, sizeof(bfh), &written, nullptr);
    WriteFile(hFile, &bih, sizeof(bih), &written, nullptr);
    WriteFile(hFile, buffer.data(), size, &written, nullptr);
    CloseHandle(hFile);
    g_pGraph->RemoveFilter(pGrabberFilter);
    pGrabber->Release();
    pGrabberFilter->Release();
    return S_OK;
}
// AVI Mux
static HRESULT LancerEnregistrement(const wchar_t* filename)
{
    if (!g_pGraph || !g_pBuilder || !g_pCap) return E_FAIL;
    if (g_recording) return S_OK;
    if (FAILED(CoCreateInstance(CLSID_AviDest, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)&g_pMux)))return E_FAIL;
    g_pGraph->AddFilter(g_pMux, L"AVI Mux");
    if (FAILED(CoCreateInstance(CLSID_FileWriter, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)&g_pWriter))) return E_FAIL;
    g_pGraph->AddFilter(g_pWriter, L"File Writer");
    IFileSinkFilter* sink = nullptr;
    g_pWriter->QueryInterface(IID_IFileSinkFilter, (void**)&sink);
    sink->SetFileName(filename, nullptr);
    sink->Release();
    if (FAILED(g_pBuilder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,g_pCap, nullptr, g_pMux)))return E_FAIL;
    if (FAILED(g_pBuilder->RenderStream(nullptr, nullptr, g_pMux, nullptr, g_pWriter)))return E_FAIL;
    g_recording = true;
    return S_OK;
}
static void ArreterEnregistrement()
{
    if (!g_recording) return;
    if (g_pControl) g_pControl->Stop();
    if (g_pWriter) { g_pGraph->RemoveFilter(g_pWriter); g_pWriter->Release(); g_pWriter = nullptr; }
    if (g_pMux) { g_pGraph->RemoveFilter(g_pMux); g_pMux->Release(); g_pMux = nullptr; }
    g_recording = false;
    if (g_pControl) g_pControl->Run();
}
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        g_hVideo = CreateWindowEx(WS_EX_CLIENTEDGE, L"STATIC", L"",WS_CHILD | WS_VISIBLE,0, 0,390, 300,hWnd, nullptr, g_hInst, nullptr);
        g_hButton = CreateWindow(L"BUTTON", L"Start Chrono",WS_CHILD | WS_VISIBLE,0, 330, 125, 30,hWnd, (HMENU)ID_BUTTON_CHRONO, g_hInst, nullptr);
        g_hStatus = CreateStatusWindow(WS_CHILD | WS_VISIBLE, L"", hWnd, ID_STATUSBAR);
        SetTimer(hWnd, ID_TIMER_STATUS, 200, nullptr);
        EnumererCameras();
        HMENU hMenuBar = CreateMenu();
        CreerMenuCamera(hMenuBar);
        SetMenu(hWnd, hMenuBar);
        InitialiserGraphe();
        if (!g_cams.empty())InitialiserCamera(0);
        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0,SWP_NOMOVE | SWP_NOSIZE);
        return 0;
    }
    case WM_SIZE: SendMessage(g_hStatus, WM_SIZE, 0, 0);return 0;
    case WM_TIMER:if (wParam == ID_TIMER_STATUS)MiseJourStatut();return 0;
    case WM_COMMAND:
    {
        UINT id = LOWORD(wParam);
        if (id == ID_BUTTON_CHRONO)
        {
            g_chrono = !g_chrono;
            if (g_chrono){g_chronoStart = GetTickCount64();SetWindowText(g_hButton, L"Arreter");}
            else{SetWindowText(g_hButton, L"Demarrer");}
            return 0;
        }
        if (id >= ID_MENU_CAMERA && id < ID_MENU_CAMERA + 1000)
        {
            size_t idx = id - ID_MENU_CAMERA;
            if (idx < g_cams.size())InitialiserCamera(idx);
            return 0;
        }
        if (id == ID_MENU_CAPTURE)
        {
            SauvegarderFrameBMP(L"WebcamDisplay.bmp");
            MessageBox(hWnd, L"Photo enregistree en WebcamDisplay.bmp", L"Capture", MB_OK);
            return 0;
        }
        if (id == ID_MENU_RECORD)
        {
            if (!g_recording){LancerEnregistrement(L"WebcamDisplay.avi");MessageBox(hWnd, L"Enregistrement demarre (video.avi)", L"Record", MB_OK);}
            else{ArreterEnregistrement();MessageBox(hWnd, L"Enregistrement arrête", L"Record", MB_OK);}
            return 0;
        }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hWnd, ID_TIMER_STATUS);
        NettoyerGraphe();
        if (g_pBuilder) { g_pBuilder->Release(); g_pBuilder = nullptr; }
        if (g_pGraph) { g_pGraph->Release(); g_pGraph = nullptr; }
        LibererCameras();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    g_hInst = hInstance;
    CoInitialize(nullptr);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"WebcamDisplay";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&wc);
    RECT rc = {0,0,0,0};
    GetClientRect(GetDesktopWindow(),&rc);
    g_hMain = CreateWindow(wc.lpszClassName, L"WebcamDisplay",WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,rc.right-400, rc.top, 400, 400,nullptr, nullptr, hInstance, nullptr);

    ShowWindow(g_hMain, nCmdShow);
    UpdateWindow(g_hMain);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}
