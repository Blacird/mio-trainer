/*
 * MIO_Trainer.c —— MIO: Memories in Orbit 图形化修改器
 *
 * 目标游戏版本：pc-ship/patch3-rc3/r21756（Steam 发行版）
 * 实现：原生 Win32 + GDI 手绘，无任何第三方运行时依赖。
 * 说明：本程序主要由 AI Agent 辅助开发，可能含未被察觉的缺陷，使用前请自行审查代码。
 *
 * ─── 功能 ───────────────────────────────────────────────────────────
 *   · 无限生命 / 无视地形伤害 / 无限能量 / 无限跳跃 / 万物可钩 / 调试模式
 *   · 鼠标点击开关或对应热键（F1/F2/F4/F5/F6/F8）切换
 *   · 自动连接运行中的 mio.exe，实时显示生命/能量/跳跃/坐标
 *
 * ─── 硬性约束（修改本文件时必须遵守） ───────────────────────────────
 *   1. 产物为单一 exe，可直接分发运行；
 *      导入表只允许：KERNEL32 / USER32 / GDI32 / PSAPI / msvcrt
 *      （禁止引入 libwinpthread / libgcc_s / libstdc++、Python 或配置文件）。
 *   2. 绝不读写任何游戏存档文件（slot_*.save 等）。
 *   3. 只做内存读写，不向游戏进程注入代码。
 *
 * ─── 编译 ───────────────────────────────────────────────────────────
 *   windres MIO_Trainer.rc -o MIO_Trainer.res.o
 *   gcc -O2 -s -mwindows -o MIO_Trainer.exe MIO_Trainer.c MIO_Trainer.res.o \
 *       -lpsapi -lgdi32 -luser32
 *
 * ─── 目录 ───────────────────────────────────────────────────────────
 *   1. 目标地址          —— 游戏内静态全局的 RVA 与字段偏移
 *   2. 界面常量          —— 基准尺寸、配色
 *   3. 功能表 g_feat[]   —— 所有可切换功能的数据声明（扩展点）
 *   4. 运行状态          —— 进程句柄、实时数值、布局缓存
 *   5. 日志              —— 排错用的运行日志（写到 exe 同目录 MIO_Trainer.log）
 *   6. 进程 / 内存       —— 附着、读写
 *   7. 应用修改          —— 把开启的功能写回游戏内存
 *   8. 实时读取          —— 拉取生命/能量/跳跃/坐标用于显示
 *   9. 绘图              —— GDI 控件与图标
 *  10. 窗口过程 / 入口   —— 消息分发、缩放、热键
 */
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
#include <math.h>


/* ===================================================================
 * 1. 目标地址
 *
 *   游戏使用自研 Flamby 引擎；exe 保留完整 RTTI，未做商业加固。
 *   以下 RVA 为静态全局对象在模块内的偏移，运行时地址 = 基址 + RVA。
 *   ASLR 由模块基址自动补偿，无需手动重定位。
 *
 *   Metagame::State  @ 0x10E1C88
 *       +0x04 debug_enabled   开发者叠加层开关
 *       +0x05 debug_allowed   调试许可（由密钥/柯南码置位，本程序只读 +0x04）
 *
 *   Mio（主角本体）@ 0x10EFC50
 *       +0x002F8  transform 指针（ptr+0x18/0x1C/0x20 = x/y/z，渲染坐标）
 *       +0x00074  跳跃上限 u32 (=2)        +0x00B9C  剩余跳跃 u32
 *       +0x00784  生命上限                 +0x00790  生命当前
 *       +0x21D58  能量当前 f32             +0x21D5C  能量上限 u32 (=100)
 *       +0x22980  Combat invulnerable      +0x22981  Platforming invulnerable
 *       +0x2298F  Force no recharge（必须维持 0，置 1 会导致地面无法回能）
 *       +0x22990  Everything hookable
 * =================================================================== */

#define RVA_METAGAME          0x10E1C88u
#define OFF_DEBUG_ENABLED     0x04

#define RVA_MIO               0x10EFC50u
#define OFF_TRANSFORM         0x2F8u
#define OFF_JUMP_MAX          0x074u
#define OFF_JUMP_LEFT         0xB9Cu
#define OFF_LIFE_D            0x790u
#define OFF_LIFE_E            0x784u
#define OFF_CHARGE_CUR        0x21D58u
#define OFF_CHARGE_MAX        0x21D5Cu
#define OFF_REINVULN_COMBAT   0x22980u
#define OFF_REINVULN_PLATFORM 0x22981u
#define OFF_FORCE_NO_RECHARGE 0x2298Fu
#define OFF_HOOKABLE          0x22990u


/* ===================================================================
 * 2. 界面常量
 * =================================================================== */

/* 设计基准尺寸：所有布局按此设计，再用 sx()/sy() 缩放到当前窗口 */
#define BASE_W       470
#define BASE_H       452

/* 配色：取自游戏封面 SplashScreen 的深海洋蓝 + 金色能量线 */
#define C_BG          RGB(16,  51,  79)    /* 海蓝背景 */
#define C_HEADER      RGB(23,  63,  99)    /* 标题栏 */
#define C_ROW_OFF     RGB(19,  56,  90)    /* 功能行（关） */
#define C_ROW_ON      RGB(28,  78,  122)   /* 功能行（开） */
#define C_ACCENT      RGB(240, 198, 116)   /* 金色，主强调色 */
#define C_PIN_ON      RGB(10,  34,  58)    /* 置顶开启：深蓝 */
#define C_TEXT        RGB(230, 243, 242)   /* 主文字 */
#define C_TEXT_DIM    RGB(140, 175, 195)   /* 次级文字 */
#define C_BADGE_BG    RGB(15,  46,  75)    /* 徽章底 */
#define C_BADGE_BD    RGB(50,  101, 144)   /* 徽章边 */
#define C_SWITCH_OFF  RGB(65,  101, 127)   /* 开关轨道（关） */
#define C_KNOB        RGB(230, 243, 242)   /* 开关球 */
#define C_LINE        RGB(40,  86,  126)   /* 分隔线 */
#define C_WARN        RGB(224, 122,  95)   /* 警告橙红 */
#define C_OK          RGB(240, 198, 116)   /* 成功金（与主强调色一致） */


/* ===================================================================
 * 3. 功能表
 *
 *   功能分三种行为类型：
 *     KIND_FLAG    —— 写一个字节标志位到 rva（值为 val）
 *     KIND_CHARGE  —— 每帧把能量冻结到上限（rva/val 不用）
 *     KIND_JUMP    —— 每帧把剩余跳跃次数补满到上限（rva/val 不用）
 *
 *   ── 扩展指南 ──────────────────────────────────────────────────────
 *   · 新增一个标志位功能：在 g_feat[] 里加一行，
 *       { L"显示名", L"热键", VK_xx, KIND_FLAG, RVA_XXX + 偏移, 目标值, 0 }
 *   · 新增一种新的行为类型：
 *       ① 在下面 enum 加一个 KIND_XXX；
 *       ② 在 apply_all() 的 switch 里加对应 case；
 *       ③ 如需在状态栏显示，在 read_live() 里读取。
 * =================================================================== */

enum { KIND_FLAG = 0, KIND_CHARGE, KIND_JUMP };

typedef struct {
    const wchar_t *name;     /* 显示名 */
    const wchar_t *hk;       /* 热键徽章文字 */
    int            vk;       /* 虚拟键码 */
    int            kind;     /* 行为类型 */
    unsigned       rva;      /* 目标 RVA（KIND_FLAG 用） */
    unsigned char  val;      /* 写入值（KIND_FLAG 用） */
    int            on;       /* 当前是否开启 */
} Feature;

static Feature g_feat[] = {
    { L"无限生命",       L"F1", VK_F1, KIND_FLAG,   RVA_MIO + OFF_REINVULN_COMBAT,    1, 0 },
    { L"无视地形伤害",   L"F2", VK_F2, KIND_FLAG,   RVA_MIO + OFF_REINVULN_PLATFORM,  1, 0 },
    { L"无限能量",       L"F4", VK_F4, KIND_CHARGE, 0, 0, 0 },
    { L"无限跳跃",       L"F5", VK_F5, KIND_JUMP,   0, 0, 0 },
    { L"万物可钩",       L"F6", VK_F6, KIND_FLAG,   RVA_MIO + OFF_HOOKABLE,           1, 0 },
    { L"调试模式",       L"F8", VK_F8, KIND_FLAG,   RVA_METAGAME + OFF_DEBUG_ENABLED, 1, 0 },
};
#define NFEAT ((int)(sizeof(g_feat) / sizeof(g_feat[0])))


/* ===================================================================
 * 4. 运行状态
 * =================================================================== */

static HWND     g_hwnd = NULL;
static HANDLE   g_proc = NULL;          /* 游戏进程句柄 */
static DWORD    g_pid  = 0;
static ULONG64  g_base = 0;             /* mio.exe 模块基址 */
static int      g_attached = 0;
static int      g_status = 0;           /* 0=未连接 1=已连接 2=权限不足 */
static wchar_t  g_statusText[256];
static int      g_tick = 0;

/* 实时数值（用于状态栏显示） */
static int      g_charge = 0, g_chargeMax = 0;
static int      g_jump = 0,    g_jumpMax = 0;
static int      g_hp = 0,      g_hpMax = 0;
static float    g_pos[3] = { 0, 0, 0 };
static int      g_posValid = 0;

/* 窗口与缩放 */
static int      g_w = BASE_W, g_h = BASE_H;
static float    g_sw = 1.0f, g_sh = 1.0f;
static int      g_pinned = 1;            /* 置顶状态 */

/* GDI 资源（WM_SIZE 时重建） */
static HFONT    g_fTitle, g_fItem, g_fSmall, g_fTiny;
static HBRUSH   g_brBg, g_brHeader, g_brRowOff, g_brRowOn;

/* 命中区域缓存（paint 时写入，鼠标交互时读取） */
static RECT     g_rcClose, g_rcMin, g_rcPin;
static RECT     g_rcAll, g_rcRow[NFEAT];

/* 缩放辅助：把基准坐标按当前窗口尺寸映射 */
static int sx(int x) { return (int)(x * g_sw); }
static int sy(int y) { return (int)(y * g_sh); }
static int sclamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}


/* ===================================================================
 * 5. 日志
 *
 *   运行日志写到 exe 同目录的 MIO_Trainer.log，便于排查问题。
 *   · 启动时若日志 > 256KB 会清空，避免无限增长。
 *   · 仅在关键事件（连接/分离/切换/读写失败/关闭）写一行，不逐帧刷屏。
 *   · 排错时把这份 log 一并发给维护者即可。
 * =================================================================== */

static char g_logPath[MAX_PATH] = "";

static void log_init(void)
{
    GetModuleFileNameA(NULL, g_logPath, MAX_PATH);
    char *p = strrchr(g_logPath, '\\');
    if (p) p[1] = 0; else g_logPath[0] = 0;
    strcat(g_logPath, "MIO_Trainer.log");

    FILE *f = fopen(g_logPath, "rb");
    if (f) { fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f);
             if (sz > 256 * 1024) remove(g_logPath); }
}

static void log_line(const char *fmt, ...)
{
    FILE *f = fopen(g_logPath, "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d.%03d  ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* 把宽字符功能名转成 UTF-8 字符串供日志使用 */
static void wname_to_utf8(const wchar_t *w, char *out, int outlen)
{
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outlen, NULL, NULL);
}


/* ===================================================================
 * 6. 进程 / 内存
 * =================================================================== */

/* 按可执行文件名查找进程 PID（用 Toolhelp，兼容无 EnumProcessModules 权限的场景） */
static DWORD find_process(const char *exe)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe; memset(&pe, 0, sizeof(pe)); pe.dwSize = sizeof(pe);
    DWORD found = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            char name[MAX_PATH];
            WideCharToMultiByte(CP_ACP, 0, pe.szExeFile, -1, name, sizeof(name), NULL, NULL);
            if (_stricmp(name, exe) == 0) { found = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

/* 取进程主模块基址 */
static ULONG64 module_base(HANDLE proc)
{
    HMODULE mods[256]; DWORD need = 0;
    if (!EnumProcessModules(proc, mods, sizeof(mods), &need)) return 0;
    MODULEINFO mi;
    if (!GetModuleInformation(proc, mods[0], &mi, sizeof(mi))) return 0;
    return (ULONG64)(ULONG_PTR)mi.lpBaseOfDll;
}

static BOOL rd(ULONG64 addr, void *buf, SIZE_T size)
{
    SIZE_T got = 0;
    return ReadProcessMemory(g_proc, (LPCVOID)(ULONG_PTR)addr, buf, size, &got) && got == size;
}

static BOOL wr(ULONG64 addr, const void *buf, SIZE_T size)
{
    DWORD old = 0;
    VirtualProtectEx(g_proc, (LPVOID)(ULONG_PTR)addr, size, PAGE_EXECUTE_READWRITE, &old);
    SIZE_T put = 0;
    BOOL ok = WriteProcessMemory(g_proc, (LPVOID)(ULONG_PTR)addr, buf, size, &put);
    VirtualProtectEx(g_proc, (LPVOID)(ULONG_PTR)addr, size, old, &old);
    return ok && put == size;
}

static void detach(void)
{
    if (g_proc) { CloseHandle(g_proc); g_proc = NULL; }
    g_attached = 0; g_pid = 0; g_base = 0;
    g_charge = g_chargeMax = g_jump = g_jumpMax = g_hp = g_hpMax = 0;
    g_posValid = 0;
}

/* 周期性尝试连接 mio.exe；进程消失时自动分离 */
static void try_attach(void)
{
    DWORD pid = find_process("mio.exe");
    if (!pid) {
        if (g_attached) { log_line("游戏进程已退出，自动分离"); detach(); }
        g_status = 0;
        swprintf(g_statusText, 256, L"等待游戏启动…… 未检测到 mio.exe");
        return;
    }
    if (g_proc && pid == g_pid) { g_status = 1; return; }
    if (g_proc) { CloseHandle(g_proc); g_proc = NULL; }
    g_proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!g_proc)
        g_proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                             PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
    if (!g_proc) {
        g_attached = 0; g_status = 2;
        DWORD err = GetLastError();
        log_line("OpenProcess 失败 err=%lu PID=%u", err, pid);
        swprintf(g_statusText, 256, L"无法附加（请右键以管理员身份运行）");
        return;
    }
    g_pid = pid;
    g_base = module_base(g_proc);
    if (!g_base) {
        CloseHandle(g_proc); g_proc = NULL; g_attached = 0; g_status = 0;
        log_line("无法读取模块基址 PID=%u", pid);
        swprintf(g_statusText, 256, L"等待游戏启动……");
        return;
    }
    g_attached = 1; g_status = 1;
    log_line("已连接 mio.exe PID=%u 基址=0x%llX", pid, (unsigned long long)g_base);
    swprintf(g_statusText, 256, L"已连接 mio.exe   PID %u   基址 0x%llX",
             g_pid, (unsigned long long)g_base);
}


/* ===================================================================
 * 7. 应用修改
 * =================================================================== */

static void set_flag(Feature *f, int on)
{
    if (!g_attached) return;
    unsigned char v = on ? f->val : 0;
    if (!wr(g_base + f->rva, &v, 1)) {
        char nm[128]; wname_to_utf8(f->name, nm, sizeof(nm));
        log_line("写标志失败 rva=0x%X val=%d name=%s", f->rva, v, nm);
    }
}

/* 每帧调用：把所有已开启的功能写回游戏内存 */
static void apply_all(void)
{
    if (!g_attached) return;

    /* Force no recharge 必须始终为 0，否则地面无法回能 —— 保险性地清零 */
    { unsigned char z = 0; ULONG64 a = g_base + RVA_MIO + OFF_FORCE_NO_RECHARGE;
      unsigned char cur = 1; if (rd(a, &cur, 1) && cur) wr(a, &z, 1); }

    for (int i = 0; i < NFEAT; i++) {
        Feature *f = &g_feat[i];
        if (!f->on) continue;
        switch (f->kind) {

        case KIND_FLAG: {
            unsigned char cur = 0;
            ULONG64 a = g_base + f->rva;
            if (rd(a, &cur, 1) && cur != f->val) wr(a, &f->val, 1);
            break;
        }

        case KIND_CHARGE: {
            unsigned mx = 0;
            if (rd(g_base + RVA_MIO + OFF_CHARGE_MAX, &mx, 4) && mx > 0) {
                float want = (float)mx, cur = -1.0f;
                if (rd(g_base + RVA_MIO + OFF_CHARGE_CUR, &cur, 4) && cur != want)
                    wr(g_base + RVA_MIO + OFF_CHARGE_CUR, &want, 4);
            }
            break;
        }

        case KIND_JUMP: {
            unsigned mx = 0, cur = 0;
            if (rd(g_base + RVA_MIO + OFF_JUMP_MAX, &mx, 4) && mx > 0) {
                if (rd(g_base + RVA_MIO + OFF_JUMP_LEFT, &cur, 4) && cur < mx)
                    wr(g_base + RVA_MIO + OFF_JUMP_LEFT, &mx, 4);
            }
            break;
        }
        }
    }
}


/* ===================================================================
 * 8. 实时读取（仅用于状态栏显示）
 * =================================================================== */

static ULONG64 mio_transform(void)
{
    ULONG64 p = 0;
    if (!rd(g_base + RVA_MIO + OFF_TRANSFORM, &p, 8)) return 0;
    if (p < 0x10000 || p > 0x7FFFFFFFFFFFULL) return 0;
    return p;
}

static void read_live(void)
{
    if (!g_attached) return;
    float f = 0; unsigned u = 0;

    if (rd(g_base + RVA_MIO + OFF_CHARGE_CUR, &f, 4)) g_charge = (int)(f + 0.5f);
    if (rd(g_base + RVA_MIO + OFF_CHARGE_MAX, &u, 4)) g_chargeMax = (int)u;
    if (rd(g_base + RVA_MIO + OFF_JUMP_LEFT, &u, 4))  g_jump = (int)u;
    if (rd(g_base + RVA_MIO + OFF_JUMP_MAX, &u, 4))   g_jumpMax = (int)u;
    unsigned d = 0, e = 0;
    if (rd(g_base + RVA_MIO + OFF_LIFE_D, &d, 4)) g_hp = (int)d;
    if (rd(g_base + RVA_MIO + OFF_LIFE_E, &e, 4)) g_hpMax = (int)e;

    ULONG64 tr = mio_transform();
    g_posValid = 0;
    if (tr) {
        float v[3];
        if (rd(tr + 0x18, v, 12)) {
            /* 过滤 NaN 与离谱值 */
            if (v[0] == v[0] && v[1] == v[1] && v[2] == v[2] &&
                fabsf(v[0]) < 1e7f && fabsf(v[1]) < 1e7f) {
                memcpy(g_pos, v, sizeof(v));
                g_posValid = 1;
            }
        }
    }
}


/* ===================================================================
 * 9. 绘图
 * =================================================================== */

static void fill(HDC dc, RECT r, HBRUSH br) { FillRect(dc, &r, br); }

static void frame(HDC dc, RECT r, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(p);
}

static void text(HDC dc, int x, int y, const wchar_t *s, COLORREF c, HFONT f)
{
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    TextOutW(dc, x, y, s, (int)wcslen(s));
    SelectObject(dc, of);
}

static void text_r(HDC dc, RECT r, const wchar_t *s, COLORREF c, HFONT f, UINT fmt)
{
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, fmt);
    SelectObject(dc, of);
}

/* iOS 风开关 */
static void draw_toggle(HDC dc, int x, int y, int w, int h, int on)
{
    HBRUSH bg = CreateSolidBrush(on ? C_ACCENT : C_SWITCH_OFF);
    HPEN   pn = CreatePen(PS_SOLID, 1, on ? C_ACCENT : C_SWITCH_OFF);
    HGDIOBJ ob = SelectObject(dc, bg), op = SelectObject(dc, pn);
    RoundRect(dc, x, y, x + w, y + h, h, h);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(bg); DeleteObject(pn);

    int kr = h - sy(8);
    int kx = on ? (x + w - 4 - kr) : (x + 4);
    HBRUSH kb = CreateSolidBrush(C_KNOB);
    HPEN   kp = CreatePen(PS_SOLID, 1, C_KNOB);
    ob = SelectObject(dc, kb); op = SelectObject(dc, kp);
    Ellipse(dc, kx, y + sy(4), kx + kr, y + sy(4) + kr);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(kb); DeleteObject(kp);
}

/* 热键徽章 */
static void draw_badge(HDC dc, RECT r, const wchar_t *s, int on)
{
    HBRUSH bg = CreateSolidBrush(C_BADGE_BG);
    HGDIOBJ ob = SelectObject(dc, bg);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    int rad = sy(6); if (rad < 2) rad = 2;
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(bg);
    frame(dc, r, on ? C_ACCENT : C_BADGE_BD);
    text_r(dc, r, s, on ? C_ACCENT : C_TEXT_DIM, g_fSmall,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* 底部普通按钮 */
static void draw_btn(HDC dc, RECT r, const wchar_t *s, int hot)
{
    HBRUSH b = CreateSolidBrush(hot ? C_ROW_ON : C_BADGE_BG);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    int rad = sy(8); if (rad < 2) rad = 2;
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b);
    frame(dc, r, hot ? C_ACCENT : C_BADGE_BD);
    text_r(dc, r, s, hot ? C_TEXT : C_TEXT_DIM, g_fSmall,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* 标题栏小按钮（圆角底 + 文字） */
static void draw_header_btn(HDC dc, RECT r, const wchar_t *s, COLORREF fg)
{
    HGDIOBJ ob = SelectObject(dc, g_brHeader);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    int rad = sy(6); if (rad < 2) rad = 2;
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    frame(dc, r, C_LINE);
    text_r(dc, r, s, fg, g_fSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* 置顶图标：顶部横杠 + 向上箭头（几何取自一个 1024 视图的置顶 SVG）。
 * 状态切换只改线条颜色：置顶=深蓝，浮动=灰，形状完全一致。 */
static void draw_pin_icon(HDC dc, RECT r, COLORREF c)
{
    int w = r.right - r.left, h = r.bottom - r.top;
    int s = (w < h ? w : h);
    int pad = s / 8;                       /* 四周留白 */
    int box = s - pad * 2;
    if (box < 6) box = 6;
    float k = box / 550.0f;                /* 图标内容在 1024 视图中跨度约 550 */
    int cx = (r.left + r.right) / 2;
    int cy = (r.top + r.bottom) / 2;

    /* 1024 视图坐标 -> 屏幕坐标（内容中心约 513, 502） */
#define PX(v) (cx + (int)(((v) - 513.0f) * k + 0.5f))
#define PY(v) (cy + (int)(((v) - 502.0f) * k + 0.5f))

    int lw = (int)(45.0f * k + 0.5f);
    if (lw < 1) lw = 1;
    HPEN pen = CreatePen(PS_SOLID, lw, c);
    HGDIOBJ op = SelectObject(dc, pen);

    MoveToEx(dc, PX(260.6f), PY(283.5f), NULL);   /* 顶部横杠 */
    LineTo  (dc, PX(766.1f), PY(283.5f));
    MoveToEx(dc, PX(512.7f), PY(744.0f), NULL);   /* 箭杆 */
    LineTo  (dc, PX(512.7f), PY(400.0f));
    MoveToEx(dc, PX(512.7f), PY(400.0f), NULL);   /* 左臂 */
    LineTo  (dc, PX(375.0f), PY(505.0f));
    MoveToEx(dc, PX(512.7f), PY(400.0f), NULL);   /* 右臂 */
    LineTo  (dc, PX(650.0f), PY(505.0f));

    SelectObject(dc, op);
    DeleteObject(pen);
#undef PX
#undef PY
}

/* 重建字体与画刷（窗口尺寸变化时调用） */
static void update_resources(void)
{
    if (g_fTitle) DeleteObject(g_fTitle);
    if (g_fItem)  DeleteObject(g_fItem);
    if (g_fSmall) DeleteObject(g_fSmall);
    if (g_fTiny)  DeleteObject(g_fTiny);
    if (g_brBg)      DeleteObject(g_brBg);
    if (g_brHeader)  DeleteObject(g_brHeader);
    if (g_brRowOff)  DeleteObject(g_brRowOff);
    if (g_brRowOn)   DeleteObject(g_brRowOn);

    #define MAKE_FONT(dst, size, weight) \
        dst = CreateFontW(-sclamp(sy(size), 8, 48), 0, 0, 0, weight, 0, 0, 0, \
                          DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, \
                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI")
    MAKE_FONT(g_fTitle, 19, 600);
    MAKE_FONT(g_fItem,  14, 400);
    MAKE_FONT(g_fSmall, 12, 400);
    MAKE_FONT(g_fTiny,  10, 400);
    #undef MAKE_FONT

    g_brBg     = CreateSolidBrush(C_BG);
    g_brHeader = CreateSolidBrush(C_HEADER);
    g_brRowOff = CreateSolidBrush(C_ROW_OFF);
    g_brRowOn  = CreateSolidBrush(C_ROW_ON);
}

/* 功能列表页 */
static void paint_page_features(HDC dc)
{
    int headerH = sy(58);
    int rowTop  = headerH + sy(10);
    int rowH    = sy(40);
    int rowGap  = sy(6);
    int y = rowTop;

    for (int i = 0; i < NFEAT; i++) {
        Feature *f = &g_feat[i];
        RECT r = { sx(12), y, g_w - sx(12), y + rowH };
        g_rcRow[i] = r;
        fill(dc, r, f->on ? g_brRowOn : g_brRowOff);
        if (f->on) {
            RECT bar = { r.left, r.top, r.left + sx(3), r.bottom };
            HBRUSH b = CreateSolidBrush(C_ACCENT);
            fill(dc, bar, b); DeleteObject(b);
        }
        text(dc, r.left + sx(16), r.top + sy(10), f->name,
             f->on ? C_TEXT : C_TEXT_DIM, g_fItem);
        RECT bd = { r.right - sx(126), r.top + sy(10), r.right - sx(70), r.top + sy(30) };
        draw_badge(dc, bd, f->hk, f->on);
        draw_toggle(dc, r.right - sx(56), r.top + sy(9), sx(46), sy(22), f->on);
        y += rowH + rowGap;
    }

    /* 状态栏：生命 / 能量 / 跳跃 */
    wchar_t buf[256];
    if (g_attached && g_hpMax > 0)
        swprintf(buf, 256, L"生命 %d/%d      能量 %d/%d      跳跃 %d/%d",
                 g_hp, g_hpMax, g_charge, g_chargeMax, g_jump, g_jumpMax);
    else
        swprintf(buf, 256, L"生命 —/—      能量 —/—      跳跃 —/—");
    text(dc, sx(18), y + sy(4), buf, g_attached ? C_TEXT : C_TEXT_DIM, g_fSmall);

    /* 状态栏：坐标 */
    wchar_t buf2[256];
    if (g_posValid)
        swprintf(buf2, 256, L"坐标  X %.0f    Y %.0f    Z %.0f", g_pos[0], g_pos[1], g_pos[2]);
    else
        swprintf(buf2, 256, L"坐标  —— 未进入游戏 ——");
    text(dc, sx(18), y + sy(24), buf2, C_TEXT_DIM, g_fSmall);
}

/* 双缓冲整窗绘制 */
static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, g_w, g_h);
    HGDIOBJ ob = SelectObject(dc, bmp);

    RECT full = { 0, 0, g_w, g_h };
    fill(dc, full, g_brBg);

    /* 标题栏 */
    int headerH = sy(58);
    RECT hdr = { 0, 0, g_w, headerH };
    fill(dc, hdr, g_brHeader);
    { RECT a = { 0, 0, sx(4), headerH };              /* 连接状态指示条 */
      HBRUSH b = CreateSolidBrush(g_attached ? C_ACCENT : C_TEXT_DIM);
      fill(dc, a, b); DeleteObject(b); }
    text(dc, sx(18), sy(12), L"MIO 修改器", C_TEXT, g_fTitle);
    text(dc, sx(18), sy(36), L"Memories in Orbit · pc-ship/patch3-rc3/r21756",
         C_TEXT_DIM, g_fSmall);

    /* 标题栏右侧三个按钮：最小化 / 置顶 / 关闭 */
    int btnH = sy(26);
    int btnY = sy(16);
    int btnGap = sx(6);
    int marginR = sx(12);
    g_rcClose = (RECT){ g_w - marginR - sx(28), btnY, g_w - marginR, btnY + btnH };
    g_rcPin   = (RECT){ g_rcClose.left - sx(34) - btnGap, btnY,
                        g_rcClose.left - btnGap, btnY + btnH };
    g_rcMin   = (RECT){ g_rcPin.left - sx(34) - btnGap, btnY,
                        g_rcPin.left - btnGap, btnY + btnH };

    draw_header_btn(dc, g_rcMin,   L"—", C_TEXT_DIM);
    COLORREF pinFg = g_pinned ? C_PIN_ON : C_TEXT_DIM;
    draw_header_btn(dc, g_rcPin,   L"", pinFg);
    draw_pin_icon (dc, g_rcPin, pinFg);
    draw_header_btn(dc, g_rcClose, L"×", C_TEXT_DIM);

    { HPEN p = CreatePen(PS_SOLID, 1, C_LINE);        /* 标题栏分隔线 */
      HGDIOBJ op = SelectObject(dc, p);
      MoveToEx(dc, 0, headerH, NULL); LineTo(dc, g_w, headerH);
      SelectObject(dc, op); DeleteObject(p); }

    /* 功能列表 */
    paint_page_features(dc);

    /* 底栏 */
    int footerH = sy(46);
    RECT foot = { 0, g_h - footerH, g_w, g_h };
    fill(dc, foot, g_brHeader);
    { HPEN p = CreatePen(PS_SOLID, 1, C_LINE);
      HGDIOBJ op = SelectObject(dc, p);
      MoveToEx(dc, 0, foot.top, NULL); LineTo(dc, g_w, foot.top);
      SelectObject(dc, op); DeleteObject(p); }

    int allOn = 1;
    for (int i = 0; i < NFEAT; i++) if (!g_feat[i].on) { allOn = 0; break; }
    g_rcAll = (RECT){ sx(14), foot.top + sy(10), sx(110), foot.top + sy(34) };
    draw_btn(dc, g_rcAll, allOn ? L"全部关闭" : L"全部开启", allOn);
    RECT sr = { g_rcAll.right + sx(8), foot.top + sy(6), g_w - sx(12), g_h - sy(6) };
    COLORREF sc = (g_status == 1) ? C_OK : (g_status == 2 ? C_WARN : C_TEXT_DIM);
    text_r(dc, sr, g_statusText, sc, g_fSmall,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    frame(dc, full, C_LINE);
    BitBlt(hdc, 0, 0, g_w, g_h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}


/* ===================================================================
 * 10. 交互与窗口过程
 * =================================================================== */

static void toggle_feature(int i)
{
    Feature *f = &g_feat[i];
    f->on = !f->on;
    { char nm[128]; wname_to_utf8(f->name, nm, sizeof(nm));
      log_line("切换功能 [%s] -> %s", nm, f->on ? "开" : "关"); }
    if (!f->on && f->kind == KIND_FLAG) set_flag(f, 0);
    if (f->on) apply_all();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void toggle_all(void)
{
    int allOn = 1;
    for (int i = 0; i < NFEAT; i++) if (!g_feat[i].on) { allOn = 0; break; }
    for (int i = 0; i < NFEAT; i++) {
        g_feat[i].on = allOn ? 0 : 1;
        if (!g_feat[i].on && g_feat[i].kind == KIND_FLAG) set_flag(&g_feat[i], 0);
    }
    if (!allOn) apply_all();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static int pt_in(POINT p, RECT r)
{
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

static void set_topmost(int top)
{
    g_pinned = top;
    SetWindowPos(g_hwnd, top ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE: {
        g_hwnd = hwnd;
        RECT rc; GetClientRect(hwnd, &rc);
        g_w = rc.right; g_h = rc.bottom;
        g_sw = g_w / (float)BASE_W;
        g_sh = g_h / (float)BASE_H;
        update_resources();
        wcscpy(g_statusText, L"等待游戏启动……");
        SetTimer(hwnd, 1, 50, NULL);
        return 0;
    }

    case WM_SIZE: {
        g_w = LOWORD(lp); g_h = HIWORD(lp);
        if (g_w < 1) g_w = 1; if (g_h < 1) g_h = 1;
        g_sw = g_w / (float)BASE_W;
        g_sh = g_h / (float)BASE_H;
        update_resources();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = 360;
        mmi->ptMinTrackSize.y = 340;
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;                       /* 自绘，禁止系统擦背景 */

    case WM_PAINT:
        paint(hwnd);
        return 0;

    case WM_TIMER: {
        /* 50ms 主循环：连接 → 应用 → 读实时值 → 处理热键 → 限速重绘 */
        try_attach();
        apply_all();
        read_live();

        static int prev[16] = { 0 };
        for (int i = 0; i < NFEAT; i++) {
            int down = (GetAsyncKeyState(g_feat[i].vk) & 0x8000) ? 1 : 0;
            if (down && !prev[i]) toggle_feature(i);
            prev[i] = down;
        }
        if (++g_tick >= 3) { g_tick = 0; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    }

    case WM_NCHITTEST: {
        /* 标题栏拖拽 + 四边四角缩放；标题栏内三个按钮区域返回 HTCLIENT
         * 以便 WM_LBUTTONDOWN 接管点击。WM_SETCURSOR 交给 DefWindowProc
         * 自动按命中码切换指针（边缘=缩放箭头，其余=普通箭头）。 */
        POINT cl = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &cl);
        if (pt_in(cl, g_rcClose)) return HTCLIENT;
        if (pt_in(cl, g_rcMin))   return HTCLIENT;
        if (pt_in(cl, g_rcPin))   return HTCLIENT;
        if (cl.y < sy(58))        return HTCAPTION;

        POINT sc = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT wr; GetWindowRect(hwnd, &wr);
        int bw = 6;
        int right = wr.right - wr.left, bottom = wr.bottom - wr.top;
        int rx = sc.x - wr.left, ry = sc.y - wr.top;
        if (rx < bw        && ry < bw)         return HTTOPLEFT;
        if (rx > right-bw  && ry < bw)         return HTTOPRIGHT;
        if (rx < bw        && ry > bottom-bw)  return HTBOTTOMLEFT;
        if (rx > right-bw  && ry > bottom-bw)  return HTBOTTOMRIGHT;
        if (rx < bw)                           return HTLEFT;
        if (rx > right-bw)                     return HTRIGHT;
        if (ry < bw)                           return HTTOP;
        if (ry > bottom-bw)                    return HTBOTTOM;
        return HTCLIENT;
    }

    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (pt_in(p, g_rcClose)) { DestroyWindow(hwnd); return 0; }
        if (pt_in(p, g_rcMin))   { ShowWindow(hwnd, SW_MINIMIZE); return 0; }
        if (pt_in(p, g_rcPin))   { set_topmost(!g_pinned); return 0; }
        if (pt_in(p, g_rcAll))   { toggle_all(); return 0; }
        for (int i = 0; i < NFEAT; i++)
            if (pt_in(p, g_rcRow[i])) { toggle_feature(i); return 0; }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        detach();
        log_line("MIO Trainer 关闭");
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    (void)hPrev; (void)cmd;
    SetProcessDPIAware();
    log_init();
    log_line("MIO Trainer 启动（目标 build r21756）");

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(1));   /* 游戏原版图标 */
    wc.hIconSm       = wc.hIcon;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"MioTrainerWnd";
    if (!RegisterClassExW(&wc)) return 1;

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW,
                                L"MioTrainerWnd", L"MIO 修改器",
                                WS_POPUP,
                                (sw - BASE_W) / 2, (sh - BASE_H) / 3,
                                BASE_W, BASE_H, NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, show);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
