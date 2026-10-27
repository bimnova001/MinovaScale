#include "gui_app.h"

#include <windows.h>
#include <commctrl.h>

#include "capture_app.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32")

namespace gui {

namespace {

// control IDs
enum {
    ID_LIST        = 101,
    ID_REFRESH     = 102,
    ID_COMBO_OUT   = 103,
    ID_COMBO_FORCE = 105,
    ID_SLIDER      = 107,
    ID_SHARP_TXT   = 108,
    ID_CHK_OVERLAY = 109,
    ID_CHK_UNCAP   = 110,
    ID_BTN_START   = 111,
    ID_BTN_STOP    = 112,
    ID_EDIT_LOG    = 113,
    ID_STATIC_FPS  = 114,
};

const wchar_t kGuiClass[] = L"MinovaUpscalerGui";

const UINT WM_APP_LINE       = WM_APP + 1;   // one stdout line (UTF-8, heap)
const UINT WM_APP_CHILD_EXIT = WM_APP + 2;

HWND       gHwnd       = nullptr;
HFONT      gFont       = nullptr;
HANDLE     gChild      = nullptr;
HANDLE     gChildPipe  = nullptr;
HANDLE     gReader     = nullptr;
std::vector<capture::WindowEntry> gEntries;

HWND ctrl(int id) { return GetDlgItem(gHwnd, id); }

HWND makeCtrl(const wchar_t* cls, const wchar_t* text, DWORD style, DWORD ex,
              int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, gHwnd, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)gFont, TRUE);
    return c;
}

void appendLog(const wchar_t* text)
{
    HWND e = ctrl(ID_EDIT_LOG);
    int len  = GetWindowTextLengthW(e);
    // keep the log bounded
    if (len > 12000) {
        SendMessageW(e, EM_SETSEL, 0, len - 12000);
        SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(e);
    }
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
}

void updateSharpLabel()
{
    int pos = (int)SendMessageW(ctrl(ID_SLIDER), TBM_GETPOS, 0, 0);
    wchar_t t[32];
    std::swprintf(t, 32, L"%.2f stops", pos / 100.0);
    SetWindowTextW(ctrl(ID_SHARP_TXT), t);
}

bool parseWH(HWND combo, unsigned* wOut, unsigned* hOut)
{
    wchar_t txt[64] = {};
    GetWindowTextW(combo, txt, 64);
    unsigned w = 0, h = 0;
    if (std::swscanf(txt, L"%ux%u", &w, &h) != 2) {
        if (!w && !h) {
            const wchar_t* p = txt;
            // locale-free parse of "WxH" when %u fails on the Thai label path
            wchar_t* end = nullptr;
            w = (unsigned)std::wcstol(p, &end, 10);
            if (end && *end == L'x')
                h = (unsigned)std::wcstol(end + 1, nullptr, 10);
        }
        if (!w || !h) return false;
    }
    *wOut = w;
    *hOut = h;
    return true;
}

void stopCapture()
{
    if (gChild) {
        TerminateProcess(gChild, 0);
        WaitForSingleObject(gChild, 2000);
        CloseHandle(gChild);
        gChild = nullptr;
    }
    // pipe/reader cleanup happens in WM_APP_CHILD_EXIT (reader sees EOF)
    EnableWindow(ctrl(ID_BTN_START), TRUE);
    EnableWindow(ctrl(ID_BTN_STOP), FALSE);
}

void startCapture()
{
    const int sel = (int)SendMessageW(ctrl(ID_LIST), LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || sel >= (int)gEntries.size()) {
        appendLog(L"!! เลือกหน้าต่างเกมก่อน (1)");
        return;
    }

    unsigned outW = 1920, outH = 1080;
    if (!parseWH(ctrl(ID_COMBO_OUT), &outW, &outH)) {
        appendLog(L"!! output resolution ไม่ถูกต้อง");
        return;
    }

    unsigned fw = 0, fh = 0;
    wchar_t forceTxt[64] = {};
    GetWindowTextW(ctrl(ID_COMBO_FORCE), forceTxt, 64);
    bool force = false;
    unsigned tw = 0, th = 0;
    if (parseWH(ctrl(ID_COMBO_FORCE), &tw, &th)) {
        force = true;
        fw = tw;
        fh = th;
    }
    else {
        force = false;   // "ไม่บังคับ" or anything invalid = keep game size
        forceTxt[0] = L'\0';
    }

    const int sharpPos = (int)SendMessageW(ctrl(ID_SLIDER), TBM_GETPOS, 0, 0);
    const bool overlay =
        SendMessageW(ctrl(ID_CHK_OVERLAY), BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool uncapped =
        SendMessageW(ctrl(ID_CHK_UNCAP), BM_GETCHECK, 0, 0) == BST_CHECKED;

    wchar_t exe[MAX_PATH + 2] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);

    // child = the same exe in CLI mode; args assembled from the UI state
    std::wstring cmd = L"\"";
    cmd += exe;
    cmd += L"\" --pid ";
    cmd += std::to_wstring(gEntries[sel].pid);
    cmd += L" --out " + std::to_wstring(outW) + L" " + std::to_wstring(outH);
    if (force)
        cmd += L" --force-res " + std::to_wstring(fw) + L" " +
               std::to_wstring(fh);
    cmd += L" --sharpness " + std::to_wstring(double(sharpPos) / 100.0);
    if (overlay)  cmd += L" --overlay";
    if (uncapped) cmd += L" --novsync";

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        appendLog(L"!! สร้าง pipe ไม่สำเร็จ");
        return;
    }
    // only the write end is inherited by the child
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb           = sizeof(si);
    si.dwFlags      = STARTF_USESTDHANDLES;
    si.hStdOutput   = wr;
    si.hStdError    = wr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                        0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        appendLog(L"!! เริ่ม child capture ไม่สำเร็จ");
        return;
    }
    CloseHandle(wr);
    CloseHandle(pi.hThread);
    gChild     = pi.hProcess;
    gChildPipe = rd;

    EnableWindow(ctrl(ID_BTN_START), FALSE);
    EnableWindow(ctrl(ID_BTN_STOP), TRUE);
    appendLog(cmd.c_str());

    gReader = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        char buf[2048];
        DWORD n = 0;
        std::string pending;
        for (;;) {
            if (!ReadFile(gChildPipe, buf, sizeof(buf) - 1, &n, nullptr) ||
                n == 0)
                break;
            buf[n] = '\0';
            pending.append(buf, n);
            size_t pos;
            while ((pos = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, pos);
                pending.erase(0, pos + 1);
                if (!line.empty()) {
                    char* heap = _strdup(line.c_str());  // freed by GUI thread
                    if (heap)
                        PostMessageW(gHwnd, WM_APP_LINE, 0, (LPARAM)heap);
                }
            }
        }
        PostMessageW(gHwnd, WM_APP_CHILD_EXIT, 0, 0);
        return 0;
    }, nullptr, 0, nullptr);
}

void refreshList()
{
    SendMessageW(ctrl(ID_LIST), LB_RESETCONTENT, 0, 0);
    gEntries = capture::listCaptureTargets();
    for (const auto& e : gEntries)
        SendMessageW(ctrl(ID_LIST), LB_ADDSTRING, 0,
                     (LPARAM)e.title.c_str());
    wchar_t t[64];
    std::swprintf(t, 64, L"1) เลือกหน้าต่างเกม (%lu)",
                  (unsigned long)gEntries.size());
    SetWindowTextW(ctrl(ID_REFRESH), t);
}

LRESULT CALLBACK guiWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DESTROY:
        stopCapture();
        if (gReader) {
            TerminateThread(gReader, 0);
            CloseHandle(gReader);
            gReader = nullptr;
        }
        if (gChildPipe) { CloseHandle(gChildPipe); gChildPipe = nullptr; }
        PostQuitMessage(0);
        return 0;

    case WM_HSCROLL:
        if ((HWND)lp == ctrl(ID_SLIDER)) updateSharpLabel();
        return 0;

    case WM_COMMAND:
        switch (HIWORD(wp)) {
        case LBN_DBLCLK: startCapture(); return 0;
        default: break;
        }
        switch (LOWORD(wp)) {
        case ID_REFRESH: refreshList(); return 0;
        case ID_BTN_START: startCapture(); return 0;
        case ID_BTN_STOP: stopCapture(); return 0;
        default: return 0;
        }

    case WM_APP_LINE: {
        const char* line = reinterpret_cast<const char*>(lp);
        if (!line) return 0;
        int len = MultiByteToWideChar(CP_UTF8, 0, line, -1, nullptr, 0);
        std::wstring w(size_t(len > 0 ? len - 1 : 0), L'\0');
        if (len > 0)
            MultiByteToWideChar(CP_UTF8, 0, line, -1, w.data(), len);
        std::free((void*)line);

        if (w.compare(0, 6, L"[fps] ") == 0) {
            const double fps = std::wcstod(w.c_str() + 6, nullptr);
            wchar_t t[64];
            std::swprintf(t, 64, L"%.0f fps", fps);
            SetWindowTextW(ctrl(ID_STATIC_FPS), t);
        }
        appendLog(w.c_str());
        return 0;
    }

    case WM_APP_CHILD_EXIT:
        if (gChild) { CloseHandle(gChild); gChild = nullptr; }
        if (gChildPipe) {
            CloseHandle(gChildPipe);
            gChildPipe = nullptr;
        }
        gReader = nullptr;
        EnableWindow(ctrl(ID_BTN_START), TRUE);
        EnableWindow(ctrl(ID_BTN_STOP), FALSE);
        appendLog(L"--- จบการทำงาน ---");
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace

int runGui()
{
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wcx{};
    wcx.cbSize        = sizeof(wcx);
    wcx.style         = CS_HREDRAW | CS_VREDRAW;
    wcx.lpfnWndProc   = guiWndProc;
    wcx.hInstance     = GetModuleHandleW(nullptr);
    wcx.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wcx.lpszClassName = kGuiClass;
    if (!RegisterClassExW(&wcx)) return 1;

    // client area 640x560
    RECT r{0, 0, 640, 560};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    gHwnd = CreateWindowExW(
        0, kGuiClass, L"MinovaUpscaler",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, wcx.hInstance, nullptr);
    if (!gHwnd) return 1;

    gFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    // 1) window picker
    makeCtrl(L"STATIC", L"1) เลือกหน้าต่างเกม:", 0, 0, 10, 10, 200, 20, 0);
    makeCtrl(L"LISTBOX", nullptr,
             WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS, 0,
             10, 34, 500, 170, ID_LIST);
    makeCtrl(L"BUTTON", L"รีเฟรช", BS_PUSHBUTTON, 0,
             520, 34, 110, 28, ID_REFRESH);

    // 2) resolutions
    makeCtrl(L"STATIC", L"2) ความละเอียด output:", 0, 0,
             10, 214, 170, 20, 0);
    HWND cbOut = makeCtrl(L"COMBOBOX", nullptr,
                          CBS_DROPDOWNLIST | WS_VSCROLL, 0,
                          180, 210, 150, 200, ID_COMBO_OUT);
    for (const wchar_t* s : {L"1920x1080", L"1600x900", L"1280x720"})
        SendMessageW(cbOut, CB_ADDSTRING, 0, (LPARAM)s);
    SendMessageW(cbOut, CB_SETCURSEL, 0, 0);

    makeCtrl(L"STATIC", L"3) บังคับเกม render ที่:", 0, 0,
             350, 214, 150, 20, 0);
    HWND cbForce = makeCtrl(L"COMBOBOX", nullptr,
                            CBS_DROPDOWNLIST | WS_VSCROLL, 0,
                            500, 210, 130, 200, ID_COMBO_FORCE);
    for (const wchar_t* s : {L"ไม่บังคับ", L"1280x720", L"960x540",
                             L"640x360"})
        SendMessageW(cbForce, CB_ADDSTRING, 0, (LPARAM)s);
    SendMessageW(cbForce, CB_SETCURSEL, 0, 0);

    // 3) sharpness
    makeCtrl(L"STATIC", L"4) ความคม (RCAS):", 0, 0, 10, 246, 140, 20, 0);
    HWND sl = makeCtrl(L"msctls_trackbar32", nullptr,
                       TBS_HORZ | TBS_TRANSPARENTBKGND, 0,
                       150, 240, 200, 28, ID_SLIDER);
    SendMessageW(sl, TBM_SETRANGE, TRUE, MAKELPARAM(0, 60));
    SendMessageW(sl, TBM_SETPOS, TRUE, 20);
    makeCtrl(L"STATIC", L"0.20 stops", 0, 0, 360, 246, 100, 20, ID_SHARP_TXT);
    updateSharpLabel();

    // 4) mode checkboxes
    makeCtrl(L"BUTTON", L"Overlay: แสดงบนจอโดยตรงทับเกม (ต้องมี WGC)",
             BS_AUTOCHECKBOX, 0, 10, 280, 420, 22, ID_CHK_OVERLAY);
    makeCtrl(L"BUTTON", L"ไม่จำกัด FPS (Present 0) — โหมดเทียบผล",
             BS_AUTOCHECKBOX, 0, 10, 306, 420, 22, ID_CHK_UNCAP);

    // 5) run controls + fps
    makeCtrl(L"BUTTON", L"เริ่ม (Start)", BS_PUSHBUTTON, 0,
             10, 340, 140, 32, ID_BTN_START);
    makeCtrl(L"BUTTON", L"หยุด (Stop)", BS_PUSHBUTTON, 0,
             160, 340, 140, 32, ID_BTN_STOP);
    HWND fps = makeCtrl(L"STATIC", L"— fps", SS_RIGHT | SS_CENTERIMAGE, 0,
                        500, 338, 130, 36, ID_STATIC_FPS);
    HFONT big = CreateFontW(-26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH, L"Segoe UI");
    SendMessageW(fps, WM_SETFONT, (WPARAM)big, TRUE);

    // 6) log
    makeCtrl(L"STATIC", L"บันทึก:", 0, 0, 10, 384, 100, 20, 0);
    makeCtrl(L"EDIT", nullptr,
             ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0,
             10, 406, 620, 144, ID_EDIT_LOG);

    refreshList();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

} // namespace gui
