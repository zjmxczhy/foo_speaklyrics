#include "stdafx.h"
#include "background_task.h"
#include "config.h"
#include "lyrics_search_window.h"
#include "playback.h"
#include "speech_engine.h"
#include "temp_lrc_manifest.h"

class speaklyrics_initquit : public initquit {
public:
    void on_init() override {
        temp_lrc_manifest_cleanup();
        write_screen_reader_channel_config_files();
        speech_preload();
    }
    void on_quit() override {
        // Stop accepting new work and signal every SDK-managed task first.
        speaklyrics_cancel_all_background_tasks();
        cancel_playback_background_tasks();
        cancel_lyrics_search_background_tasks();
        speech_shutdown();
    }
};

static initquit_factory_t<speaklyrics_initquit> g_initquit;
