#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

#include <glib/gstdio.h>

namespace {

struct AppState {
    GtkApplication* app = nullptr;
    GtkWidget* window = nullptr;
    GtkWidget* video_area = nullptr;
    GtkWidget* playlist_scroller = nullptr;
    GtkWidget* playlist_view = nullptr;
    GtkListStore* playlist_store = nullptr;
    GtkWidget* playlist_toggle_item = nullptr;
    GtkWidget* fullscreen_toggle_item = nullptr;
    GtkWidget* playback_state_label = nullptr;
    GtkWidget* position_scale = nullptr;
    GtkWidget* volume_scale = nullptr;
    GtkWidget* playback_controls = nullptr;
    GtkWidget* player_context_menu = nullptr;
    GtkWidget* show_controls_item = nullptr;
    GtkWidget* menubar = nullptr;

    GSubprocess* mpv_process = nullptr;
    GSocketClient* socket_client = nullptr;
    GSocketConnection* socket_connection = nullptr;
    GOutputStream* mpv_out = nullptr;

    std::string ipc_socket_path;
    bool fullscreen = false;
    bool suppress_position_seek = false;
    std::string current_media_uri;
};

enum PlaylistColumns {
    COL_TITLE = 0,
    COL_URI,
    COL_COUNT
};

static bool ensure_mpv_running(AppState* state);
static bool launch_mpv_process(AppState* state, GtkWidget* video_widget);

static void set_menubar_visibility(AppState* state, bool visible) {
    if (!state->menubar) {
        return;
    }
    gtk_widget_set_visible(state->menubar, visible);
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

static std::string json_escape(const std::string& input) {
    std::string escaped;
    escaped.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += c; break;
        }
    }
    return escaped;
}

static void send_json_line(AppState* state, const std::string& line) {
    if (!state->mpv_out) {
        return;
    }

    std::string payload = line + "\n";
    GError* error = nullptr;
    g_output_stream_write_all(state->mpv_out, payload.c_str(), payload.size(), nullptr, nullptr, &error);
    g_output_stream_flush(state->mpv_out, nullptr, &error);
    if (error) {
        g_warning("Failed to send command to mpv: %s", error->message);
        g_clear_error(&error);
    }
}

static void connect_ipc_socket(AppState* state) {
    if (state->socket_connection || !state->socket_client || state->ipc_socket_path.empty()) {
        return;
    }

    GError* error = nullptr;
    auto* address = g_unix_socket_address_new(state->ipc_socket_path.c_str());
    state->socket_connection = g_socket_client_connect(state->socket_client, G_SOCKET_CONNECTABLE(address), nullptr, &error);
    g_object_unref(address);

    if (!state->socket_connection) {
        if (error) {
            g_warning("Unable to connect to mpv IPC socket: %s", error->message);
            g_clear_error(&error);
        }
        return;
    }

    state->mpv_out = g_io_stream_get_output_stream(G_IO_STREAM(state->socket_connection));
}

static void send_loadfile(AppState* state, const std::string& uri, const std::string& mode) {
    if (!ensure_mpv_running(state)) {
        g_warning("mpv is not ready yet, skipping loadfile for '%s'", uri.c_str());
        return;
    }

    std::string command = "{\"command\": [\"loadfile\", \"" + json_escape(uri) + "\", \"" + mode + "\"]}";
    send_json_line(state, command);
    if (mode == "replace") {
        state->current_media_uri = uri;
    }
}

static void set_playback_state(AppState* state, const char* status) {
    if (!state->playback_state_label) {
        return;
    }
    std::string text = std::string("Playback state: ") + status;
    gtk_label_set_text(GTK_LABEL(state->playback_state_label), text.c_str());
}

static void send_seek_relative(AppState* state, int seconds) {
    if (!ensure_mpv_running(state)) {
        return;
    }
    std::string command = "{\"command\": [\"seek\", " + std::to_string(seconds) + ", \"relative\"]}";
    send_json_line(state, command);
}

static void send_seek_percent(AppState* state, double percent) {
    if (!ensure_mpv_running(state)) {
        return;
    }
    std::string command = "{\"command\": [\"seek\", " + std::to_string(percent) + ", \"absolute-percent\"]}";
    send_json_line(state, command);
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
    send_json_line(state, "{\"command\": [\"cycle\", \"pause\"]}");
    const char* text = gtk_label_get_text(GTK_LABEL(state->playback_state_label));
    if (g_str_has_suffix(text, "Paused")) {
        set_playback_state(state, "Playing");
    } else {
        set_playback_state(state, "Paused");
    }
}

static void on_stop_clicked(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    if (!ensure_mpv_running(state)) {
        return;
    }
    send_json_line(state, "{\"command\": [\"stop\"]}");
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
    if (state->suppress_position_seek) {
        return;
    }
    send_seek_percent(state, gtk_range_get_value(range));
}

static void on_volume_value_changed(GtkRange* range, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    if (!ensure_mpv_running(state)) {
        return;
    }
    std::string command = "{\"command\": [\"set_property\", \"volume\", " +
                          std::to_string(gtk_range_get_value(range)) + "]}";
    send_json_line(state, command);
}

static void on_fullscreen_button_clicked(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    set_fullscreen_state(state, !state->fullscreen);
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
    gboolean active = gtk_check_menu_item_get_active(item);
    if (state->playback_controls) {
        gtk_widget_set_visible(state->playback_controls, active);
    }
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
    gtk_list_store_set(state->playlist_store, &iter,
                       COL_TITLE, title.c_str(),
                       COL_URI, uri.c_str(),
                       -1);
}

static std::vector<std::string> selected_files_from_dialog(GtkWindow* parent) {
    std::vector<std::string> files;
    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        "Open Media Files",
        parent,
        GTK_FILE_CHOOSER_ACTION_OPEN,
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
    auto files = selected_files_from_dialog(GTK_WINDOW(state->window));

    if (files.empty()) {
        return;
    }

    for (size_t i = 0; i < files.size(); ++i) {
        const auto& path = files[i];
        const auto title_start = path.find_last_of("/");
        const std::string title = (title_start == std::string::npos) ? path : path.substr(title_start + 1);
        add_playlist_entry(state, title, path);
        send_loadfile(state, path, i == 0 ? "replace" : "append-play");
    }
}

static std::vector<std::pair<std::string, std::string>> read_channels_conf() {
    std::vector<std::pair<std::string, std::string>> channels;
    const char* home = g_get_home_dir();
    std::string conf_path = std::string(home ? home : "") + "/.config/mpv/channels.conf";

    std::ifstream input(conf_path);
    if (!input.is_open()) {
        return channels;
    }

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        auto delim = line.find(':');
        if (delim == std::string::npos || delim == 0 || delim == line.size() - 1) {
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
    auto channels = read_channels_conf();

    if (channels.empty()) {
        GtkWidget* msg = gtk_message_dialog_new(GTK_WINDOW(state->window),
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING,
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
}

static void on_toggle_fullscreen(GtkCheckMenuItem* item, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    gboolean active = gtk_check_menu_item_get_active(item);
    set_fullscreen_state(state, active);
}

static gboolean on_window_motion_notify(GtkWidget*, GdkEventMotion* event, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    if (!state->fullscreen || !state->menubar || !event) {
        return FALSE;
    }

    const bool near_top_edge = event->y <= 1.0;
    set_menubar_visibility(state, near_top_edge);
    return FALSE;
}

static void on_menu_placeholder_activate(GtkWidget* widget, gpointer) {
    const char* label = gtk_menu_item_get_label(GTK_MENU_ITEM(widget));
    g_message("Menu action '%s' is not implemented yet", label ? label : "(unknown)");
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
    gtk_scale_set_draw_value(GTK_SCALE(state->volume_scale), TRUE);
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

    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        "Preferences",
        GTK_WINDOW(state->window),
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        "_Close", GTK_RESPONSE_CLOSE,
        nullptr);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget* notebook = gtk_notebook_new();

    GtkWidget* audio_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget* video_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

    gtk_container_set_border_width(GTK_CONTAINER(audio_box), 12);
    gtk_container_set_border_width(GTK_CONTAINER(video_box), 12);

    gtk_box_pack_start(GTK_BOX(audio_box), gtk_label_new("Audio settings go here."), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(video_box), gtk_label_new("Video settings go here."), FALSE, FALSE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), audio_box, gtk_label_new("Audio"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), video_box, gtk_label_new("Video"));

    gtk_box_pack_start(GTK_BOX(content), notebook, TRUE, TRUE, 0);
    gtk_widget_set_size_request(dialog, 420, 280);

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static bool launch_mpv_process(AppState* state, GtkWidget* video_widget) {
    if (state->mpv_process) {
        return true;
    }

    if (!GTK_IS_WIDGET(video_widget) || !gtk_widget_get_realized(video_widget)) {
        return false;
    }

    GdkWindow* gdk_window = gtk_widget_get_window(video_widget);
#ifndef GDK_WINDOWING_X11
    (void)gdk_window;
#endif

    gchar* pid_component = g_strdup_printf("%d", getpid());
    state->ipc_socket_path = std::string(g_get_tmp_dir()) + "/mate-mpv-" + pid_component + ".sock";
    g_free(pid_component);

    std::string ipc_arg = "--input-ipc-server=" + state->ipc_socket_path;
    std::vector<std::string> args = {
        "mpv",
        "--idle=yes",
        "--force-window=yes",
        "--keep-open=yes"
    };

#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_WINDOW(gdk_window)) {
        guint32 xid = gdk_x11_window_get_xid(gdk_window);
        args.emplace_back("--wid=" + std::to_string(xid));
    } else
#endif
    {
        g_message("Running on a non-X11 backend; mpv will use its own window on this platform.");
    }

    args.push_back(ipc_arg);

    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);

    GError* error = nullptr;
    state->mpv_process = g_subprocess_newv(argv.data(),
                                           static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE),
                                           &error);
    if (!state->mpv_process) {
        g_warning("Failed to launch mpv: %s", error ? error->message : "unknown error");
        g_clear_error(&error);
        return false;
    }

    return true;
}

static void on_video_area_realize(GtkWidget* widget, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    launch_mpv_process(state, widget);
    ensure_mpv_running(state);
}

static bool ensure_mpv_running(AppState* state) {
    if (!state->mpv_process) {
        if (!launch_mpv_process(state, state->video_area)) {
            return false;
        }
    }

    if (state->socket_client == nullptr) {
        state->socket_client = g_socket_client_new();
    }

    if (state->socket_connection) {
        return true;
    }

    for (int i = 0; i < 20 && !state->socket_connection; ++i) {
        connect_ipc_socket(state);
        if (!state->socket_connection) {
            g_usleep(50 * 1000);
        }
    }

    return state->socket_connection != nullptr;
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
    GtkWidget* media_info_item = gtk_menu_item_new_with_label("Media Info");
    GtkWidget* details_item = gtk_menu_item_new_with_label("Details");
    GtkWidget* audio_meter_item = gtk_menu_item_new_with_label("Audio Meter");
    GtkWidget* fullscreen_item = gtk_menu_item_new_with_label("FullScreen");
    GtkWidget* normal_size_item = gtk_menu_item_new_with_label("Normal (1:1)");
    GtkWidget* double_size_item = gtk_menu_item_new_with_label("Double Size (2:1)");
    GtkWidget* half_size_item = gtk_menu_item_new_with_label("Half Size (1:2)");
    GtkWidget* half_larger_item = gtk_menu_item_new_with_label("Half Larger (1.5:1)");
    GtkWidget* aspect_item = gtk_menu_item_new_with_label("Aspect");
    GtkWidget* aspect_menu = gtk_menu_new();
    GtkWidget* aspect_3_2_item = gtk_menu_item_new_with_label("3:2");
    GtkWidget* aspect_4_3_item = gtk_menu_item_new_with_label("4:3");
    GtkWidget* aspect_5_4_item = gtk_menu_item_new_with_label("5:4");
    GtkWidget* aspect_9_16_item = gtk_menu_item_new_with_label("9:16");
    GtkWidget* aspect_16_9_item = gtk_menu_item_new_with_label("16:9");
    GtkWidget* aspect_16_10_item = gtk_menu_item_new_with_label("16:10");
    GtkWidget* aspect_21_9_item = gtk_menu_item_new_with_label("21:9");
    GtkWidget* aspect_185_1_item = gtk_menu_item_new_with_label("1.85:1");
    GtkWidget* aspect_235_1_item = gtk_menu_item_new_with_label("2.35:1");
    GtkWidget* subtitles_item = gtk_check_menu_item_new_with_label("Show subtitles");
    GtkWidget* subtitle_smaller_item = gtk_menu_item_new_with_label("Decrease Subtitle Size");
    GtkWidget* subtitle_larger_item = gtk_menu_item_new_with_label("Increase Subtitle Size");
    GtkWidget* subtitle_delay_down_item = gtk_menu_item_new_with_label("Decrease Subtitle Delay");
    GtkWidget* subtitle_delay_up_item = gtk_menu_item_new_with_label("Increase Subtitle Delay");
    GtkWidget* switch_angle_item = gtk_menu_item_new_with_label("Switch Angle Ctrl-A");
    GtkWidget* controls_item = gtk_check_menu_item_new_with_label("Controls C");
    GtkWidget* video_picture_adjustments_item = gtk_menu_item_new_with_label("Video Picture Adjustments.");

    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(playlist_item), TRUE);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(subtitles_item), TRUE);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(controls_item), TRUE);

    g_signal_connect(playlist_item, "toggled", G_CALLBACK(on_toggle_playlist), state);
    g_signal_connect(media_info_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(details_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(audio_meter_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(fullscreen_item, "activate", G_CALLBACK(on_fullscreen_button_clicked), state);
    g_signal_connect(normal_size_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(double_size_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(half_size_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(half_larger_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(aspect_3_2_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(aspect_4_3_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(aspect_5_4_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(aspect_9_16_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(aspect_16_9_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(aspect_16_10_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(aspect_21_9_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(aspect_185_1_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(aspect_235_1_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(subtitles_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(subtitle_smaller_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(subtitle_larger_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(subtitle_delay_down_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(subtitle_delay_up_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(switch_angle_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);
    g_signal_connect(controls_item, "toggled", G_CALLBACK(on_toggle_controls), state);
    g_signal_connect(video_picture_adjustments_item, "activate", G_CALLBACK(on_menu_placeholder_activate), nullptr);

    gtk_menu_item_set_submenu(GTK_MENU_ITEM(aspect_item), aspect_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(aspect_menu), aspect_3_2_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(aspect_menu), aspect_4_3_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(aspect_menu), aspect_5_4_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(aspect_menu), aspect_9_16_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(aspect_menu), aspect_16_9_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(aspect_menu), aspect_16_10_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(aspect_menu), aspect_21_9_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(aspect_menu), aspect_185_1_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(aspect_menu), aspect_235_1_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), playlist_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), media_info_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), details_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), audio_meter_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), fullscreen_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), normal_size_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), double_size_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), half_size_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), half_larger_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), aspect_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), subtitles_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), subtitle_smaller_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), subtitle_larger_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), subtitle_delay_down_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), subtitle_delay_up_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), switch_angle_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), controls_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), video_picture_adjustments_item);

    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_menu_item), file_menu);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(edit_menu_item), edit_menu);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view_menu_item), view_menu);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(help_menu_item), help_menu);

    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file_menu_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), edit_menu_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), view_menu_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), help_menu_item);

    state->playlist_toggle_item = playlist_item;
    state->fullscreen_toggle_item = nullptr;

    return menubar;
}


static GtkWidget* create_player_context_menu(AppState* state) {
    GtkWidget* menu = gtk_menu_new();

    GtkWidget* pause_item = gtk_menu_item_new_with_label("Pause");
    GtkWidget* stop_item = gtk_menu_item_new_with_label("Stop");
    GtkWidget* open_item = gtk_menu_item_new_with_label("Open Ctrl+O");
    GtkWidget* show_controls_item = gtk_check_menu_item_new_with_label("Show Controls");
    GtkWidget* fullscreen_item = gtk_menu_item_new_with_label("Full Screen");
    GtkWidget* copy_location_item = gtk_menu_item_new_with_label("Copy Location");
    GtkWidget* preferences_item = gtk_menu_item_new_with_label("Preferences");
    GtkWidget* quit_item = gtk_menu_item_new_with_label("Quit Ctrl+Q");

    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(show_controls_item), TRUE);

    g_signal_connect(pause_item, "activate", G_CALLBACK(on_play_pause_clicked), state);
    g_signal_connect(stop_item, "activate", G_CALLBACK(on_stop_clicked), state);
    g_signal_connect(open_item, "activate", G_CALLBACK(on_open_files_activate), state);
    g_signal_connect(show_controls_item, "toggled", G_CALLBACK(on_toggle_controls), state);
    g_signal_connect(fullscreen_item, "activate", G_CALLBACK(on_fullscreen_button_clicked), state);
    g_signal_connect(copy_location_item, "activate", G_CALLBACK(on_copy_location_activate), state);
    g_signal_connect(preferences_item, "activate", G_CALLBACK(on_preferences_activate), state);
    g_signal_connect_swapped(quit_item, "activate", G_CALLBACK(gtk_window_close), state->window);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), pause_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), stop_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), open_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), show_controls_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), fullscreen_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), copy_location_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), preferences_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

    gtk_widget_show_all(menu);

    state->show_controls_item = show_controls_item;
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

    GtkWidget* menubar = create_menu_bar(state);
    state->menubar = menubar;
    gtk_box_pack_start(GTK_BOX(root), menubar, FALSE, FALSE, 0);

    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(root), paned, TRUE, TRUE, 0);

    state->video_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(state->video_area, TRUE);
    gtk_widget_set_vexpand(state->video_area, TRUE);
    gtk_widget_set_app_paintable(state->video_area, TRUE);
    gtk_widget_set_size_request(state->video_area, 640, 360);
    g_signal_connect(state->video_area, "realize", G_CALLBACK(on_video_area_realize), state);

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

    if (state->mpv_out) {
        send_json_line(state, "{\"command\": [\"quit\"]}");
    }

    if (state->socket_connection) {
        g_object_unref(state->socket_connection);
        state->socket_connection = nullptr;
    }

    if (state->socket_client) {
        g_object_unref(state->socket_client);
        state->socket_client = nullptr;
    }

    if (state->mpv_process) {
        g_subprocess_force_exit(state->mpv_process);
        g_object_unref(state->mpv_process);
        state->mpv_process = nullptr;
    }

    if (!state->ipc_socket_path.empty()) {
        g_unlink(state->ipc_socket_path.c_str());
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
