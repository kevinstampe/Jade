#ifndef AMALGAMATED_BUILD
#include "../button_events.h"
#include "../jade_assert.h"
#include "../ui.h"

gui_activity_t* make_camera_activity(gui_view_node_t** image_node, gui_view_node_t** label_node,
    const bool show_click_btn, const qr_guide_type_t qr_guide_type, progress_bar_t* progress_bar,
    const bool show_help_btn, const uint16_t preview_width, const uint16_t preview_height)
{
    // progress bar is optional
    JADE_INIT_OUT_PPTR(image_node);
    JADE_INIT_OUT_PPTR(label_node);

    // NOTE: atm show_click_btn and help_url are mutually exclusive
    JADE_ASSERT(!show_click_btn || !show_help_btn);

    gui_activity_t* const act = gui_make_activity();
    gui_view_node_t* image_parent = act->root_node;

    // Center the preview; portrait overlays must use its bounds, not the full screen.
    gui_make_picture(image_node, NULL);
    gui_set_align(*image_node, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
#if CONFIG_DISPLAY_HEIGHT > CONFIG_DISPLAY_WIDTH
    JADE_ASSERT(preview_width <= CONFIG_DISPLAY_WIDTH && preview_height <= CONFIG_DISPLAY_HEIGHT);
    const uint16_t xmargin = (CONFIG_DISPLAY_WIDTH - preview_width) / 2;
    const uint16_t ymargin = (CONFIG_DISPLAY_HEIGHT - preview_height) / 2;
    gui_make_vsplit(&image_parent, GUI_SPLIT_ABSOLUTE, 3, ymargin, preview_height, GUI_SPLIT_FILL_REMAINING);
    gui_set_parent(image_parent, act->root_node);
    gui_view_node_t* header_parent;
    gui_make_fill(&header_parent, TFT_BLACK, FILL_PLAIN, image_parent);
    gui_set_margins(*image_node, GUI_MARGIN_ALL_DIFFERENT, 0,
        CONFIG_DISPLAY_WIDTH - preview_width - xmargin, 0, xmargin);
#endif
    gui_set_parent(*image_node, image_parent);
    gui_view_node_t* parent = *image_node;

    // QR frame guide if applicable
    if (qr_guide_type == QR_GUIDE_SHOW) {
        gui_make_qrguide(&parent, TFT_WHITE);
        gui_set_parent(parent, *image_node);
    }

    gui_view_node_t* vsplit;
    gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 3, 20, 60, 20);
    gui_set_parent(vsplit, parent);
#if CONFIG_DISPLAY_HEIGHT > CONFIG_DISPLAY_WIDTH
    // The buttons live above the preview; leave their former overlay row empty.
    gui_view_node_t* header_space;
    gui_make_vsplit(&header_space, GUI_SPLIT_RELATIVE, 1, 100);
    gui_set_parent(header_space, vsplit);
#else
    gui_view_node_t* header_parent = vsplit;
#endif

    // Header row buttons - back and either help or 'click'
    btn_data_t hdrbtns[]
        = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_CAMERA_EXIT, .borders = GUI_BORDER_ALL },
              { .txt = "?", .font = GUI_TITLE_FONT, .ev_id = BTN_CAMERA_HELP, .borders = GUI_BORDER_ALL } };

    if (show_click_btn) {
        hdrbtns[1].txt = "S";
        hdrbtns[1].font = VARIOUS_SYMBOLS_FONT;
        hdrbtns[1].ev_id = BTN_CAMERA_CLICK;
    }

    gui_view_node_t* hsplit;
    gui_make_hsplit(&hsplit, GUI_SPLIT_RELATIVE, 3, 15, 70, 15);
    gui_set_parent(hsplit, header_parent);

    // Back/cancel button
    add_button(hsplit, &hdrbtns[0]);

    // Any help or 'click' button, if required
    if (show_help_btn || show_click_btn) {
        gui_view_node_t* spacer;
        gui_make_vsplit(&spacer, GUI_SPLIT_RELATIVE, 1, 100); // no-op transparent spacer
        gui_set_parent(spacer, hsplit);
        add_button(hsplit, &hdrbtns[1]);
    }

    // Portrait captions sit below the preview rather than obscuring the QR.
    gui_make_text(label_node, "Initializing...", TFT_WHITE);
    gui_set_align(*label_node, GUI_ALIGN_CENTER, GUI_ALIGN_MIDDLE);
#if CONFIG_DISPLAY_HEIGHT > CONFIG_DISPLAY_WIDTH
    // gui_update_text() repaints the parent; give it an opaque background
    // so replacing "Initializing..." clears the previous caption.
    gui_view_node_t* caption_background;
    gui_make_fill(&caption_background, TFT_BLACK, FILL_PLAIN, image_parent);
    gui_set_parent(*label_node, caption_background);
    // Keep the middle overlay row empty so progress remains at the bottom.
    gui_view_node_t* centre_space;
    gui_make_vsplit(&centre_space, GUI_SPLIT_RELATIVE, 1, 100);
    gui_set_parent(centre_space, vsplit);
#else
    gui_set_parent(*label_node, vsplit);
#endif

    // Bottom part, any progress bar if applicable (transparent)
    if (progress_bar) {
        progress_bar->transparent = true;
        make_progress_bar(vsplit, progress_bar);
        gui_set_borders(progress_bar->container, GUI_BLOCKSTREAM_BUTTONBORDER_GREY, 1, GUI_BORDER_ALL);
        gui_set_margins(progress_bar->container, GUI_MARGIN_ALL_DIFFERENT, 12, 12, 4, 12);
    }

    return act;
}
#endif // AMALGAMATED_BUILD
