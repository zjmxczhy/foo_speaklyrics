#include "stdafx.h"
#include "speech_engine.h"
#include "temp_lrc_manifest.h"

class speaklyrics_initquit : public initquit {
public:
    void on_init() override { temp_lrc_manifest_cleanup(); }
    void on_quit() override {
        temp_lrc_manifest_cleanup();
        speech_shutdown();
    }
};

static initquit_factory_t<speaklyrics_initquit> g_initquit;
