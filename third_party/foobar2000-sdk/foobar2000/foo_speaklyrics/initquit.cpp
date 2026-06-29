#include "stdafx.h"
#include "config.h"
#include "speech_engine.h"

namespace {

constexpr UINT kDefaultStartupHideDelayMs = 800;
constexpr UINT kMaxStartupHideDelayMs = 10000;
UINT_PTR g_startup_hide_timer = 0;

UINT startup_hide_delay_ms() {
    int delay = static_cast<int>(cfg_hide_main_window_delay_ms.get());
    if (delay < 0) delay = 0;
    if (delay > static_cast<int>(kMaxStartupHideDelayMs)) delay = static_cast<int>(kMaxStartupHideDelayMs);
    return static_cast<UINT>(delay);
}

void CALLBACK startup_hide_timer_proc(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    if (id != g_startup_hide_timer || core_api::is_shutting_down()) return;
    g_startup_hide_timer = 0;

    auto ui = ui_control::tryGet();
    if (ui.is_empty()) return;
    ui->hide();
}

void schedule_startup_hide() {
    if (!cfg_hide_main_window_on_startup.get() || core_api::is_quiet_mode_enabled()) return;

    UINT delay = startup_hide_delay_ms();
    g_startup_hide_timer = SetTimer(nullptr, 0, delay, startup_hide_timer_proc);
    if (!g_startup_hide_timer) {
        g_startup_hide_timer = SetTimer(nullptr, 0, kDefaultStartupHideDelayMs, startup_hide_timer_proc);
    }
}

}

class speaklyrics_initquit : public initquit {
public:
    void on_init() override { schedule_startup_hide(); }
    void on_quit() override { speech_shutdown(); }
};

static initquit_factory_t<speaklyrics_initquit> g_initquit;
