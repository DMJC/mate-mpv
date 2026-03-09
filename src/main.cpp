#include <gtk/gtk.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <glib/gstdio.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif

#include "langlist.h"

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct AppState {
    GtkApplication* app = nullptr;
    GtkWidget* window = nullptr;
    GtkWidget* video_area = nullptr;
    GtkWidget* playlist_sidebar = nullptr;
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
    GtkWidget* play_pause_image = nullptr;
    GtkWidget* player_context_menu = nullptr;
    GtkWidget* show_controls_item = nullptr;
    GtkWidget* menubar = nullptr;
    GtkWidget* media_details_window = nullptr;
    GtkWidget* media_details_label = nullptr;
    GtkWidget* picture_adjustments_window = nullptr;

    mpv_handle* mpv = nullptr;
    mpv_render_context* mpv_render = nullptr;

#ifdef GDK_WINDOWING_WAYLAND
    wl_display* wl_display_handle = nullptr;
#endif

    bool fullscreen = false;
    bool suppress_position_seek = false;
    guint position_update_source = 0;
    std::string current_media_uri;
};

struct AppConfig {
    int window_width = 1280;
    int window_height = 720;
    bool playlist_visible = true;
    bool controls_visible = true;
    bool fullscreen = false;
    double volume = 100.0;
    std::string audio_language;
    std::string subtitle_language;
};

enum PlaylistColumns {
    COL_TITLE = 0,
    COL_URI,
    COL_COUNT
};

static void on_open_files_activate(GtkWidget*, gpointer user_data);
static void on_open_url_activate(GtkWidget*, gpointer user_data);
static std::string get_mpv_string_property(AppState* state, const char* property);

static std::string file_basename(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

static void set_playback_state(AppState* state, const char* status) {
    if (!state->playback_state_label) {
        return;
    }
    std::string text = std::string("Playback state: ") + status;
    gtk_label_set_text(GTK_LABEL(state->playback_state_label), text.c_str());

    if (state->play_pause_image) {
        const char* icon_name = g_strcmp0(status, "Playing") == 0
            ? "media-playback-pause"
            : "media-playback-start";
        gtk_image_set_from_icon_name(GTK_IMAGE(state->play_pause_image), icon_name, GTK_ICON_SIZE_BUTTON);
    }
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

static std::string app_config_path() {
    return std::string(g_get_home_dir()) + "/.config/mate/mate-mpv/mate-mpv.conf";
}

static AppConfig load_app_config() {
    AppConfig config;
    GKeyFile* key_file = g_key_file_new();
    GError* error = nullptr;
    const std::string path = app_config_path();

    if (!g_key_file_load_from_file(key_file, path.c_str(), G_KEY_FILE_NONE, &error)) {
        if (error && !(error->domain == G_FILE_ERROR && error->code == G_FILE_ERROR_NOENT)) {
            g_warning("Failed to load config '%s': %s", path.c_str(), error->message);
        }
        if (error) {
            g_error_free(error);
        }
        g_key_file_unref(key_file);
        return config;
    }

    if (g_key_file_has_key(key_file, "window", "width", nullptr)) {
        config.window_width = g_key_file_get_integer(key_file, "window", "width", nullptr);
    }
    if (g_key_file_has_key(key_file, "window", "height", nullptr)) {
        config.window_height = g_key_file_get_integer(key_file, "window", "height", nullptr);
    }
    if (g_key_file_has_key(key_file, "interface", "playlist_visible", nullptr)) {
        config.playlist_visible = g_key_file_get_boolean(key_file, "interface", "playlist_visible", nullptr);
    }
    if (g_key_file_has_key(key_file, "interface", "controls_visible", nullptr)) {
        config.controls_visible = g_key_file_get_boolean(key_file, "interface", "controls_visible", nullptr);
    }
    if (g_key_file_has_key(key_file, "window", "fullscreen", nullptr)) {
        config.fullscreen = g_key_file_get_boolean(key_file, "window", "fullscreen", nullptr);
    }
    if (g_key_file_has_key(key_file, "player", "volume", nullptr)) {
        config.volume = g_key_file_get_double(key_file, "player", "volume", nullptr);
    }
    if (g_key_file_has_key(key_file, "player", "audio_language", nullptr)) {
        gchar* audio_language = g_key_file_get_string(key_file, "player", "audio_language", nullptr);
        if (audio_language) {
            config.audio_language = audio_language;
            g_free(audio_language);
        }
    }
    if (g_key_file_has_key(key_file, "player", "subtitle_language", nullptr)) {
        gchar* subtitle_language = g_key_file_get_string(key_file, "player", "subtitle_language", nullptr);
        if (subtitle_language) {
            config.subtitle_language = subtitle_language;
            g_free(subtitle_language);
        }
    }

    g_key_file_unref(key_file);

    if (config.window_width < 320) {
        config.window_width = 320;
    }
    if (config.window_height < 240) {
        config.window_height = 240;
    }
    if (config.volume < 0.0) {
        config.volume = 0.0;
    } else if (config.volume > 100.0) {
        config.volume = 100.0;
    }

    return config;
}

static void save_app_config(AppState* state) {
    if (!state || !state->window) {
        return;
    }

    const std::string path = app_config_path();
    const std::string directory = path.substr(0, path.find_last_of('/'));
    if (g_mkdir_with_parents(directory.c_str(), 0755) != 0) {
        g_warning("Failed to create config directory '%s'", directory.c_str());
        return;
    }

    GKeyFile* key_file = g_key_file_new();
    int width = 0;
    int height = 0;
    gtk_window_get_size(GTK_WINDOW(state->window), &width, &height);

    g_key_file_set_integer(key_file, "window", "width", width);
    g_key_file_set_integer(key_file, "window", "height", height);
    g_key_file_set_boolean(key_file, "window", "fullscreen", state->fullscreen);
    g_key_file_set_boolean(key_file,
                           "interface",
                           "playlist_visible",
                           state->playlist_sidebar ? gtk_widget_get_visible(state->playlist_sidebar) : true);
    g_key_file_set_boolean(key_file,
                           "interface",
                           "controls_visible",
                           state->playback_controls ? gtk_widget_get_visible(state->playback_controls) : true);
    g_key_file_set_double(key_file,
                          "player",
                          "volume",
                          state->volume_scale ? gtk_range_get_value(GTK_RANGE(state->volume_scale)) : 100.0);

    const std::string audio_language = state->mpv ? get_mpv_string_property(state, "alang") : "";
    const std::string subtitle_language = state->mpv ? get_mpv_string_property(state, "slang") : "";
    g_key_file_set_string(key_file, "player", "audio_language", audio_language.c_str());
    g_key_file_set_string(key_file, "player", "subtitle_language", subtitle_language.c_str());

    gsize length = 0;
    gchar* serialized = g_key_file_to_data(key_file, &length, nullptr);
    g_key_file_unref(key_file);
    if (!serialized) {
        g_warning("Failed to serialize app config");
        return;
    }

    GError* error = nullptr;
    if (!g_file_set_contents(path.c_str(), serialized, static_cast<gssize>(length), &error)) {
        g_warning("Failed to save config '%s': %s", path.c_str(), error ? error->message : "unknown error");
        if (error) {
            g_error_free(error);
        }
    }

    g_free(serialized);
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
    const int status = mpv_command(state->mpv, args.data());
    if (status < 0) {
        g_warning("mpv command failed: %s", mpv_error_string(status));
    }
}

static bool get_mpv_flag_property(AppState* state, const char* property, bool fallback = false) {
    if (!ensure_mpv_running(state)) {
        return fallback;
    }

    int flag = fallback ? 1 : 0;
    if (mpv_get_property(state->mpv, property, MPV_FORMAT_FLAG, &flag) < 0) {
        return fallback;
    }
    return flag != 0;
}

static bool get_mpv_double_property(AppState* state, const char* property, double* value) {
    if (!value || !ensure_mpv_running(state)) {
        return false;
    }
    return mpv_get_property(state->mpv, property, MPV_FORMAT_DOUBLE, value) >= 0;
}

static std::string get_mpv_string_property(AppState* state, const char* property) {
    if (!ensure_mpv_running(state)) {
        return "";
    }

    char* value = mpv_get_property_string(state->mpv, property);
    if (!value) {
        return "";
    }

    std::string output(value);
    mpv_free(value);
    return output;
}

static void set_mpv_flag_property(AppState* state, const char* property, bool value) {
    if (!ensure_mpv_running(state)) {
        return;
    }

    int flag = value ? 1 : 0;
    if (mpv_set_property(state->mpv, property, MPV_FORMAT_FLAG, &flag) < 0) {
        g_warning("Failed to set property '%s'", property);
    }
}

static void set_mpv_double_property(AppState* state, const char* property, double value) {
    if (!ensure_mpv_running(state)) {
        return;
    }

    if (mpv_set_property(state->mpv, property, MPV_FORMAT_DOUBLE, &value) < 0) {
        g_warning("Failed to set property '%s'", property);
    }
}

static void set_mpv_string_property(AppState* state, const char* property, const std::string& value) {
    if (!ensure_mpv_running(state)) {
        return;
    }

    if (mpv_set_property_string(state->mpv, property, value.c_str()) < 0) {
        g_warning("Failed to set property '%s'", property);
    }
}

static void send_loadfile(AppState* state, const std::string& uri, const std::string& mode) {
    run_mpv_command(state, {"loadfile", uri.c_str(), mode.c_str()});
    if (ensure_mpv_running(state)) {
        int pause = 0;
        mpv_set_property(state->mpv, "pause", MPV_FORMAT_FLAG, &pause);
    }
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

    const int scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(area));
    const int width = gtk_widget_get_allocated_width(GTK_WIDGET(area)) * scale_factor;
    const int height = gtk_widget_get_allocated_height(GTK_WIDGET(area)) * scale_factor;

    GLint bound_fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound_fbo);

    mpv_opengl_fbo fbo{.fbo = bound_fbo, .w = width, .h = height, .internal_format = 0};
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
    run_mpv_command(state, {"cycle", "pause"});

    const char* label = gtk_label_get_text(GTK_LABEL(state->playback_state_label));
    if (g_str_has_suffix(label, "Paused")) {
        set_playback_state(state, "Playing");
    } else {
        set_playback_state(state, "Paused");
    }
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

static void on_skip_backward_clicked(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    run_mpv_command(state, {"playlist-prev", "force"});
    set_playback_state(state, "Playing");
}

static void on_skip_forward_clicked(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    run_mpv_command(state, {"playlist-next", "force"});
    set_playback_state(state, "Playing");
}

static void on_position_value_changed(GtkRange* range, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    if (!state->suppress_position_seek) {
        send_seek_percent(state, gtk_range_get_value(range));
    }
}

static gboolean update_position_scale(gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    if (!state->position_scale || !state->mpv) {
        return G_SOURCE_CONTINUE;
    }

    double duration = 0.0;
    double time_pos = 0.0;
    if (mpv_get_property(state->mpv, "duration", MPV_FORMAT_DOUBLE, &duration) < 0 || duration <= 0.0) {
        return G_SOURCE_CONTINUE;
    }
    if (mpv_get_property(state->mpv, "time-pos", MPV_FORMAT_DOUBLE, &time_pos) < 0) {
        return G_SOURCE_CONTINUE;
    }

    double percent = (time_pos / duration) * 100.0;
    if (percent < 0.0) {
        percent = 0.0;
    } else if (percent > 100.0) {
        percent = 100.0;
    }

    state->suppress_position_seek = true;
    gtk_range_set_value(GTK_RANGE(state->position_scale), percent);
    state->suppress_position_seek = false;
    return G_SOURCE_CONTINUE;
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

static void show_message_dialog(GtkWindow* parent, GtkMessageType type, const char* message) {
    GtkWidget* dialog = gtk_message_dialog_new(parent,
                                               GTK_DIALOG_MODAL,
                                               type,
                                               GTK_BUTTONS_OK,
                                               "%s",
                                               message);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static bool dump_audio_to_wav(AppState* state, const std::string& output_file, std::string* error) {
    std::string source_uri = get_mpv_string_property(state, "path");
    if (source_uri.empty()) {
        source_uri = state->current_media_uri;
    }
    if (source_uri.empty()) {
        if (error) {
            *error = "No media is currently loaded.";
        }
        return false;
    }

    mpv_handle* dumper = mpv_create();
    if (!dumper) {
        if (error) {
            *error = "Failed to create mpv instance for audio dump.";
        }
        return false;
    }

    mpv_set_option_string(dumper, "terminal", "no");
    mpv_set_option_string(dumper, "vid", "no");
    mpv_set_option_string(dumper, "audio-display", "no");
    mpv_set_option_string(dumper, "ao", "pcm");
    mpv_set_option_string(dumper, "ao-pcm-file", output_file.c_str());
    mpv_set_option_string(dumper, "ao-pcm-waveheader", "yes");

    int status = mpv_initialize(dumper);
    if (status < 0) {
        if (error) {
            *error = std::string("Failed to initialize mpv for audio dump: ") + mpv_error_string(status);
        }
        mpv_terminate_destroy(dumper);
        return false;
    }

    const char* load_cmd[] = {"loadfile", source_uri.c_str(), "replace", nullptr};
    status = mpv_command(dumper, load_cmd);
    if (status < 0) {
        if (error) {
            *error = std::string("Failed to start audio dump: ") + mpv_error_string(status);
        }
        mpv_terminate_destroy(dumper);
        return false;
    }

    for (;;) {
        mpv_event* event = mpv_wait_event(dumper, 0.1);
        if (!event) {
            continue;
        }
        if (event->event_id == MPV_EVENT_END_FILE) {
            auto* end_file = static_cast<mpv_event_end_file*>(event->data);
            if (end_file && end_file->error < 0 && error) {
                *error = std::string("Audio dump failed: ") + mpv_error_string(end_file->error);
                mpv_terminate_destroy(dumper);
                return false;
            }
            break;
        }
        if (event->event_id == MPV_EVENT_SHUTDOWN) {
            break;
        }
    }

    mpv_terminate_destroy(dumper);
    return true;
}

static void on_dump_audio_activate(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    GtkWidget* dialog = gtk_file_chooser_dialog_new("Dump Audio",
                                                     GTK_WINDOW(state->window),
                                                     GTK_FILE_CHOOSER_ACTION_SAVE,
                                                     "_Cancel",
                                                     GTK_RESPONSE_CANCEL,
                                                     "_Save",
                                                     GTK_RESPONSE_ACCEPT,
                                                     nullptr);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "audio_dump.wav");

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            std::string output(filename);
            g_free(filename);

            if (!g_str_has_suffix(output.c_str(), ".wav")) {
                output += ".wav";
            }

            std::string error;
            if (dump_audio_to_wav(state, output, &error)) {
                std::string message = std::string("Audio dumped to:\n") + output;
                show_message_dialog(GTK_WINDOW(state->window), GTK_MESSAGE_INFO, message.c_str());
            } else {
                show_message_dialog(GTK_WINDOW(state->window), GTK_MESSAGE_ERROR, error.c_str());
            }
        }
    }

    gtk_widget_destroy(dialog);
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

static gboolean on_window_key_press(GtkWidget*, GdkEventKey* event, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    if (!event) {
        return FALSE;
    }

    const guint modifiers = event->state & gtk_accelerator_get_default_mod_mask();
    if (modifiers == GDK_CONTROL_MASK && (event->keyval == GDK_KEY_o || event->keyval == GDK_KEY_O)) {
        on_open_files_activate(nullptr, state);
        return TRUE;
    }

    if (modifiers != 0) {
        return FALSE;
    }

    if (event->keyval == GDK_KEY_f || event->keyval == GDK_KEY_F) {
        set_fullscreen_state(state, !state->fullscreen);
        return TRUE;
    }

    if (event->keyval == GDK_KEY_space) {
        on_play_pause_clicked(nullptr, state);
        return TRUE;
    }

    if (event->keyval == GDK_KEY_c || event->keyval == GDK_KEY_C) {
        if (state->show_controls_item) {
            const gboolean controls_visible = gtk_widget_get_visible(state->playback_controls);
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(state->show_controls_item), !controls_visible);
        } else {
            gtk_widget_set_visible(state->playback_controls, !gtk_widget_get_visible(state->playback_controls));
        }
        return TRUE;
    }

    if (event->keyval == GDK_KEY_p || event->keyval == GDK_KEY_P) {
        const gboolean playlist_visible = gtk_widget_get_visible(state->playlist_sidebar);
        if (state->playlist_toggle_item) {
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(state->playlist_toggle_item), !playlist_visible);
        } else {
            gtk_widget_set_visible(state->playlist_sidebar, !playlist_visible);
        }
        return TRUE;
    }

    return FALSE;
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
        add_playlist_entry(state, file_basename(path), path);
        send_loadfile(state, path, i == 0 ? "replace" : "append-play");
    }
}

static void on_open_url_activate(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    GtkWidget* dialog = gtk_dialog_new_with_buttons("Open URL/Stream",
                                                     GTK_WINDOW(state->window),
                                                     static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                                                     "_Cancel", GTK_RESPONSE_CANCEL,
                                                     "_Open", GTK_RESPONSE_ACCEPT,
                                                     nullptr);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "https://example.com/stream");
    gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 8);
    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(entry);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        const gchar* uri_text = gtk_entry_get_text(GTK_ENTRY(entry));
        if (uri_text) {
            const std::string uri = trim_copy(uri_text);
            if (!uri.empty()) {
                add_playlist_entry(state, uri, uri);
                send_loadfile(state, uri, "append-play");
            }
        }
    }

    gtk_widget_destroy(dialog);
}

static std::vector<std::string> load_playlist_paths_from_file(const std::string& filename) {
    std::vector<std::string> paths;
    std::ifstream input(filename);
    std::string line;
    while (std::getline(input, line)) {
        std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        paths.push_back(trimmed);
    }
    return paths;
}

static void save_playlist_to_file(AppState* state, const std::string& filename) {
    std::ofstream output(filename);
    if (!output) {
        return;
    }

    output << "#EXTM3U\n";
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(state->playlist_store), &iter);
    while (valid) {
        gchar* uri = nullptr;
        gtk_tree_model_get(GTK_TREE_MODEL(state->playlist_store), &iter, COL_URI, &uri, -1);
        if (uri) {
            output << uri << '\n';
            g_free(uri);
        }
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(state->playlist_store), &iter);
    }
}

static void on_load_playlist_clicked(GtkButton*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    GtkWidget* dialog = gtk_file_chooser_dialog_new("Load Playlist", GTK_WINDOW(state->window), GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     "_Cancel", GTK_RESPONSE_CANCEL,
                                                     "_Open", GTK_RESPONSE_ACCEPT,
                                                     nullptr);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            const auto paths = load_playlist_paths_from_file(filename);
            gtk_list_store_clear(state->playlist_store);
            for (size_t i = 0; i < paths.size(); ++i) {
                add_playlist_entry(state, file_basename(paths[i]), paths[i]);
                send_loadfile(state, paths[i], i == 0 ? "replace" : "append-play");
            }
            g_free(filename);
        }
    }
    gtk_widget_destroy(dialog);
}

static void on_save_playlist_clicked(GtkButton*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    GtkWidget* dialog = gtk_file_chooser_dialog_new("Save Playlist", GTK_WINDOW(state->window), GTK_FILE_CHOOSER_ACTION_SAVE,
                                                     "_Cancel", GTK_RESPONSE_CANCEL,
                                                     "_Save", GTK_RESPONSE_ACCEPT,
                                                     nullptr);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "playlist.m3u");
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            save_playlist_to_file(state, filename);
            g_free(filename);
        }
    }
    gtk_widget_destroy(dialog);
}

static void on_add_files_to_playlist_clicked(GtkButton*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    const auto files = selected_files_from_dialog(GTK_WINDOW(state->window));
    for (const auto& path : files) {
        add_playlist_entry(state, file_basename(path), path);
        send_loadfile(state, path, "append-play");
    }
}

static void on_remove_selected_playlist_clicked(GtkButton*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(state->playlist_view));
    GList* rows = gtk_tree_selection_get_selected_rows(selection, nullptr);
    for (GList* item = g_list_last(rows); item != nullptr; item = item->prev) {
        GtkTreePath* path = static_cast<GtkTreePath*>(item->data);
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(state->playlist_store), &iter, path)) {
            gtk_list_store_remove(state->playlist_store, &iter);
        }
    }
    g_list_free_full(rows, reinterpret_cast<GDestroyNotify>(gtk_tree_path_free));
}

static void move_selected_row(AppState* state, bool up) {
    GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(state->playlist_view));
    GtkTreeModel* model = GTK_TREE_MODEL(state->playlist_store);
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, nullptr, &iter)) {
        return;
    }

    GtkTreePath* path = gtk_tree_model_get_path(model, &iter);
    if (!path) {
        return;
    }

    int* indices = gtk_tree_path_get_indices(path);
    if (!indices) {
        gtk_tree_path_free(path);
        return;
    }
    const int current = indices[0];
    const int target = up ? current - 1 : current + 1;
    const int total = gtk_tree_model_iter_n_children(model, nullptr);
    if (target < 0 || target >= total) {
        gtk_tree_path_free(path);
        return;
    }

    GtkTreePath* target_path = gtk_tree_path_new_from_indices(target, -1);
    GtkTreeIter target_iter;
    if (gtk_tree_model_get_iter(model, &target_iter, target_path)) {
        if (up) {
            gtk_list_store_swap(state->playlist_store, &iter, &target_iter);
        } else {
            gtk_list_store_swap(state->playlist_store, &target_iter, &iter);
        }
        GtkTreePath* new_selected_path = gtk_tree_path_new_from_indices(target, -1);
        gtk_tree_selection_unselect_all(selection);
        gtk_tree_selection_select_path(selection, new_selected_path);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(state->playlist_view), new_selected_path, nullptr, FALSE, 0.0f, 0.0f);
        gtk_tree_path_free(new_selected_path);
    }

    gtk_tree_path_free(target_path);
    gtk_tree_path_free(path);
}

static void on_move_playlist_up_clicked(GtkButton*, gpointer user_data) {
    move_selected_row(static_cast<AppState*>(user_data), true);
}

static void on_move_playlist_down_clicked(GtkButton*, gpointer user_data) {
    move_selected_row(static_cast<AppState*>(user_data), false);
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
    gtk_widget_set_visible(state->playlist_sidebar, active);

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

static std::string format_mpv_double(double value, int precision = 2) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

static void refresh_media_details(AppState* state) {
    if (!state->media_details_label) {
        return;
    }

    std::ostringstream details;
    const std::string title = get_mpv_string_property(state, "media-title");
    const std::string path = get_mpv_string_property(state, "path");
    const std::string video_codec = get_mpv_string_property(state, "video-codec");
    const std::string audio_codec = get_mpv_string_property(state, "audio-codec-name");

    details << "Title: " << (title.empty() ? "(unknown)" : title) << '\n';
    details << "Path: " << (path.empty() ? "(unknown)" : path) << '\n';

    double duration = 0.0;
    if (get_mpv_double_property(state, "duration", &duration) && duration > 0.0) {
        details << "Duration: " << format_mpv_double(duration, 1) << " s\n";
    } else {
        details << "Duration: (unknown)\n";
    }

    double width = 0.0;
    double height = 0.0;
    if (get_mpv_double_property(state, "width", &width) && get_mpv_double_property(state, "height", &height) && width > 0 &&
        height > 0) {
        details << "Video: " << static_cast<int>(width) << "x" << static_cast<int>(height);
        double fps = 0.0;
        if (get_mpv_double_property(state, "estimated-vf-fps", &fps) && fps > 0.0) {
            details << " @ " << format_mpv_double(fps) << " fps";
        }
        if (!video_codec.empty()) {
            details << " (" << video_codec << ")";
        }
        details << '\n';
    } else {
        details << "Video: (none)\n";
    }

    if (!audio_codec.empty()) {
        details << "Audio codec: " << audio_codec << '\n';
    } else {
        details << "Audio codec: (none)\n";
    }

    double channels = 0.0;
    if (get_mpv_double_property(state, "audio-params/channel-count", &channels) && channels > 0.0) {
        details << "Audio channels: " << static_cast<int>(channels) << '\n';
    }

    double sample_rate = 0.0;
    if (get_mpv_double_property(state, "audio-params/samplerate", &sample_rate) && sample_rate > 0.0) {
        details << "Sample rate: " << static_cast<int>(sample_rate) << " Hz\n";
    }

    gtk_label_set_text(GTK_LABEL(state->media_details_label), details.str().c_str());
}

static void on_media_details_activate(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);

    if (!state->media_details_window) {
        state->media_details_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(state->media_details_window), "Media Details");
        gtk_window_set_default_size(GTK_WINDOW(state->media_details_window), 480, 320);
        gtk_window_set_transient_for(GTK_WINDOW(state->media_details_window), GTK_WINDOW(state->window));
        gtk_window_set_destroy_with_parent(GTK_WINDOW(state->media_details_window), TRUE);

        GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_container_set_border_width(GTK_CONTAINER(scroller), 12);
        gtk_container_add(GTK_CONTAINER(state->media_details_window), scroller);

        state->media_details_label = gtk_label_new("");
        gtk_label_set_selectable(GTK_LABEL(state->media_details_label), TRUE);
        gtk_label_set_xalign(GTK_LABEL(state->media_details_label), 0.0f);
        gtk_label_set_yalign(GTK_LABEL(state->media_details_label), 0.0f);
        gtk_label_set_line_wrap(GTK_LABEL(state->media_details_label), TRUE);
        gtk_container_add(GTK_CONTAINER(scroller), state->media_details_label);
    }

    refresh_media_details(state);
    gtk_widget_show_all(state->media_details_window);
    gtk_window_present(GTK_WINDOW(state->media_details_window));
}

static void on_video_scale_changed(GtkRange* range, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    const char* property = static_cast<const char*>(g_object_get_data(G_OBJECT(range), "mpv-property"));
    if (!property) {
        return;
    }
    const double value = gtk_range_get_value(range);
    set_mpv_double_property(state, property, value);
}

static void create_video_adjustment_row(AppState* state,
                                        GtkWidget* grid,
                                        int row,
                                        const char* label_text,
                                        const char* property) {
    GtkWidget* label = gtk_label_new(label_text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    GtkWidget* scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -100.0, 100.0, 1.0);
    gtk_widget_set_hexpand(scale, TRUE);

    double initial = 0.0;
    if (get_mpv_double_property(state, property, &initial)) {
        gtk_range_set_value(GTK_RANGE(scale), initial);
    }

    g_object_set_data_full(G_OBJECT(scale), "mpv-property", g_strdup(property), g_free);
    g_signal_connect(scale, "value-changed", G_CALLBACK(on_video_scale_changed), state);

    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), scale, 1, row, 1, 1);
}

static void on_video_picture_adjustments_activate(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);

    if (!state->picture_adjustments_window) {
        state->picture_adjustments_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(state->picture_adjustments_window), "Video Picture Adjustments");
        gtk_window_set_default_size(GTK_WINDOW(state->picture_adjustments_window), 420, 260);
        gtk_window_set_transient_for(GTK_WINDOW(state->picture_adjustments_window), GTK_WINDOW(state->window));
        gtk_window_set_destroy_with_parent(GTK_WINDOW(state->picture_adjustments_window), TRUE);

        GtkWidget* grid = gtk_grid_new();
        gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
        gtk_container_add(GTK_CONTAINER(state->picture_adjustments_window), grid);

        create_video_adjustment_row(state, grid, 0, "Brightness", "brightness");
        create_video_adjustment_row(state, grid, 1, "Contrast", "contrast");
        create_video_adjustment_row(state, grid, 2, "Saturation", "saturation");
        create_video_adjustment_row(state, grid, 3, "Gamma", "gamma");
        create_video_adjustment_row(state, grid, 4, "Hue", "hue");
    }

    gtk_widget_show_all(state->picture_adjustments_window);
    gtk_window_present(GTK_WINDOW(state->picture_adjustments_window));
}

static void on_window_scale_activate(GtkWidget* item, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    const char* zoom = static_cast<const char*>(g_object_get_data(G_OBJECT(item), "video-zoom"));
    if (!zoom) {
        return;
    }
    run_mpv_command(state, {"set", "video-zoom", zoom});
}

static void on_aspect_activate(GtkWidget* item, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    const char* aspect = static_cast<const char*>(g_object_get_data(G_OBJECT(item), "aspect-ratio"));
    if (!aspect) {
        return;
    }
    run_mpv_command(state, {"set", "video-aspect-override", aspect});
}

static void on_toggle_subtitles(GtkCheckMenuItem* item, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    set_mpv_flag_property(state, "sub-visibility", gtk_check_menu_item_get_active(item));
}

static void on_subtitle_scale_adjust(GtkWidget* item, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    const char* delta = static_cast<const char*>(g_object_get_data(G_OBJECT(item), "subtitle-scale-delta"));
    if (!delta) {
        return;
    }
    run_mpv_command(state, {"add", "sub-scale", delta});
}

static void on_subtitle_delay_adjust(GtkWidget* item, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    const char* delta = static_cast<const char*>(g_object_get_data(G_OBJECT(item), "subtitle-delay-delta"));
    if (!delta) {
        return;
    }
    run_mpv_command(state, {"add", "sub-delay", delta});
}

static void on_switch_angle_activate(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    run_mpv_command(state, {"cycle", "angle"});
}

static void on_audio_meter_activate(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    run_mpv_command(state, {"script-binding", "stats/display-stats-toggle"});
}

static GtkWidget* create_playback_controls(AppState* state) {
    GtkWidget* controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(controls), 8);

    GtkWidget* center_column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(center_column, TRUE);

    GtkWidget* button_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* skip_backward_button = gtk_button_new();
    GtkWidget* start_button = gtk_button_new();
    GtkWidget* rewind_button = gtk_button_new();
    GtkWidget* play_pause_button = gtk_button_new();
    GtkWidget* stop_button = gtk_button_new();
    GtkWidget* fast_forward_button = gtk_button_new();
    GtkWidget* skip_forward_button = gtk_button_new();

    gtk_button_set_image(GTK_BUTTON(skip_backward_button), gtk_image_new_from_icon_name("media-skip-backward", GTK_ICON_SIZE_BUTTON));
    gtk_button_set_image(GTK_BUTTON(start_button), gtk_image_new_from_icon_name("media-skip-backward", GTK_ICON_SIZE_BUTTON));
    gtk_button_set_image(GTK_BUTTON(rewind_button), gtk_image_new_from_icon_name("media-seek-backward", GTK_ICON_SIZE_BUTTON));
    state->play_pause_image = gtk_image_new_from_icon_name("media-playback-start", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(play_pause_button), state->play_pause_image);
    gtk_button_set_image(GTK_BUTTON(stop_button), gtk_image_new_from_icon_name("media-playback-stop", GTK_ICON_SIZE_BUTTON));
    gtk_button_set_image(GTK_BUTTON(fast_forward_button), gtk_image_new_from_icon_name("media-seek-forward", GTK_ICON_SIZE_BUTTON));
    gtk_button_set_image(GTK_BUTTON(skip_forward_button), gtk_image_new_from_icon_name("media-skip-forward", GTK_ICON_SIZE_BUTTON));

    gtk_widget_set_tooltip_text(skip_backward_button, "Previous in Playlist");
    gtk_widget_set_tooltip_text(start_button, "Return to Start");
    gtk_widget_set_tooltip_text(rewind_button, "Rewind");
    gtk_widget_set_tooltip_text(play_pause_button, "Play/Pause");
    gtk_widget_set_tooltip_text(stop_button, "Stop");
    gtk_widget_set_tooltip_text(fast_forward_button, "Fast Forward");
    gtk_widget_set_tooltip_text(skip_forward_button, "Next in Playlist");

    state->position_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 1.0);
    gtk_scale_set_draw_value(GTK_SCALE(state->position_scale), FALSE);
    gtk_widget_set_hexpand(state->position_scale, TRUE);
    state->playback_state_label = gtk_label_new("Playback state: Stopped");
    gtk_label_set_xalign(GTK_LABEL(state->playback_state_label), 0.0f);

    gtk_box_pack_start(GTK_BOX(button_row), skip_backward_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_row), start_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_row), rewind_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_row), play_pause_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_row), stop_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_row), fast_forward_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_row), skip_forward_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(button_row), state->position_scale, FALSE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(center_column), button_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(center_column), state->playback_state_label, FALSE, FALSE, 0);

    GtkWidget* right_column = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* volume_label = gtk_label_new("Volume");
    state->volume_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 1.0);
    gtk_range_set_value(GTK_RANGE(state->volume_scale), 100.0);
    gtk_widget_set_size_request(state->volume_scale, 140, -1);
    GtkWidget* fullscreen_button = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(fullscreen_button), gtk_image_new_from_icon_name("view-fullscreen", GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_tooltip_text(fullscreen_button, "Fullscreen");

    gtk_box_pack_start(GTK_BOX(right_column), volume_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_column), state->volume_scale, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_column), fullscreen_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(controls), center_column, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(controls), right_column, FALSE, FALSE, 0);

    g_signal_connect(skip_backward_button, "clicked", G_CALLBACK(on_skip_backward_clicked), state);
    g_signal_connect(start_button, "clicked", G_CALLBACK(on_return_to_start_clicked), state);
    g_signal_connect(rewind_button, "clicked", G_CALLBACK(on_rewind_clicked), state);
    g_signal_connect(play_pause_button, "clicked", G_CALLBACK(on_play_pause_clicked), state);
    g_signal_connect(stop_button, "clicked", G_CALLBACK(on_stop_clicked), state);
    g_signal_connect(fast_forward_button, "clicked", G_CALLBACK(on_fast_forward_clicked), state);
    g_signal_connect(skip_forward_button, "clicked", G_CALLBACK(on_skip_forward_clicked), state);
    g_signal_connect(state->position_scale, "value-changed", G_CALLBACK(on_position_value_changed), state);
    g_signal_connect(state->volume_scale, "value-changed", G_CALLBACK(on_volume_value_changed), state);
    g_signal_connect(fullscreen_button, "clicked", G_CALLBACK(on_fullscreen_button_clicked), state);

    return controls;
}

static void populate_language_dropdown(GtkComboBoxText* combo) {
    gtk_combo_box_text_append_text(combo, "System Default");
    for (int index = 0; langlist[index] != nullptr; ++index) {
        const std::string entry = langlist[index];
        const auto separator = entry.find(',');
        if (separator == std::string::npos) {
            gtk_combo_box_text_append_text(combo, entry.c_str());
            continue;
        }

        const std::string language_name = trim_copy(entry.substr(0, separator));
        const std::string codes = trim_copy(entry.substr(separator + 1));
        if (language_name.empty()) {
            continue;
        }

        const std::string label = codes.empty() ? language_name : language_name + " (" + codes + ")";
        gtk_combo_box_text_append_text(combo, label.c_str());
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
}

static GtkWidget* create_language_selection_tab(const char* primary_label_text,
                                                const char* fallback_label_text) {
    GtkWidget* grid = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);

    GtkWidget* primary_label = gtk_label_new(primary_label_text);
    GtkWidget* fallback_label = gtk_label_new(fallback_label_text);
    gtk_widget_set_halign(primary_label, GTK_ALIGN_START);
    gtk_widget_set_halign(fallback_label, GTK_ALIGN_START);

    GtkWidget* primary_combo = gtk_combo_box_text_new();
    GtkWidget* fallback_combo = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(primary_combo, TRUE);
    gtk_widget_set_hexpand(fallback_combo, TRUE);

    populate_language_dropdown(GTK_COMBO_BOX_TEXT(primary_combo));
    populate_language_dropdown(GTK_COMBO_BOX_TEXT(fallback_combo));

    gtk_grid_attach(GTK_GRID(grid), primary_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), primary_combo, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), fallback_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), fallback_combo, 1, 1, 1, 1);

    return grid;
}

static GtkWidget* create_placeholder_tab(const char* text) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget* label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    return box;
}

static GtkWidget* create_preferences_notebook() {
    GtkWidget* notebook = gtk_notebook_new();

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                             create_placeholder_tab("Player settings are not configured yet."),
                             gtk_label_new("Player"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                             create_language_selection_tab("Primary language:", "Fallback language:"),
                             gtk_label_new("Language Settings"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                             create_language_selection_tab("Primary subtitle language:",
                                                           "Fallback subtitle language:"),
                             gtk_label_new("Subtitles"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                             create_placeholder_tab("Interface settings are not configured yet."),
                             gtk_label_new("Interface"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                             create_placeholder_tab("Keyboard shortcut settings are not configured yet."),
                             gtk_label_new("Keyboard Shortcuts"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                             create_placeholder_tab("MPV settings are not configured yet."),
                             gtk_label_new("MPV"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook),
                             create_placeholder_tab("Plugin settings are not configured yet."),
                             gtk_label_new("Plugin"));

    return notebook;
}

static void on_preferences_activate(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);
    GtkWidget* dialog = gtk_dialog_new_with_buttons("Preferences", GTK_WINDOW(state->window),
                                                     static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                                                                 GTK_DIALOG_DESTROY_WITH_PARENT),
                                                     "_Close", GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 760, 520);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget* notebook = create_preferences_notebook();
    gtk_box_pack_start(GTK_BOX(content), notebook, TRUE, TRUE, 0);
    gtk_widget_show_all(dialog);

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
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
    GtkWidget* open_url = gtk_menu_item_new_with_label("Open URL/Stream...");
    GtkWidget* open_tv = gtk_menu_item_new_with_label("Open TV://");
    GtkWidget* quit_item = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(open_files, "activate", G_CALLBACK(on_open_files_activate), state);
    g_signal_connect(open_url, "activate", G_CALLBACK(on_open_url_activate), state);
    g_signal_connect(open_tv, "activate", G_CALLBACK(on_open_tv_activate), state);
    g_signal_connect_swapped(quit_item, "activate", G_CALLBACK(gtk_window_close), state->window);

    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), open_files);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), open_url);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), open_tv);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), quit_item);

    GtkWidget* prefs_item = gtk_menu_item_new_with_label("Preferences");
    GtkWidget* dump_audio_item = gtk_menu_item_new_with_label("Dump Audio");
    GtkWidget* about_item = gtk_menu_item_new_with_label("About");
    g_signal_connect(prefs_item, "activate", G_CALLBACK(on_preferences_activate), state);
    g_signal_connect(dump_audio_item, "activate", G_CALLBACK(on_dump_audio_activate), state);
    g_signal_connect(about_item, "activate", G_CALLBACK(on_about_activate), state);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), dump_audio_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), prefs_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(help_menu), about_item);

    GtkWidget* playlist_item = gtk_check_menu_item_new_with_label("Playlist");
    GtkWidget* media_info_item = gtk_menu_item_new_with_label("Media Info");
    GtkWidget* details_item = gtk_menu_item_new_with_label("Details");
    GtkWidget* audio_meter_item = gtk_menu_item_new_with_label("Audio Meter");
    GtkWidget* fullscreen_item = gtk_check_menu_item_new_with_label("Full Screen");
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
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(controls_item), TRUE);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(subtitles_item), get_mpv_flag_property(state, "sub-visibility", true));

    g_signal_connect(playlist_item, "toggled", G_CALLBACK(on_toggle_playlist), state);
    g_signal_connect(fullscreen_item, "toggled", G_CALLBACK(on_toggle_fullscreen), state);
    g_signal_connect(media_info_item, "activate", G_CALLBACK(on_media_details_activate), state);
    g_signal_connect(details_item, "activate", G_CALLBACK(on_media_details_activate), state);
    g_signal_connect(audio_meter_item, "activate", G_CALLBACK(on_audio_meter_activate), state);

    g_object_set_data(G_OBJECT(normal_size_item), "video-zoom", const_cast<char*>("0"));
    g_object_set_data(G_OBJECT(double_size_item), "video-zoom", const_cast<char*>("1"));
    g_object_set_data(G_OBJECT(half_size_item), "video-zoom", const_cast<char*>("-1"));
    g_object_set_data(G_OBJECT(half_larger_item), "video-zoom", const_cast<char*>("0.5849625007"));
    g_signal_connect(normal_size_item, "activate", G_CALLBACK(on_window_scale_activate), state);
    g_signal_connect(double_size_item, "activate", G_CALLBACK(on_window_scale_activate), state);
    g_signal_connect(half_size_item, "activate", G_CALLBACK(on_window_scale_activate), state);
    g_signal_connect(half_larger_item, "activate", G_CALLBACK(on_window_scale_activate), state);

    g_object_set_data(G_OBJECT(aspect_3_2_item), "aspect-ratio", const_cast<char*>("3:2"));
    g_object_set_data(G_OBJECT(aspect_4_3_item), "aspect-ratio", const_cast<char*>("4:3"));
    g_object_set_data(G_OBJECT(aspect_5_4_item), "aspect-ratio", const_cast<char*>("5:4"));
    g_object_set_data(G_OBJECT(aspect_9_16_item), "aspect-ratio", const_cast<char*>("9:16"));
    g_object_set_data(G_OBJECT(aspect_16_9_item), "aspect-ratio", const_cast<char*>("16:9"));
    g_object_set_data(G_OBJECT(aspect_16_10_item), "aspect-ratio", const_cast<char*>("16:10"));
    g_object_set_data(G_OBJECT(aspect_21_9_item), "aspect-ratio", const_cast<char*>("21:9"));
    g_object_set_data(G_OBJECT(aspect_185_1_item), "aspect-ratio", const_cast<char*>("1.85"));
    g_object_set_data(G_OBJECT(aspect_235_1_item), "aspect-ratio", const_cast<char*>("2.35"));
    g_signal_connect(aspect_3_2_item, "activate", G_CALLBACK(on_aspect_activate), state);
    g_signal_connect(aspect_4_3_item, "activate", G_CALLBACK(on_aspect_activate), state);
    g_signal_connect(aspect_5_4_item, "activate", G_CALLBACK(on_aspect_activate), state);
    g_signal_connect(aspect_9_16_item, "activate", G_CALLBACK(on_aspect_activate), state);
    g_signal_connect(aspect_16_9_item, "activate", G_CALLBACK(on_aspect_activate), state);
    g_signal_connect(aspect_16_10_item, "activate", G_CALLBACK(on_aspect_activate), state);
    g_signal_connect(aspect_21_9_item, "activate", G_CALLBACK(on_aspect_activate), state);
    g_signal_connect(aspect_185_1_item, "activate", G_CALLBACK(on_aspect_activate), state);
    g_signal_connect(aspect_235_1_item, "activate", G_CALLBACK(on_aspect_activate), state);

    g_signal_connect(subtitles_item, "toggled", G_CALLBACK(on_toggle_subtitles), state);
    g_object_set_data(G_OBJECT(subtitle_smaller_item), "subtitle-scale-delta", const_cast<char*>("-0.1"));
    g_object_set_data(G_OBJECT(subtitle_larger_item), "subtitle-scale-delta", const_cast<char*>("0.1"));
    g_signal_connect(subtitle_smaller_item, "activate", G_CALLBACK(on_subtitle_scale_adjust), state);
    g_signal_connect(subtitle_larger_item, "activate", G_CALLBACK(on_subtitle_scale_adjust), state);
    g_object_set_data(G_OBJECT(subtitle_delay_down_item), "subtitle-delay-delta", const_cast<char*>("-0.1"));
    g_object_set_data(G_OBJECT(subtitle_delay_up_item), "subtitle-delay-delta", const_cast<char*>("0.1"));
    g_signal_connect(subtitle_delay_down_item, "activate", G_CALLBACK(on_subtitle_delay_adjust), state);
    g_signal_connect(subtitle_delay_up_item, "activate", G_CALLBACK(on_subtitle_delay_adjust), state);
    g_signal_connect(switch_angle_item, "activate", G_CALLBACK(on_switch_angle_activate), state);
    g_signal_connect(controls_item, "toggled", G_CALLBACK(on_toggle_controls), state);
    g_signal_connect(video_picture_adjustments_item, "activate", G_CALLBACK(on_video_picture_adjustments_activate), state);

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
    const AppConfig config = load_app_config();

    state->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(state->window), "mate-mpv");
    gtk_window_set_default_size(GTK_WINDOW(state->window), config.window_width, config.window_height);

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
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(state->playlist_view)), GTK_SELECTION_MULTIPLE);
    GtkCellRenderer* renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn* column = gtk_tree_view_column_new_with_attributes("Playlist", renderer, "text", COL_TITLE, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(state->playlist_view), column);
    g_signal_connect(state->playlist_view, "row-activated", G_CALLBACK(on_playlist_row_activated), state);

    state->playlist_sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(state->playlist_sidebar, 260, -1);

    state->playlist_scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_vexpand(state->playlist_scroller, TRUE);
    gtk_container_add(GTK_CONTAINER(state->playlist_scroller), state->playlist_view);
    gtk_box_pack_start(GTK_BOX(state->playlist_sidebar), state->playlist_scroller, TRUE, TRUE, 0);

    GtkWidget* playlist_button_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_halign(playlist_button_bar, GTK_ALIGN_CENTER);

    GtkWidget* load_playlist_button = gtk_button_new_from_icon_name("document-open", GTK_ICON_SIZE_BUTTON);
    GtkWidget* save_playlist_button = gtk_button_new_from_icon_name("document-save", GTK_ICON_SIZE_BUTTON);
    GtkWidget* add_files_button = gtk_button_new_from_icon_name("folder-open", GTK_ICON_SIZE_BUTTON);
    GtkWidget* remove_selected_button = gtk_button_new_from_icon_name("window-close", GTK_ICON_SIZE_BUTTON);
    GtkWidget* move_up_button = gtk_button_new_from_icon_name("go-up", GTK_ICON_SIZE_BUTTON);
    GtkWidget* move_down_button = gtk_button_new_from_icon_name("go-down", GTK_ICON_SIZE_BUTTON);

    gtk_widget_set_tooltip_text(load_playlist_button, "Load playlist");
    gtk_widget_set_tooltip_text(save_playlist_button, "Save playlist");
    gtk_widget_set_tooltip_text(add_files_button, "Add files to playlist");
    gtk_widget_set_tooltip_text(remove_selected_button, "Remove selected playlist items");
    gtk_widget_set_tooltip_text(move_up_button, "Move selected item up");
    gtk_widget_set_tooltip_text(move_down_button, "Move selected item down");

    g_signal_connect(load_playlist_button, "clicked", G_CALLBACK(on_load_playlist_clicked), state);
    g_signal_connect(save_playlist_button, "clicked", G_CALLBACK(on_save_playlist_clicked), state);
    g_signal_connect(add_files_button, "clicked", G_CALLBACK(on_add_files_to_playlist_clicked), state);
    g_signal_connect(remove_selected_button, "clicked", G_CALLBACK(on_remove_selected_playlist_clicked), state);
    g_signal_connect(move_up_button, "clicked", G_CALLBACK(on_move_playlist_up_clicked), state);
    g_signal_connect(move_down_button, "clicked", G_CALLBACK(on_move_playlist_down_clicked), state);

    gtk_box_pack_start(GTK_BOX(playlist_button_bar), load_playlist_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(playlist_button_bar), save_playlist_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(playlist_button_bar), add_files_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(playlist_button_bar), remove_selected_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(playlist_button_bar), move_up_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(playlist_button_bar), move_down_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(state->playlist_sidebar), playlist_button_bar, FALSE, FALSE, 0);

    gtk_paned_pack1(GTK_PANED(paned), state->video_area, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), state->playlist_sidebar, FALSE, FALSE);

    state->playback_controls = create_playback_controls(state);
    gtk_box_pack_end(GTK_BOX(root), state->playback_controls, FALSE, FALSE, 0);

    state->player_context_menu = create_player_context_menu(state);
    gtk_widget_set_events(state->video_area, gtk_widget_get_events(state->video_area) | GDK_BUTTON_PRESS_MASK);
    g_signal_connect(state->video_area, "button-press-event", G_CALLBACK(on_video_area_button_press), state);

    gtk_widget_add_events(state->window, GDK_POINTER_MOTION_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(state->window, "motion-notify-event", G_CALLBACK(on_window_motion_notify), state);
    g_signal_connect(state->window, "key-press-event", G_CALLBACK(on_window_key_press), state);

    gtk_widget_show_all(state->window);

    if (state->volume_scale) {
        gtk_range_set_value(GTK_RANGE(state->volume_scale), config.volume);
    }
    if (ensure_mpv_running(state)) {
        set_mpv_double_property(state, "volume", config.volume);
        if (!config.audio_language.empty()) {
            set_mpv_string_property(state, "alang", config.audio_language);
        }
        if (!config.subtitle_language.empty()) {
            set_mpv_string_property(state, "slang", config.subtitle_language);
        }
    }

    if (state->playlist_sidebar) {
        gtk_widget_set_visible(state->playlist_sidebar, config.playlist_visible);
    }
    if (state->playlist_toggle_item) {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(state->playlist_toggle_item), config.playlist_visible);
    }
    if (state->context_playlist_toggle_item) {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(state->context_playlist_toggle_item), config.playlist_visible);
    }
    if (state->playback_controls) {
        gtk_widget_set_visible(state->playback_controls, config.controls_visible);
    }
    if (state->show_controls_item) {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(state->show_controls_item), config.controls_visible);
    }

    if (config.fullscreen) {
        set_fullscreen_state(state, true);
    }

    if (state->position_update_source == 0) {
        state->position_update_source = g_timeout_add(250, update_position_scale, state);
    }
}

static void shutdown_app(GApplication*, gpointer user_data) {
    auto* state = static_cast<AppState*>(user_data);

    save_app_config(state);

    if (state->position_update_source != 0) {
        g_source_remove(state->position_update_source);
        state->position_update_source = 0;
    }

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
