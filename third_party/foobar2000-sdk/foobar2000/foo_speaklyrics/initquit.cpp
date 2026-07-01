#include "stdafx.h"
#include "config.h"
#include "speaklyrics_log.h"
#include "speech_engine.h"

namespace {

constexpr UINT kDefaultStartupHideDelayMs = 800;
constexpr UINT kMaxStartupHideDelayMs = 10000;
constexpr UINT kStartupHideVerifyIntervalMs = 250;
constexpr unsigned kStartupHideVerifyCount = 12;
UINT_PTR g_startup_hide_timer = 0;
UINT_PTR g_startup_hide_verify_timer = 0;
unsigned g_startup_hide_verify_remaining = 0;

bool startup_is_component_install() {
    const wchar_t* commandLine = GetCommandLineW();
    if (!commandLine) return false;

    std::wstring lowered(commandLine);
    CharLowerBuffW(lowered.data(), static_cast<DWORD>(lowered.size()));
    return lowered.find(L".fb2k-component") != std::wstring::npos;
}

UINT startup_hide_delay_ms() {
    int delay = static_cast<int>(cfg_hide_main_window_delay_ms.get());
    if (delay < 0) delay = 0;
    if (delay > static_cast<int>(kMaxStartupHideDelayMs)) delay = static_cast<int>(kMaxStartupHideDelayMs);
    return static_cast<UINT>(delay);
}

bool main_window_is_foreground(HWND wnd) {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    if (foreground == wnd || IsChild(wnd, foreground)) return true;
    return GetAncestor(foreground, GA_ROOT) == wnd;
}

bool main_window_was_shown_by_user() {
    HWND wnd = core_api::get_main_window();
    return wnd && IsWindow(wnd) && IsWindowVisible(wnd) && !IsIconic(wnd) && main_window_is_foreground(wnd);
}

void hide_main_window_from_alt_tab() {
    HWND wnd = core_api::get_main_window();
    if (wnd && IsWindow(wnd)) {
        ShowWindow(wnd, SW_HIDE);
        speaklyrics_log_info(L"启动隐藏：已隐藏 foobar2000 主窗口。");
    }
}

void hide_main_window_on_startup() {
    auto ui = ui_control::tryGet();
    if (!ui.is_empty()) {
        ui->hide();
    }
    hide_main_window_from_alt_tab();
}

void stop_startup_hide_verify_timer() {
    if (g_startup_hide_verify_timer) {
        KillTimer(nullptr, g_startup_hide_verify_timer);
        g_startup_hide_verify_timer = 0;
    }
    g_startup_hide_verify_remaining = 0;
}

void CALLBACK startup_hide_verify_timer_proc(HWND, UINT, UINT_PTR id, DWORD) {
    if (id != g_startup_hide_verify_timer) return;
    if (core_api::is_shutting_down() || g_startup_hide_verify_remaining == 0) {
        stop_startup_hide_verify_timer();
        return;
    }

    if (main_window_was_shown_by_user()) {
        speaklyrics_log_info(L"启动隐藏：检测到用户主动显示主窗口，停止后续复查隐藏。");
        stop_startup_hide_verify_timer();
        return;
    }

    hide_main_window_from_alt_tab();
    --g_startup_hide_verify_remaining;
    if (g_startup_hide_verify_remaining == 0) {
        stop_startup_hide_verify_timer();
    }
}

void start_startup_hide_verify_timer() {
    stop_startup_hide_verify_timer();
    g_startup_hide_verify_remaining = kStartupHideVerifyCount;
    g_startup_hide_verify_timer = SetTimer(nullptr, 0, kStartupHideVerifyIntervalMs, startup_hide_verify_timer_proc);
    if (!g_startup_hide_verify_timer) {
        g_startup_hide_verify_remaining = 0;
    }
}

void CALLBACK startup_hide_timer_proc(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    if (id != g_startup_hide_timer || core_api::is_shutting_down()) return;
    g_startup_hide_timer = 0;

    if (main_window_was_shown_by_user()) {
        speaklyrics_log_info(L"启动隐藏：主窗口已由用户激活，本次跳过隐藏。");
        return;
    }

    hide_main_window_on_startup();
    start_startup_hide_verify_timer();
}

void schedule_startup_hide() {
    if (!cfg_hide_main_window_on_startup.get() || core_api::is_quiet_mode_enabled()) return;
    if (startup_is_component_install()) {
        speaklyrics_log_info(L"启动隐藏：检测到组件安装启动，本次跳过隐藏。");
        return;
    }

    UINT delay = startup_hide_delay_ms();
    speaklyrics_log_info(L"启动隐藏：计划在 %u 毫秒后隐藏主窗口。", delay);
    g_startup_hide_timer = SetTimer(nullptr, 0, delay, startup_hide_timer_proc);
    if (!g_startup_hide_timer) {
        g_startup_hide_timer = SetTimer(nullptr, 0, kDefaultStartupHideDelayMs, startup_hide_timer_proc);
    }
}

}

class speaklyrics_initquit : public initquit {
public:
    void on_init() override { schedule_startup_hide(); }
    void on_quit() override {
        if (g_startup_hide_timer) {
            KillTimer(nullptr, g_startup_hide_timer);
            g_startup_hide_timer = 0;
        }
        stop_startup_hide_verify_timer();
        speech_shutdown();
    }
};

static initquit_factory_t<speaklyrics_initquit> g_initquit;
