#include <gtk/gtk.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif

#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct AppState {
    GtkApplication* app = nullptr;
    GtkWidget* window = nullptr;
    GtkWidget* video_area = nullptr;
    GtkWidget* playlist_scroller = nullptr;
    GtkWidget* playlist_view = nullptr;
    GtkListStore* playlist_store = nullptr;
    GtkWidget* playlist_toggle_item = nullptr;
    GtkWidget* context_playlist_toggle_item = nullptr;
    GtkWidget* fullscreen_toggle_item = nullptr;
    GtkWidget* playback_state_label = nullptr;
    GtkWidget* position_scale = nullptr;
    GtkWidget* volume_scale = nullptr;
    GtkWidget* playback_controls = nullptr;
    GtkWidget* player_context_menu = nullptr;
    GtkWidget* show_controls_item = nullptr;
    GtkWidget* menubar = nullptr;

    mpv_handle* mpv = nullptr;
    mpv_render_context* mpv_render = nullptr;

#ifdef GDK_WINDOWING_WAYLAND
    wl_display* wl_display_handle = nullptr;
#endif

    bool fullscreen = false;
    bool suppress_position_seek = false;
    std::string current_media_uri;
};

enum PlaylistColumns {
    COL_TITLE = 0,
    COL_URI,
    COL_COUNT
};

static void set_playback_state(AppState* state, const char* status) {
    if (!state->playback_state_label) {
        return;
    }
    std::string text = std::string("Playback state: ") + status;
    gtk_label_set_text(GTK_LABEL(state->playback_state_label), text.c_str());
}

static void set_menubar_visibility(AppState* state, bool visible) {
    if (state->menubar) {
        gtk_widget_set_visible(state->menubar, visible);
    }
}

static void set_fullscreen_state(AppState* state, bool fullscreen) {
    if (fullscreen) {
        gtk_window_fullscreen(GTK_WINDOW(state->window));
        state->fullscreen = true;
        set_menubar_visibility(state, false);
    } else {
        gtk_window_unfullscreen(GTK_WINDOW(state->window));
        state->fullscreen = false;
        set_menubar_visibility(state, true);
    }

    if (state->fullscreen_toggle_item) {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(state->fullscreen_toggle_item), state->fullscreen);
    }
}

static std::string trim_copy(const std::string& value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

static bool ensure_mpv_running(AppState* state) {
    if (state->mpv) {
        return true;
    }

    state->mpv = mpv_create();
    if (!state->mpv) {
        g_warning("Failed to create mpv instance");
        return false;
    }

    mpv_set_option_string(state->mpv, "terminal", "yes");
    mpv_set_option_string(state->mpv, "idle", "yes");
    mpv_set_option_string(state->mpv, "keep-open", "yes");
    mpv_set_option_string(state->mpv, "force-window", "no");
    mpv_set_option_string(state->mpv, "vo", "libmpv");
    mpv_set_option_string(state->mpv, "hwdec", "auto-safe");

    if (mpv_initialize(state->mpv) < 0) {
        g_warning("Failed to initialize mpv");
        mpv_terminate_destroy(state->mpv);
        state->mpv = nullptr;
        return false;
    }

    return true;
}

static void run_mpv_command(AppState* state, std::vector<const char*> args) {
    if (!ensure_mpv_running(state)) {
        return;
    }
    args.push_back(nullptr);
    if (mpv_command_async(state->mpv, 0, args.data()) < 0) {
        g_warning("mpv command failed");
    }
}

static void send_loadfile(AppState* state, const std::string& uri, const std::string& mode) {
    run_mpv_command(state, {"loadfile", uri.c_str(), mode.c_str()});
    if (mode == "replace") {
        state->current_media_uri = uri;
        set_playback_state(state, "Playing");
    }
}

static void send_seek_relative(AppState* state, int seconds) {
    std::string offset = std::to_string(seconds);
    run_mpv_command(state, {"seek", offset.c_str(), "relative"});
}

static void send_seek_percent(AppState* state, double percent) {
    std::string pct = std::to_string(percent);
    run_mpv_command(state, {"seek", pct.c_str(), "absolute-percent"});
}

static void* get_proc_address(void*, const char* name) {
    const auto proc = eglGetProcAddress(name);
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(proc));
}

static gboolean queue_video_redraw(gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    if (state->video_area) {
        gtk_gl_area_queue_render(GTK_GL_AREA(state->video_area));
    }
    return G_SOURCE_REMOVE;
}

static void on_mpv_render_update(void* ctx) {
    auto* state = static_cast<AppState*>(ctx);
    g_main_context_invoke(nullptr, queue_video_redraw, state);
}

static void on_video_area_realize(GtkWidget* widget, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    if (!ensure_mpv_running(state)) {
        return;
    }

    gtk_gl_area_make_current(GTK_GL_AREA(widget));
    if (gtk_gl_area_get_error(GTK_GL_AREA(widget)) != nullptr) {
        g_warning("GL area failed to initialize");
        return;
    }

    if (state->mpv_render) {
        return;
    }

    mpv_opengl_init_params gl_init_params{
        .get_proc_address = get_proc_address,
        .get_proc_address_ctx = widget,
    };

#ifdef GDK_WINDOWING_WAYLAND
    GdkDisplay* gdk_display = gtk_widget_get_display(widget);
    if (GDK_IS_WAYLAND_DISPLAY(gdk_display)) {
        state->wl_display_handle = gdk_wayland_display_get_wl_display(gdk_display);
    } else {
        g_warning("Expected Wayland display backend for mate-mpv");
    }
#endif

    int advanced = 1;
#ifdef MPV_RENDER_PARAM_WL_DISPLAY
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
#ifdef GDK_WINDOWING_WAYLAND
        {MPV_RENDER_PARAM_WL_DISPLAY, state->wl_display_handle},
#endif
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
#else
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
#endif

    if (mpv_render_context_create(&state->mpv_render, state->mpv, params) < 0) {
        g_warning("Failed to create mpv render context");
        return;
    }

    mpv_render_context_set_update_callback(state->mpv_render, on_mpv_render_update, state);
}

static void on_video_area_unrealize(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    if (state->mpv_render) {
        mpv_render_context_set_update_callback(state->mpv_render, nullptr, nullptr);
        mpv_render_context_free(state->mpv_render);
        state->mpv_render = nullptr;
    }
}

static gboolean on_video_area_render(GtkGLArea* area, GdkGLContext*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    if (!state->mpv_render) {
        return TRUE;
    }

    const int width = gtk_widget_get_allocated_width(GTK_WIDGET(area));
    const int height = gtk_widget_get_allocated_height(GTK_WIDGET(area));

    mpv_opengl_fbo fbo{.fbo = 0, .w = width, .h = height, .internal_format = 0};
    int flip_y = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };

    mpv_render_context_render(state->mpv_render, params);
    return TRUE;
}

static void on_return_to_start_clicked(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    send_seek_percent(state, 0.0);
    state->suppress_position_seek = true;
    gtk_range_set_value(GTK_RANGE(state->position_scale), 0.0);
    state->suppress_position_seek = false;
    set_playback_state(state, "Playing");
}

static void on_rewind_clicked(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    send_seek_relative(state, -10);
    set_playback_state(state, "Playing");
}

static void on_play_pause_clicked(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    if (!ensure_mpv_running(state)) {
        return;
    }

    int pause = 0;
    mpv_get_property(state->mpv, "pause", MPV_FORMAT_FLAG, &pause);
    pause = !pause;
    mpv_set_property(state->mpv, "pause", MPV_FORMAT_FLAG, &pause);
    set_playback_state(state, pause ? "Paused" : "Playing");
}

static void on_stop_clicked(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    run_mpv_command(state, {"stop"});
    state->suppress_position_seek = true;
    gtk_range_set_value(GTK_RANGE(state->position_scale), 0.0);
    state->suppress_position_seek = false;
    set_playback_state(state, "Stopped");
}

static void on_fast_forward_clicked(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    send_seek_relative(state, 10);
    set_playback_state(state, "Playing");
}

static void on_position_value_changed(GtkRange* range, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    if (!state->suppress_position_seek) {
        send_seek_percent(state, gtk_range_get_value(range));
    }
}

static void on_volume_value_changed(GtkRange* range, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    if (!ensure_mpv_running(state)) {
        return;
    }
    double volume = gtk_range_get_value(range);
    mpv_set_property(state->mpv, "volume", MPV_FORMAT_DOUBLE, &volume);
}

static void on_fullscreen_button_clicked(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    set_fullscreen_state(state, !state->fullscreen);
}

static void on_toggle_fullscreen(GtkCheckMenuItem* item, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    set_fullscreen_state(state, gtk_check_menu_item_get_active(item));
}

static void on_copy_location_activate(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    if (state->current_media_uri.empty()) {
        return;
    }
    GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clipboard, state->current_media_uri.c_str(), -1);
}

static void on_toggle_controls(GtkCheckMenuItem* item, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    gtk_widget_set_visible(state->playback_controls, gtk_check_menu_item_get_active(item));
}

static gboolean on_video_area_button_press(GtkWidget*, GdkEventButton* event, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    if (!event || event->type != GDK_BUTTON_PRESS || event->button != 3 || !state->player_context_menu) {
        return FALSE;
    }
    gtk_menu_popup_at_pointer(GTK_MENU(state->player_context_menu), reinterpret_cast<GdkEvent*>(event));
    return TRUE;
}

static void add_playlist_entry(AppState* state, const std::string& title, const std::string& uri) {
    GtkTreeIter iter;
    gtk_list_store_append(state->playlist_store, &iter);
    gtk_list_store_set(state->playlist_store, &iter, COL_TITLE, title.c_str(), COL_URI, uri.c_str(), -1);
}

static std::vector<std::string> selected_files_from_dialog(GtkWindow* parent) {
    std::vector<std::string> files;
    GtkWidget* dialog = gtk_file_chooser_dialog_new("Open Media Files", parent, GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     "_Cancel", GTK_RESPONSE_CANCEL,
                                                     "_Open", GTK_RESPONSE_ACCEPT,
                                                     nullptr);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        GSList* selected = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
        for (GSList* l = selected; l != nullptr; l = l->next) {
            char* file = static_cast<char*>(l->data);
            files.emplace_back(file);
            g_free(file);
        }
        g_slist_free(selected);
    }
    gtk_widget_destroy(dialog);
    return files;
}

static void on_open_files_activate(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    const auto files = selected_files_from_dialog(GTK_WINDOW(state->window));
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& path = files[i];
        const auto slash = path.find_last_of('/');
        const std::string title = slash == std::string::npos ? path : path.substr(slash + 1);
        add_playlist_entry(state, title, path);
        send_loadfile(state, path, i == 0 ? "replace" : "append-play");
    }
}

static std::vector<std::pair<std::string, std::string>> read_channels_conf() {
    std::vector<std::pair<std::string, std::string>> channels;
    std::string conf_path = std::string(g_get_home_dir()) + "/.config/mpv/channels.conf";
    std::ifstream input(conf_path);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        auto delim = line.find(':');
        if (delim == std::string::npos) {
            continue;
        }
        std::string title = trim_copy(line.substr(0, delim));
        std::string uri = trim_copy(line.substr(delim + 1));
        if (!title.empty() && !uri.empty()) {
            channels.emplace_back(title, uri);
        }
    }
    return channels;
}

static void on_open_tv_activate(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    const auto channels = read_channels_conf();
    if (channels.empty()) {
        GtkWidget* msg = gtk_message_dialog_new(GTK_WINDOW(state->window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                                 GTK_BUTTONS_OK,
                                                 "No channels found in ~/.config/mpv/channels.conf");
        gtk_dialog_run(GTK_DIALOG(msg));
        gtk_widget_destroy(msg);
        return;
    }

    for (size_t i = 0; i < channels.size(); ++i) {
        add_playlist_entry(state, channels[i].first, channels[i].second);
        send_loadfile(state, channels[i].second, i == 0 ? "replace" : "append-play");
    }
}

static void on_playlist_row_activated(GtkTreeView* tree_view, GtkTreePath* path, GtkTreeViewColumn*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    GtkTreeModel* model = gtk_tree_view_get_model(tree_view);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) {
        return;
    }
    gchar* uri = nullptr;
    gtk_tree_model_get(model, &iter, COL_URI, &uri, -1);
    if (uri) {
        send_loadfile(state, uri, "replace");
        g_free(uri);
    }
}

static void on_toggle_playlist(GtkCheckMenuItem* item, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    gboolean active = gtk_check_menu_item_get_active(item);
    gtk_widget_set_visible(state->playlist_scroller, active);

    if (state->playlist_toggle_item && GTK_WIDGET(item) != state->playlist_toggle_item) {
        g_signal_handlers_block_by_func(state->playlist_toggle_item, reinterpret_cast<gpointer>(on_toggle_playlist), state);
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(state->playlist_toggle_item), active);
        g_signal_handlers_unblock_by_func(state->playlist_toggle_item, reinterpret_cast<gpointer>(on_toggle_playlist), state);
    }

    if (state->context_playlist_toggle_item && GTK_WIDGET(item) != state->context_playlist_toggle_item) {
        g_signal_handlers_block_by_func(state->context_playlist_toggle_item, reinterpret_cast<gpointer>(on_toggle_playlist), state);
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(state->context_playlist_toggle_item), active);
        g_signal_handlers_unblock_by_func(state->context_playlist_toggle_item, reinterpret_cast<gpointer>(on_toggle_playlist), state);
    }
}

static gboolean on_window_motion_notify(GtkWidget*, GdkEventMotion* event, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    if (!state->fullscreen || !state->menubar || !event) {
        return FALSE;
    }
    set_menubar_visibility(state, event->y <= 1.0);
    return FALSE;
}

static GtkWidget* create_playback_controls(AppState* state) {
    GtkWidget* controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(controls), 8);

    GtkWidget* center_column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(center_column, TRUE);

    GtkWidget* button_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* start_button = gtk_button_new_with_label("Return to Start");
    GtkWidget* rewind_button = gtk_button_new_with_label("Rewind");
    GtkWidget* play_pause_button = gtk_button_new_with_label("Play/Pause");
    GtkWidget* stop_button = gtk_button_new_with_label("Stop");
    GtkWidget* fast_forward_button = gtk_button_new_with_label("Fast Forward");

    gtk_box_pack_start(GTK_BOX(button_row), start_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_row), rewind_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_row), play_pause_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_row), stop_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_row), fast_forward_button, FALSE, FALSE, 0);

    state->position_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 1.0);
    gtk_scale_set_draw_value(GTK_SCALE(state->position_scale), FALSE);
    gtk_widget_set_hexpand(state->position_scale, TRUE);

    state->playback_state_label = gtk_label_new("Playback state: Stopped");
    gtk_label_set_xalign(GTK_LABEL(state->playback_state_label), 0.0f);

    gtk_box_pack_start(GTK_BOX(center_column), button_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(center_column), state->position_scale, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(center_column), state->playback_state_label, FALSE, FALSE, 0);

    GtkWidget* right_column = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* volume_label = gtk_label_new("Volume");
    state->volume_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 1.0);
    gtk_range_set_value(GTK_RANGE(state->volume_scale), 100.0);
    gtk_widget_set_size_request(state->volume_scale, 140, -1);
    GtkWidget* fullscreen_button = gtk_button_new_with_label("Fullscreen");

    gtk_box_pack_start(GTK_BOX(right_column), volume_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_column), state->volume_scale, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_column), fullscreen_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(controls), center_column, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(controls), right_column, FALSE, FALSE, 0);

    g_signal_connect(start_button, "clicked", G_CALLBACK(on_return_to_start_clicked), state);
    g_signal_connect(rewind_button, "clicked", G_CALLBACK(on_rewind_clicked), state);
    g_signal_connect(play_pause_button, "clicked", G_CALLBACK(on_play_pause_clicked), state);
    g_signal_connect(stop_button, "clicked", G_CALLBACK(on_stop_clicked), state);
    g_signal_connect(fast_forward_button, "clicked", G_CALLBACK(on_fast_forward_clicked), state);
    g_signal_connect(state->position_scale, "value-changed", G_CALLBACK(on_position_value_changed), state);
    g_signal_connect(state->volume_scale, "value-changed", G_CALLBACK(on_volume_value_changed), state);
    g_signal_connect(fullscreen_button, "clicked", G_CALLBACK(on_fullscreen_button_clicked), state);

    return controls;
}

static void on_preferences_activate(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    GtkWidget* msg = gtk_message_dialog_new(GTK_WINDOW(state->window), GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                                             GTK_BUTTONS_OK, "Preferences UI is not implemented yet.");
    gtk_dialog_run(GTK_DIALOG(msg));
    gtk_widget_destroy(msg);
}

static void on_about_activate(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    gtk_show_about_dialog(GTK_WINDOW(state->window),
                          "program-name", "mate-mpv",
                          "comments", "The spiritual successor to GNOME-Mplayer",
                          "version", "0.1",
                          nullptr);
}

static GtkWidget* create_menu_bar(AppState* state) {
    GtkWidget* menubar = gtk_menu_bar_new();

    GtkWidget* file_menu_item = gtk_menu_item_new_with_mnemonic("_File");
    GtkWidget* edit_menu_item = gtk_menu_item_new_with_mnemonic("_Edit");
    GtkWidget* view_menu_item = gtk_menu_item_new_with_mnemonic("_View");
    GtkWidget* help_menu_item = gtk_menu_item_new_with_mnemonic("_Help");

    GtkWidget* file_menu = gtk_menu_new();
    GtkWidget* edit_menu = gtk_menu_new();
    GtkWidget* view_menu = gtk_menu_new();
    GtkWidget* help_menu = gtk_menu_new();

    GtkWidget* open_files = gtk_menu_item_new_with_label("Open File(s)...");
    GtkWidget* open_tv = gtk_menu_item_new_with_label("Open TV://");
    GtkWidget* quit_item = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(open_files, "activate", G_CALLBACK(on_open_files_activate), state);
    g_signal_connect(open_tv, "activate", G_CALLBACK(on_open_tv_activate), state);
    g_signal_connect_swapped(quit_item, "activate", G_CALLBACK(gtk_window_close), state->window);

    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), open_files);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), open_tv);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), quit_item);

    GtkWidget* prefs_item = gtk_menu_item_new_with_label("Preferences");
    GtkWidget* about_item = gtk_menu_item_new_with_label("About");
    g_signal_connect(prefs_item, "activate", G_CALLBACK(on_preferences_activate), state);
    g_signal_connect(about_item, "activate", G_CALLBACK(on_about_activate), state);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), prefs_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(help_menu), about_item);

    GtkWidget* playlist_item = gtk_check_menu_item_new_with_label("Playlist");
    GtkWidget* fullscreen_item = gtk_check_menu_item_new_with_label("Full Screen");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(playlist_item), TRUE);
    g_signal_connect(playlist_item, "toggled", G_CALLBACK(on_toggle_playlist), state);
    g_signal_connect(fullscreen_item, "toggled", G_CALLBACK(on_toggle_fullscreen), state);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), playlist_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), fullscreen_item);

    state->playlist_toggle_item = playlist_item;
    state->fullscreen_toggle_item = fullscreen_item;

    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_menu_item), file_menu);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(edit_menu_item), edit_menu);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view_menu_item), view_menu);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(help_menu_item), help_menu);

    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file_menu_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), edit_menu_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), view_menu_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), help_menu_item);
    return menubar;
}

static GtkWidget* create_player_context_menu(AppState* state) {
    GtkWidget* menu = gtk_menu_new();

    GtkWidget* pause_item = gtk_menu_item_new_with_label("Play/Pause");
    GtkWidget* stop_item = gtk_menu_item_new_with_label("Stop");
    GtkWidget* open_item = gtk_menu_item_new_with_label("Open");
    GtkWidget* playlist_item = gtk_check_menu_item_new_with_label("Playlist");
    GtkWidget* show_controls_item = gtk_check_menu_item_new_with_label("Show Controls");
    GtkWidget* fullscreen_item = gtk_menu_item_new_with_label("Full Screen");
    GtkWidget* copy_location_item = gtk_menu_item_new_with_label("Copy Location");
    GtkWidget* preferences_item = gtk_menu_item_new_with_label("Preferences");
    GtkWidget* quit_item = gtk_menu_item_new_with_label("Quit");

    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(playlist_item), TRUE);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(show_controls_item), TRUE);

    g_signal_connect(pause_item, "activate", G_CALLBACK(on_play_pause_clicked), state);
    g_signal_connect(stop_item, "activate", G_CALLBACK(on_stop_clicked), state);
    g_signal_connect(open_item, "activate", G_CALLBACK(on_open_files_activate), state);
    g_signal_connect(playlist_item, "toggled", G_CALLBACK(on_toggle_playlist), state);
    g_signal_connect(show_controls_item, "toggled", G_CALLBACK(on_toggle_controls), state);
    g_signal_connect(fullscreen_item, "activate", G_CALLBACK(on_fullscreen_button_clicked), state);
    g_signal_connect(copy_location_item, "activate", G_CALLBACK(on_copy_location_activate), state);
    g_signal_connect(preferences_item, "activate", G_CALLBACK(on_preferences_activate), state);
    g_signal_connect_swapped(quit_item, "activate", G_CALLBACK(gtk_window_close), state->window);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), pause_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), stop_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), open_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), playlist_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), show_controls_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), fullscreen_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), copy_location_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), preferences_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

    gtk_widget_show_all(menu);

    state->show_controls_item = show_controls_item;
    state->context_playlist_toggle_item = playlist_item;
    return menu;
}

static void activate(GtkApplication* app, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    state->app = app;

    state->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(state->window), "mate-mpv");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 1024, 640);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(state->window), root);

    state->menubar = create_menu_bar(state);
    gtk_box_pack_start(GTK_BOX(root), state->menubar, FALSE, FALSE, 0);

    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(root), paned, TRUE, TRUE, 0);

    state->video_area = gtk_gl_area_new();
    gtk_gl_area_set_use_es(GTK_GL_AREA(state->video_area), FALSE);
    gtk_gl_area_set_required_version(GTK_GL_AREA(state->video_area), 3, 2);
    gtk_gl_area_set_auto_render(GTK_GL_AREA(state->video_area), FALSE);
    gtk_widget_set_hexpand(state->video_area, TRUE);
    gtk_widget_set_vexpand(state->video_area, TRUE);
    gtk_widget_set_size_request(state->video_area, 640, 360);
    g_signal_connect(state->video_area, "realize", G_CALLBACK(on_video_area_realize), state);
    g_signal_connect(state->video_area, "unrealize", G_CALLBACK(on_video_area_unrealize), state);
    g_signal_connect(state->video_area, "render", G_CALLBACK(on_video_area_render), state);

    state->playlist_store = gtk_list_store_new(COL_COUNT, G_TYPE_STRING, G_TYPE_STRING);
    state->playlist_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(state->playlist_store));
    GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn* column = gtk_tree_view_column_new_with_attributes("Playlist", renderer, "text", COL_TITLE, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(state->playlist_view), column);
    g_signal_connect(state->playlist_view, "row-activated", G_CALLBACK(on_playlist_row_activated), state);

    state->playlist_scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_size_request(state->playlist_scroller, 260, -1);
    gtk_container_add(GTK_CONTAINER(state->playlist_scroller), state->playlist_view);

    gtk_paned_pack1(GTK_PANED(paned), state->video_area, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), state->playlist_scroller, FALSE, FALSE);

    state->playback_controls = create_playback_controls(state);
    gtk_box_pack_end(GTK_BOX(root), state->playback_controls, FALSE, FALSE, 0);

    state->player_context_menu = create_player_context_menu(state);
    gtk_widget_set_events(state->video_area, gtk_widget_get_events(state->video_area) | GDK_BUTTON_PRESS_MASK);
    g_signal_connect(state->video_area, "button-press-event", G_CALLBACK(on_video_area_button_press), state);

    gtk_widget_add_events(state->window, GDK_POINTER_MOTION_MASK);
    g_signal_connect(state->window, "motion-notify-event", G_CALLBACK(on_window_motion_notify), state);

    gtk_widget_show_all(state->window);
}

static void shutdown_app(GApplication*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);

    if (state->mpv) {
        run_mpv_command(state, {"quit"});
    }

    if (state->mpv_render) {
        mpv_render_context_free(state->mpv_render);
        state->mpv_render = nullptr;
    }

    if (state->mpv) {
        mpv_terminate_destroy(state->mpv);
        state->mpv = nullptr;
    }
}

} // namespace

int main(int argc, char** argv) {
    AppState state;

    GtkApplication* app = gtk_application_new("org.mate.mate-mpv", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &state);
    g_signal_connect(app, "shutdown", G_CALLBACK(shutdown_app), &state);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
