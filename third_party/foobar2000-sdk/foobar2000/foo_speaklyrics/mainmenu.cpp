#include "stdafx.h"

#include "config.h"

#include "playback.h"
#include "lyrics_jump_window.h"
#include "lyrics_search_window.h"
#include "lyrics_timestamp_window.h"

#include "speech_engine.h"



static const GUID guid_menu_group = { 0xbf62fc58, 0xc9d2, 0x4d55,{ 0x9c, 0x48, 0x48, 0x78, 0xa6, 0x98, 0x20, 0x2d } };

static const GUID guid_cmd_settings = { 0x33c87fa3, 0x74d0, 0x4b84,{ 0x86, 0x16, 0xb7, 0xe5, 0x45, 0x97, 0x5f, 0x4d } };

static const GUID guid_cmd_toggle_auto = { 0xde9f9261, 0x3053, 0x4d09,{ 0x8f, 0xcf, 0x13, 0xcd, 0xda, 0x77, 0x12, 0x8e } };

static const GUID guid_cmd_load_file = { 0x929354bc, 0x62a8, 0x4bb7,{ 0x80, 0x3d, 0xa6, 0xe8, 0x5b, 0x1f, 0xac, 0xf3 } };

static const GUID guid_cmd_set_folder = { 0x39bd72b6, 0x44be, 0x4c3d,{ 0x8b, 0x9f, 0xf0, 0x48, 0xd5, 0x50, 0x13, 0xdf } };

static const GUID guid_cmd_set_temp_folder = { 0x6f8cfce7, 0xf048, 0x4a66,{ 0x98, 0x43, 0x1f, 0x49, 0xf4, 0x4f, 0x56, 0x7b } };
static const GUID guid_cmd_copy_plain_lyrics = { 0x98e3e24d, 0x307a, 0x4a84,{ 0x98, 0xa6, 0x5a, 0x1b, 0xa1, 0x45, 0x7e, 0x2c } };
static const GUID guid_cmd_jump_by_lyrics = { 0x7e16a037, 0x1439, 0x492d,{ 0xa0, 0xa4, 0x37, 0xd4, 0x49, 0x2d, 0xa8, 0x83 } };
static const GUID guid_cmd_search_lrc = { 0xd57492a2, 0x831d, 0x4504,{ 0x9e, 0x4c, 0x27, 0x4b, 0x0d, 0xb3, 0x71, 0x6f } };
static const GUID guid_cmd_add_timestamp_lrc = { 0x3c0fa88d, 0x1e41, 0x4b9a,{ 0x95, 0x3b, 0x5b, 0xdf, 0xd3, 0x91, 0x61, 0xa8 } };



namespace {

    const char* utf8_from_wide(const wchar_t* text) {

        enum { slots = 16 };

        static thread_local pfc::string8 buffers[slots];

        static thread_local unsigned index = 0;

        index = (index + 1) % slots;

        buffers[index] = pfc::stringcvt::string_utf8_from_wide(text).get_ptr();

        return buffers[index].get_ptr();

    }



    void set_utf8(pfc::string_base& out, const wchar_t* text) {

        out = pfc::stringcvt::string_utf8_from_wide(text).get_ptr();

    }

}



static mainmenu_group_popup_factory g_menu_group(guid_menu_group, mainmenu_groups::view, mainmenu_commands::sort_priority_dontcare, utf8_from_wide(L"\u6717\u8BFB\u6B4C\u8BCD(&L)"));



class speaklyrics_menu : public mainmenu_commands {

public:

    enum { cmd_settings, cmd_toggle_auto, cmd_load_file, cmd_set_folder, cmd_set_temp_folder, cmd_search_lrc, cmd_add_timestamp_lrc, cmd_copy_plain_lyrics, cmd_jump_by_lyrics, cmd_total };



    t_uint32 get_command_count() override { return cmd_total; }



    GUID get_command(t_uint32 index) override {

        switch (index) {

        case cmd_settings: return guid_cmd_settings;

        case cmd_toggle_auto: return guid_cmd_toggle_auto;

        case cmd_load_file: return guid_cmd_load_file;

        case cmd_set_folder: return guid_cmd_set_folder;

        case cmd_set_temp_folder: return guid_cmd_set_temp_folder;
        case cmd_search_lrc: return guid_cmd_search_lrc;
        case cmd_add_timestamp_lrc: return guid_cmd_add_timestamp_lrc;
        case cmd_copy_plain_lyrics: return guid_cmd_copy_plain_lyrics;
        case cmd_jump_by_lyrics: return guid_cmd_jump_by_lyrics;

        default: uBugCheck();

        }

    }



    void get_name(t_uint32 index, pfc::string_base& out) override {

        switch (index) {

        case cmd_settings: set_utf8(out, L"\u6717\u8BFB\u6B4C\u8BCD\u8BBE\u7F6E(&S)"); break;

        case cmd_toggle_auto: set_utf8(out, L"\u81EA\u52A8\u6717\u8BFB\u6B4C\u8BCD"); break;

        case cmd_load_file: set_utf8(out, L"\u52A0\u8F7D\u672C\u5730LRC\u6B4C\u8BCD(&L)"); break;

        case cmd_set_folder: set_utf8(out, L"\u8BBE\u7F6ELRC\u6B4C\u8BCD\u76EE\u5F55(&R)"); break;

        case cmd_set_temp_folder: set_utf8(out, L"\u8BBE\u7F6ELRC\u6B4C\u8BCD\u4E34\u65F6\u76EE\u5F55(&&c) c"); break;
        case cmd_search_lrc: set_utf8(out, L"\u641C\u7D22LRC\u6B4C\u8BCD(&&o) o"); break;
        case cmd_add_timestamp_lrc: set_utf8(out, L"\u6DFB\u52A0\u5F53\u524D\u65F6\u95F4\u6B4C\u8BCD"); break;
        case cmd_copy_plain_lyrics: set_utf8(out, L"\u590D\u5236\u65E0\u65F6\u95F4\u6233\u6B4C\u8BCD(&&p) p"); break;
        case cmd_jump_by_lyrics: set_utf8(out, L"\u6309\u6B4C\u8BCD\u8DF3\u8F6C(&&G) g"); break;

        default: uBugCheck();

        }

    }



    bool get_description(t_uint32 index, pfc::string_base& out) override {

        switch (index) {

        case cmd_settings: set_utf8(out, L"\u6253\u5F00\u6717\u8BFB\u6B4C\u8BCD\u8BBE\u7F6E\u3002"); return true;

        case cmd_toggle_auto: set_utf8(out, L"\u5F00\u542F\u6216\u5173\u95ED\u81EA\u52A8\u6717\u8BFB\u6B4C\u8BCD\u3002"); return true;

        case cmd_load_file: set_utf8(out, L"\u624B\u52A8\u9009\u62E9\u672C\u5730 LRC \u6B4C\u8BCD\u6587\u4EF6\u3002"); return true;

        case cmd_set_folder: set_utf8(out, L"\u9009\u62E9\u672C\u5730 LRC \u6B4C\u8BCD\u76EE\u5F55\u3002"); return true;

        case cmd_set_temp_folder: set_utf8(out, L"\u9009\u62E9 LRC \u6B4C\u8BCD\u4E34\u65F6\u76EE\u5F55\u3002\u4ECE\u8BE5\u76EE\u5F55\u52A0\u8F7D\u7684\u6B4C\u8BCD\u5728\u5207\u6362\u540E\u4F1A\u5220\u9664\u3002"); return true;
        case cmd_search_lrc: set_utf8(out, L"\u6253\u5F00\u641C\u7D22lrc\u6B4C\u8BCD\u7A97\u53E3\u3002"); return true;
        case cmd_add_timestamp_lrc: set_utf8(out, L"\u6253\u5F00 LRC \u624B\u52A8\u6253\u8F74\u7A97\u53E3\uFF0C\u628A\u5F53\u524D\u64AD\u653E\u65F6\u95F4\u5199\u5165\u6B4C\u8BCD\u3002"); return true;
        case cmd_copy_plain_lyrics: set_utf8(out, L"\u590D\u5236\u5F53\u524D\u5DF2\u52A0\u8F7D LRC \u6B4C\u8BCD\u7684\u65E0\u65F6\u95F4\u6233\u6587\u672C\u3002"); return true;
        case cmd_jump_by_lyrics: set_utf8(out, L"\u6253\u5F00\u6B4C\u8BCD\u5217\u8868\uFF0C\u9009\u4E2D\u4E00\u53E5\u540E\u56DE\u8F66\u8DF3\u8F6C\u5230\u5BF9\u5E94\u64AD\u653E\u4F4D\u7F6E\u3002"); return true;

        default: return false;

        }

    }



    GUID get_parent() override { return guid_menu_group; }



    void execute(t_uint32 index, service_ptr_t<service_base>) override {

        HWND parent = core_api::get_main_window();

        switch (index) {

        case cmd_settings:

            show_settings_dialog(parent);

            reload_current_lyrics();

            break;

        case cmd_toggle_auto: {

            bool enabled = !cfg_auto_speak.get();

            cfg_auto_speak = enabled;

            speech_queue_speak(enabled ? L"LRC\u6717\u8bfb\u5df2\u6253\u5f00" : L"LRC\u6717\u8bfb\u5df2\u5173\u95ed", true);

            break;

        }

        case cmd_load_file: {

            pfc::string8 path;

            if (browse_lrc_file(parent, path)) {

                set_manual_lrc_file_for_current_track(path.get_ptr());

            }

            break;

        }

        case cmd_set_folder: {

            pfc::string8 path;

            if (browse_lrc_folder(parent, path)) {

                cfg_lrc_folder.set(path.get_ptr());

                reload_current_lyrics();

            }

            break;

        }

        case cmd_set_temp_folder: {

            pfc::string8 path;

            if (browse_temp_lrc_folder(parent, path)) {

                cfg_temp_lrc_folder.set(path.get_ptr());

                reload_current_lyrics();

            }

            break;

        }

        case cmd_search_lrc:

            show_lyrics_search_window(parent);

            break;

        case cmd_add_timestamp_lrc:

            show_lyrics_timestamp_window(parent);

            break;

        case cmd_copy_plain_lyrics:

            copy_current_lyrics_without_timestamps();

            break;

        case cmd_jump_by_lyrics:

            show_lyrics_jump_window(parent);

            break;

        default:

            uBugCheck();

        }

    }



    bool get_display(t_uint32 index, pfc::string_base& text, t_uint32& flags) override {

        get_name(index, text);

        flags = 0;

        if (index == cmd_toggle_auto && cfg_auto_speak.get()) flags |= flag_checked;

        return true;

    }

};



static mainmenu_commands_factory_t<speaklyrics_menu> g_menu_factory;








