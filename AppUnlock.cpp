#define NOMINMAX
#include "framework.h"
#include "AppUnlock.h"
#include "AppVersion.generated.h"
#include "UnlockEngine.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <algorithm>
#include <memory>
#include <thread>
#include <mutex>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Dwmapi.lib")

namespace {
constexpr wchar_t WindowClass[] = L"AppUnlock.SimpleWindow";
constexpr UINT WorkComplete = WM_APP + 1;
constexpr int UnlockButton = 1001, AdminButton = 1002, RescanButton = 1003, PickButton = 1004;
constexpr COLORREF Background = RGB(18, 22, 30), Panel = RGB(27, 33, 44);
constexpr COLORREF Foreground = RGB(233, 239, 247), Muted = RGB(156, 170, 190), Accent = RGB(103, 218, 190);
HINSTANCE instance;
HWND diagnosis, unlockButton, adminButton, rescanButton, pickButton;
HFONT bodyFont, titleFont, smallFont;
HBRUSH backgroundBrush, panelBrush;
UINT dpi = 96;
bool busy = false, closing = false, administrator = false;
bool adminPromptShown = false;
std::vector<std::wstring> selection;
unlock::Report report;
std::wstring status = L"Listo para analizar", selectedLabel = L"Archivos o carpetas · análisis recursivo";
std::jthread worker;
std::mutex resultMutex;
struct Result { unlock::Report report; std::wstring outcome; };
std::unique_ptr<Result> result;

int Scale(int value) { return MulDiv(value, dpi, 96); }
RECT Box(int left, int top, int right, int bottom) { return { Scale(left), Scale(top), Scale(right), Scale(bottom) }; }
void Text(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color, UINT flags = DT_LEFT) {
    auto old = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), -1, &rect, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}
void Fonts() {
    if (bodyFont) DeleteObject(bodyFont);
    if (titleFont) DeleteObject(titleFont);
    if (smallFont) DeleteObject(smallFont);
    bodyFont = CreateFontW(-Scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    titleFont = CreateFontW(-Scale(25), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    smallFont = CreateFontW(-Scale(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}
void Place(HWND child, int x, int y, int width, int height) {
    MoveWindow(child, Scale(x), Scale(y), Scale(width), Scale(height), TRUE);
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont), TRUE);
}
void Layout() {
    Place(pickButton, 24, 64, 452, 154);
    Place(diagnosis, 30, 282, 440, 218);
    Place(unlockButton, 24, 520, 282, 44);
    Place(rescanButton, 318, 520, 158, 44);
    Place(adminButton, 24, 578, 452, 32);
}
void Update(HWND window) {
    EnableWindow(unlockButton, !busy && report.actionable());
    EnableWindow(rescanButton, !busy && !selection.empty());
    EnableWindow(pickButton, !busy);
    EnableWindow(adminButton, !busy);
    ShowWindow(adminButton, report.needsAdmin && !administrator ? SW_SHOW : SW_HIDE);
    SetWindowTextW(unlockButton, busy ? L"Procesando…" : L"Desbloquear");
    InvalidateRect(window, nullptr, FALSE);
}
void StartWork(HWND window, bool unblock, std::vector<unlock::Process> approved = {}) {
    if (busy || selection.empty()) return;
    if (worker.joinable()) worker.join();
    busy = true;
    status = unblock ? L"Desbloqueando la selección…" : L"Analizando archivos y subcarpetas…";
    SetWindowTextW(diagnosis, L"Revisando marcas de Internet, atributos, permisos y programas en uso…");
    Update(window);
    worker = std::jthread([window, roots = selection, unblock, approved = std::move(approved)](std::stop_token stop) {
        auto completed = std::make_unique<Result>();
        try {
            if (unblock) completed->report = unlock::Unblock(roots, approved, completed->outcome, stop);
            else completed->report = unlock::Analyze(roots, stop);
        } catch (...) {
            completed->report.errors = 1;
            completed->report.details.push_back(L"No se pudo completar el análisis. Probá con una selección más pequeña.");
        }
        {
            std::lock_guard guard(resultMutex);
            result = std::move(completed);
        }
        PostMessageW(window, WorkComplete, 0, 0);
    });
}
void Select(HWND window, std::vector<std::wstring> paths) {
    if (busy || paths.empty()) return;
    selection = std::move(paths);
    adminPromptShown = false;
    selectedLabel = selection.size() == 1 ? selection.front() : std::to_wstring(selection.size()) + L" elementos seleccionados";
    StartWork(window, false);
}
void Pick(HWND window) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Elegí archivos para analizar (las carpetas se pueden arrastrar a la ventana)");
    std::vector<std::wstring> paths;
    if (SUCCEEDED(dialog->Show(window))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dialog->GetResults(&items))) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &item))) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        paths.emplace_back(path);
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            items->Release();
        }
    }
    dialog->Release();
    Select(window, std::move(paths));
}
std::wstring Quote(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'\"') quoted.append(slashes * 2 + 1, L'\\');
        else quoted.append(slashes, L'\\');
        slashes = 0;
        quoted += ch;
    }
    quoted.append(slashes * 2, L'\\');
    return quoted + L'\"';
}
void Elevate(HWND window) {
    std::wstring executable(32768, L'\0');
    DWORD copied = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!copied || copied >= executable.size()) return;
    executable.resize(copied);
    std::wstring parameters;
    for (const auto& path : selection) parameters += Quote(path) + L" ";
    SHELLEXECUTEINFOW info{ sizeof(info) };
    info.hwnd = window;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_SHOWNORMAL;
    info.fMask = SEE_MASK_NOASYNC;
    if (ShellExecuteExW(&info)) DestroyWindow(window);
    else {
        DWORD error = GetLastError();
        status = error == ERROR_CANCELLED ? L"Se canceló el inicio como administrador" : L"No se pudo iniciar como administrador";
        SetWindowTextW(diagnosis, (unlock::Describe(report) + L"\r\n\r\n" + unlock::ErrorText(error)).c_str());
        Update(window);
    }
}
void RequestUnlock(HWND window) {
    if (busy || !report.actionable()) return;
    std::vector<unlock::Process> approved;
    const bool attributes = report.internet || report.readOnly || !report.readOnlyFolders.empty();
    if (attributes && MessageBoxW(window,
        L"Se quitarán las marcas de Internet de los archivos y el atributo de solo lectura de archivos y carpetas de la selección, de forma recursiva. Los permisos de seguridad no se modifican. Solo continuá si confiás en su origen.\r\n\r\n¿Querés autorizar estos cambios?",
        L"Confirmar cambios de atributos", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    for (size_t i = 0; i < report.processes.size(); ++i) {
        const auto& process = report.processes[i];
        std::wstring prompt = process.name + L" (PID " + std::to_wstring(process.identity.dwProcessId) + L")\r\n\r\n";
        prompt += process.directoryReference ? L"Este proceso mantiene abierta una carpeta de la selección. Es un candidato; podría no ser el que impide borrarla.\r\n\r\n" :
            L"Windows identificó este proceso usando archivos de la selección.\r\n\r\n";
        prompt += L"¿Autorizás cerrar este proceso?\r\n\r\nSe intentará el cierre normal. Si continúa reteniendo la selección, se finalizará. Podés perder cambios sin guardar y se pueden cerrar otras ventanas o chats del mismo programa. Guardá tu trabajo antes de aceptar.\r\n\r\nSí: autorizar este proceso.\r\nNo: dejarlo abierto.\r\nCancelar: no iniciar la operación.\r\n\r\nLos procesos críticos de Windows y AppUnlock no se finalizarán.";
        const std::wstring title = L"Proceso " + std::to_wstring(i + 1) + L" de " + std::to_wstring(report.processes.size());
        const int answer = MessageBoxW(window, prompt.c_str(), title.c_str(), MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
        if (answer == IDCANCEL) return;
        if (answer == IDYES) approved.push_back(process);
    }
    if (attributes || !approved.empty()) StartWork(window, true, std::move(approved));
}
void DrawButton(const DRAWITEMSTRUCT& item) {
    RECT rect = item.rcItem;
    const bool drop = item.CtlID == PickButton;
    const bool primary = item.CtlID == UnlockButton;
    const bool enabled = !(item.itemState & ODS_DISABLED);
    COLORREF fill = drop ? Panel : primary && enabled ? Accent : RGB(42, 50, 64);
    if (item.itemState & ODS_SELECTED) fill = RGB(58, 116, 108);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(drop ? PS_DASH : PS_SOLID, 1, drop ? RGB(74, 103, 119) : fill);
    auto oldBrush = SelectObject(item.hDC, brush);
    auto oldPen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, rect.left, rect.top, rect.right, rect.bottom, Scale(12), Scale(12));
    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);
    DeleteObject(brush); DeleteObject(pen);
    if (drop) {
        RECT title = rect; title.top += Scale(28); title.bottom = title.top + Scale(38);
        Text(item.hDC, L"Arrastre aquí", title, titleFont, Foreground, DT_CENTER | DT_SINGLELINE);
        RECT sub = rect; sub.top += Scale(80); sub.bottom = sub.top + Scale(24);
        Text(item.hDC, L"Un archivo o una carpeta", sub, bodyFont, Muted, DT_CENTER | DT_SINGLELINE);
        sub.top += Scale(26); sub.bottom += Scale(26);
        Text(item.hDC, L"También podés hacer clic para elegir archivos", sub, smallFont, Muted, DT_CENTER | DT_SINGLELINE);
    } else {
        wchar_t caption[128]{};
        GetWindowTextW(item.hwndItem, caption, 128);
        Text(item.hDC, caption, rect, bodyFont, primary && enabled ? Background : enabled ? Foreground : Muted,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (item.itemState & ODS_FOCUS) { InflateRect(&rect, -Scale(4), -Scale(4)); DrawFocusRect(item.hDC, &rect); }
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        dpi = GetDpiForWindow(window);
        Fonts();
        backgroundBrush = CreateSolidBrush(Background);
        panelBrush = CreateSolidBrush(Panel);
        auto button = [&](LPCWSTR title, int id) {
            return CreateWindowW(L"BUTTON", title, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        };
        pickButton = button(L"Arrastre aquí", PickButton);
        unlockButton = button(L"Desbloquear", UnlockButton);
        rescanButton = button(L"Volver a analizar", RescanButton);
        adminButton = button(L"Reiniciar como administrador", AdminButton);
        diagnosis = CreateWindowExW(0, L"EDIT", L"Arrastrá un archivo o una carpeta para ver su diagnóstico.\r\n\r\nLas carpetas se revisan de forma recursiva. El análisis no modifica tus archivos.",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 0, 0, window, nullptr, instance, nullptr);
        SendMessageW(diagnosis, EM_SETLIMITTEXT, 0x7ffffffe, 0);
        SendMessageW(diagnosis, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
        Layout();
        DragAcceptFiles(window, TRUE);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        Update(window);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        std::vector<std::wstring> paths;
        const UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length + 1, L'\0');
            DragQueryFileW(drop, i, path.data(), length + 1);
            path.resize(length);
            paths.push_back(std::move(path));
        }
        DragFinish(drop);
        Select(window, std::move(paths));
        return 0;
    }
    case WorkComplete: {
        std::unique_ptr<Result> completed;
        { std::lock_guard guard(resultMutex); completed = std::move(result); }
        if (worker.joinable()) worker.join();
        busy = false;
        if (closing) { DestroyWindow(window); return 0; }
        if (completed) {
            report = std::move(completed->report);
            status = report.errors || report.sharingBlocks ? L"Diagnóstico con observaciones" : report.actionable() ? L"Hay elementos para desbloquear" : L"Análisis completado";
            if (report.directoriesInUse) status = L"Carpeta en uso: revisá la lista de procesos";
            SetWindowTextW(diagnosis, (completed->outcome + unlock::Describe(report)).c_str());
            if (administrator && report.needsAdmin) {
                SetWindowTextW(diagnosis, (completed->outcome + L"Ya estás como administrador. Los errores de permisos restantes requieren revisar los permisos del archivo o la protección del sistema.\r\n\r\n" + unlock::Describe(report)).c_str());
            }
            SendMessageW(diagnosis, EM_SETSEL, 0, 0);
        }
        Update(window);
        if (report.needsAdmin && !administrator && !adminPromptShown) {
            adminPromptShown = true;
            if (MessageBoxW(window, L"Faltan permisos para completar las comprobaciones o el desbloqueo de algunos elementos.\r\n\r\n¿Querés reiniciar AppUnlock como administrador y volver a analizar esta selección? Windows te pedirá autorización.",
                L"Se necesitan permisos", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) Elevate(window);
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case PickButton: Pick(window); break;
        case UnlockButton: RequestUnlock(window); break;
        case RescanButton: StartWork(window, false); break;
        case AdminButton: Elevate(window); break;
        }
        return 0;
    case WM_DRAWITEM: DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam)); return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wParam), Foreground);
        SetBkColor(reinterpret_cast<HDC>(wParam), Panel);
        return reinterpret_cast<LRESULT>(panelBrush);
    case WM_ERASEBKGND: return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        RECT client; GetClientRect(window, &client);
        FillRect(dc, &client, backgroundBrush);
        Text(dc, L"AppUnlock", Box(24, 20, 250, 49), bodyFont, Foreground, DT_VCENTER | DT_SINGLELINE);
        Text(dc, administrator ? L"ADMINISTRADOR · SIEMPRE VISIBLE" : L"SIEMPRE VISIBLE", Box(210, 20, 476, 49), smallFont, Accent, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        Text(dc, selectedLabel, Box(24, 230, 476, 252), smallFont, Muted, DT_PATH_ELLIPSIS | DT_SINGLELINE);
        Text(dc, status, Box(24, 257, 476, 280), bodyFont, Foreground, DT_END_ELLIPSIS | DT_SINGLELINE);
        RECT panel = Box(24, 280, 476, 506); FillRect(dc, &panel, panelBrush);
        if (!report.needsAdmin || administrator)
            Text(dc, L"Se aplican cambios solo al pulsar Desbloquear.", Box(24, 582, 476, 608), smallFont, Muted, DT_CENTER | DT_SINGLELINE);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DPICHANGED: {
        dpi = HIWORD(wParam); Fonts(); Layout();
        RECT* rect = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window, HWND_TOPMOST, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOACTIVATE);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case WM_CLOSE:
        if (busy) {
            closing = true; worker.request_stop();
            status = L"Cancelando…"; Update(window);
        } else DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        DragAcceptFiles(window, FALSE);
        PostQuitMessage(0);
        return 0;
    default: return DefWindowProcW(window, message, wParam, lParam);
    }
}
}
int APIENTRY wWinMain(HINSTANCE current, HINSTANCE, LPWSTR, int show) {
    instance = current;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    administrator = unlock::IsAdministrator();
    WNDCLASSEXW cls{ sizeof(cls) };
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPUNLOCK));
    cls.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SMALL));
    cls.lpszClassName = WindowClass;
    if (!RegisterClassExW(&cls)) return 1;
    dpi = GetDpiForSystem();
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT rect = Box(0, 0, 500, 628);
    AdjustWindowRectExForDpi(&rect, style, FALSE, WS_EX_TOPMOST, dpi);
    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    HWND window = CreateWindowExW(WS_EX_TOPMOST, WindowClass, APP_WINDOW_TITLE, style,
        work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2,
        width, height, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, show);
    UpdateWindow(window);
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::wstring> paths;
    if (arguments) {
        for (int i = 1; i < count; ++i) paths.emplace_back(arguments[i]);
        LocalFree(arguments);
    }
    Select(window, std::move(paths));
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (worker.joinable()) { worker.request_stop(); worker.join(); }
    DeleteObject(bodyFont); DeleteObject(titleFont); DeleteObject(smallFont);
    DeleteObject(backgroundBrush); DeleteObject(panelBrush);
    if (SUCCEEDED(com)) CoUninitialize();
    return static_cast<int>(message.wParam);
}
