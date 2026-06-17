#include "stdafx.h"
#include "speech_engine.h"

class speaklyrics_initquit : public initquit {
public:
    void on_init() override {}
    void on_quit() override { speech_shutdown(); }
};

static initquit_factory_t<speaklyrics_initquit> g_initquit;
