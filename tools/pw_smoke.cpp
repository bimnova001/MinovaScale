// tools/pw_smoke.cpp — PrintWindow(PW_RENDERFULLCONTENT) repeatability test
// against a live D3D flip-model target window (usage: pw_smoke <pid>).

#include <windows.h>
#include <cstdio>
#include <cstdlib>

namespace {

struct PidCtx { DWORD pid = 0; HWND match = nullptr; };

BOOL CALLBACK enumByPid(HWND hwnd, LPARAM lp)
{
    auto* p = reinterpret_cast<PidCtx*>(lp);
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid == p->pid && IsWindowVisible(hwnd)) { p->match = hwnd; return FALSE; }
    return TRUE;
}

} // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (argc < 2) { std::fprintf(stderr, "usage: pw_smoke <pid>\n"); return 1; }
    PidCtx c{static_cast<DWORD>(std::strtoul(argv[1], nullptr, 0)), nullptr};
    EnumWindows(enumByPid, reinterpret_cast<LPARAM>(&c));
    if (!c.match) { std::fprintf(stderr, "no window for pid\n"); return 1; }
    wchar_t t[256] = {};
    GetWindowTextW(c.match, t, 256);
    std::fprintf(stderr, "target: %ls\n", t);

    RECT r{};
    GetClientRect(c.match, &r);
    const unsigned w = static_cast<unsigned>(r.right);
    const unsigned h = static_cast<unsigned>(r.bottom);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = LONG(w);
    bi.bmiHeader.biHeight      = -LONG(h);
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC memDc = CreateCompatibleDC(nullptr);
    SelectObject(memDc, bmp);

    for (int i = 0; i < 10; ++i) {
        LARGE_INTEGER t0{}, t1{}, f{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        BOOL ok = PrintWindow(c.match, memDc, 0x2 /*PW_RENDERFULLCONTENT*/);
        QueryPerformanceCounter(&t1);
        const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0
                        / double(f.QuadPart);
        unsigned white = 0, black = 0;
        if (ok && bits) {
            for (unsigned y = 0; y < h; y += 11) {
                const BYTE* row = static_cast<const BYTE*>(bits) + size_t(y) * w * 4;
                for (unsigned x = 0; x < w; x += 11) {
                    const BYTE* p = row + size_t(x) * 4;
                    if (p[0] > 240 && p[1] > 240 && p[2] > 240) ++white;
                    if (p[0] < 16 && p[1] < 16 && p[2] < 16) ++black;
                }
            }
        }
        std::fprintf(stderr, "#%d ok=%d %.1f ms white=%u black=%u (Gle=%lu)\n",
                     i, int(ok), ms, white, black,
                     (unsigned long)GetLastError());
        Sleep(100);
    }
    DeleteDC(memDc);
    DeleteObject(bmp);
    return 0;
}
