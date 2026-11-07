#ifndef GROK_PAGE_H
#define GROK_PAGE_H

#include "grok_monitor.h"

#include <lvgl.h>

#include <array>
#include <string>

class Display;

// Full-width LVGL page drawn over the chat view, below the status bar: one row per
// Bot, and under them the latest reply or what a busy Bot is doing.
class GrokPage {
public:
    explicit GrokPage(Display* display) : display_(display) {}

    // Safe from any task: takes the display lock itself. Creates the widgets the
    // first time the page is shown, so an unused page costs no heap. `setup_url` is
    // where the relay can be entered, empty while the local page is not up.
    void Apply(bool visible, const GrokStatus& status, const std::string& setup_url);

private:
    struct Row {
        lv_obj_t* root = nullptr;
        lv_obj_t* name = nullptr;
        lv_obj_t* state = nullptr;
    };

    void Create();
    void Render(const GrokStatus& status, const std::string& setup_url);

    Display* display_;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* heading_ = nullptr;
    lv_obj_t* setup_ = nullptr;
    std::array<Row, grok::kMaxBots> rows_;
    lv_obj_t* detail_ = nullptr;
    lv_obj_t* footer_ = nullptr;
};

#endif  // GROK_PAGE_H
