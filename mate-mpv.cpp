// main.cpp - gtkmm-3.0 + libmpv GUI with toggleable playlist pane and preferences dialog
//
// Build:
//   g++ -std=c++17 -O2 -Wall -Wextra main.cpp -o mpvgui
//     $(pkg-config --cflags --libs gtkmm-3.0 epoxy mpv)
//
// Notes:
// - Uses libmpv render API into Gtk::GLArea.
// - Playlist is a TreeView on the right, toggleable via View menu.
// - File -> Open File(s) adds items to playlist and starts playback.
// - File -> Open TV:// parses mpv channels.conf and adds entries as tv://<name>.

#include <gtkmm.h>
#include <gdk/gdkgl.h>
#include <epoxy/gl.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------
// Utility helpers
// ---------------------------
static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) b++;
    while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

static std::string home_dir() {
    const char* h = std::getenv("HOME");
    return h ? std::string(h) : std::string();
}

static std::vector<std::pair<std::string, std::string>> parse_channels_conf(const std::string& path) {
    // Returns pairs (display_name, url) where url is tv://<name>
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream f(path);
    if (!f.is_open()) return out;

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (line[0] == '#') continue;

        // channels.conf typical format: NAME:...:...
        // NAME may contain spaces; the delimiter is ':'
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;

        std::string name = trim(line.substr(0, pos));
        if (name.empty()) continue;

        // mpv supports e.g. dvb://NAME for dvb input. You asked for TV://,
        // so we create "tv://NAME". If your mpv expects "dvb://", change here.
        std::string url = "tv://" + name;
        out.emplace_back(name, url);
    }
    return out;
}

static std::optional<std::string> find_mpv_channels_conf() {
    // Common locations:
    //   ~/.config/mpv/channels.conf
    //   ~/.mpv/channels.conf (older)
    //   /etc/mpv/channels.conf (rare)
    std::vector<std::string> candidates;
    const auto home = home_dir();
    if (!home.empty()) {
        candidates.push_back(home + "/.config/mpv/channels.conf");
        candidates.push_back(home + "/.mpv/channels.conf");
    }
    candidates.push_back("/etc/mpv/channels.conf");

    for (const auto& p : candidates) {
        std::ifstream f(p);
        if (f.good()) return p;
    }
    return std::nullopt;
}

// ---------------------------
// Playlist model
// ---------------------------
class PlaylistColumns : public Gtk::TreeModel::ColumnRecord {
public:
    PlaylistColumns() {
        add(col_title);
        add(col_url);
    }
    Gtk::TreeModelColumn<Glib::ustring> col_title;
    Gtk::TreeModelColumn<Glib::ustring> col_url;
};

// ---------------------------
// Mpv GLArea widget
// ---------------------------
class MpvGLArea : public Gtk::GLArea {
public:
    MpvGLArea() {
        set_hexpand(true);
        set_vexpand(true);
        set_auto_render(true);

        // A safer default for some drivers:
        set_required_version(3, 2);
        set_has_depth_buffer(false);
        set_has_stencil_buffer(false);

        signal_realize().connect(sigc::mem_fun(*this, &MpvGLArea::on_realize), false);
        signal_unrealize().connect(sigc::mem_fun(*this, &MpvGLArea::on_unrealize), false);
        signal_render().connect(sigc::mem_fun(*this, &MpvGLArea::on_render), false);
    }

    ~MpvGLArea() override {
        shutdown_mpv();
    }

    void load_and_play(const std::string& url_or_path) {
        if (!mpv_) return;
        const char* cmd[] = {"loadfile", url_or_path.c_str(), "replace", nullptr};
        mpv_command(mpv_, cmd);
    }

    void add_to_playlist_and_play(const std::vector<std::string>& urls) {
        if (!mpv_) return;
        bool first = true;
        for (const auto& u : urls) {
            if (first) {
                const char* cmd[] = {"loadfile", u.c_str(), "replace", nullptr};
                mpv_command(mpv_, cmd);
                first = false;
            } else {
                const char* cmd[] = {"loadfile", u.c_str(), "append-play", nullptr};
                mpv_command(mpv_, cmd);
            }
        }
    }

private:
    mpv_handle* mpv_ = nullptr;
    mpv_render_context* render_ctx_ = nullptr;
    std::atomic<bool> wakeup_pending_{false};

    static void* get_proc_address(void* ctx, const char* name) {
        // ctx points to a GdkGLContext (C type), provided by us below
        GdkGLContext* gdk_ctx = static_cast<GdkGLContext*>(ctx);
        return reinterpret_cast<void*>(gdk_gl_context_get_proc_address(gdk_ctx, name));
    }

    static void wakeup_cb(void* ctx) {
        auto* self = static_cast<MpvGLArea*>(ctx);
        // Queue a redraw on GTK main thread.
        // Avoid spamming: coalesce redraw requests.
        bool expected = false;
        if (self->wakeup_pending_.compare_exchange_strong(expected, true)) {
            Glib::signal_idle().connect_once([self]() {
                self->wakeup_pending_ = false;
                self->queue_draw();
            });
        }
    }

    void init_mpv() {
        if (mpv_) return;

        mpv_ = mpv_create();
        if (!mpv_) throw std::runtime_error("mpv_create() failed");

        // Helpful defaults
        mpv_set_option_string(mpv_, "terminal", "no");
        mpv_set_option_string(mpv_, "msg-level", "all=warn");
        mpv_set_option_string(mpv_, "hwdec", "auto-safe");
        mpv_set_option_string(mpv_, "vo", "libmpv"); // ensures render API path is used

        if (mpv_initialize(mpv_) < 0) {
            throw std::runtime_error("mpv_initialize() failed");
        }

        mpv_set_wakeup_callback(mpv_, &MpvGLArea::wakeup_cb, this);
    }

    void init_render_context() {
        if (!mpv_ || render_ctx_) return;

        mpv_opengl_init_params gl_init_params;
        std::memset(&gl_init_params, 0, sizeof(gl_init_params));
	gl_init_params.get_proc_address = &MpvGLArea::get_proc_address;

	// Gtk::GLArea has a GLContext; use its underlying GdkGLContext*
	auto ctx = get_context();
	GdkGLContext* gdk_ctx = ctx ? ctx->gobj() : nullptr;
	gl_init_params.get_proc_address_ctx = gdk_ctx;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_OPENGL},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
            {MPV_RENDER_PARAM_INVALID, nullptr}
        };

        if (mpv_render_context_create(&render_ctx_, mpv_, params) < 0) {
            render_ctx_ = nullptr;
            throw std::runtime_error("mpv_render_context_create() failed");
        }

        mpv_render_context_set_update_callback(render_ctx_, &MpvGLArea::wakeup_cb, this);
    }

    void shutdown_mpv() {
        if (render_ctx_) {
            mpv_render_context_free(render_ctx_);
            render_ctx_ = nullptr;
        }
        if (mpv_) {
            mpv_terminate_destroy(mpv_);
            mpv_ = nullptr;
        }
    }

    void on_realize() {
        Gtk::GLArea::on_realize();
        make_current();

        try {
            init_mpv();
            init_render_context();
        } catch (const std::exception& e) {
            std::cerr << "mpv init error: " << e.what() << "\n";
        }
    }

    void on_unrealize() {
        // Ensure GL resources are freed while context exists.
        make_current();
        shutdown_mpv();
        Gtk::GLArea::on_unrealize();
    }

    bool on_render(const Glib::RefPtr<Gdk::GLContext>& /*context*/) {
        glViewport(0, 0, get_allocated_width(), get_allocated_height());
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (!render_ctx_) return true;

        // Render into current default framebuffer (0)
        mpv_opengl_fbo fbo;
        std::memset(&fbo, 0, sizeof(fbo));
        fbo.fbo = 0;
        fbo.w = get_allocated_width();
        fbo.h = get_allocated_height();
        fbo.internal_format = 0;

        int flip_y = 1;

        mpv_render_param render_params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
            {MPV_RENDER_PARAM_INVALID, nullptr}
        };

        mpv_render_context_render(render_ctx_, render_params);
        return true;
    }
};

// ---------------------------
// Preferences dialog
// ---------------------------
class PreferencesDialog : public Gtk::Dialog {
public:
    PreferencesDialog(Gtk::Window& parent)
        : Gtk::Dialog("Preferences", parent, true) {

        set_default_size(520, 360);
        add_button("_Close", Gtk::RESPONSE_CLOSE);

        notebook_.set_hexpand(true);
        notebook_.set_vexpand(true);

        // Audio page
        {
            auto box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 8);
            box->set_border_width(12);

            auto lbl = Gtk::make_managed<Gtk::Label>("Audio settings (placeholder)");
            lbl->set_xalign(0.f);
            box->pack_start(*lbl, Gtk::PACK_SHRINK);

            auto chk = Gtk::make_managed<Gtk::CheckButton>("Normalize audio");
            box->pack_start(*chk, Gtk::PACK_SHRINK);

            notebook_.append_page(*box, "Audio");
        }

        // Video page
        {
            auto box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 8);
            box->set_border_width(12);

            auto lbl = Gtk::make_managed<Gtk::Label>("Video settings (placeholder)");
            lbl->set_xalign(0.f);
            box->pack_start(*lbl, Gtk::PACK_SHRINK);

            auto chk = Gtk::make_managed<Gtk::CheckButton>("Enable hardware decoding");
            chk->set_active(true);
            box->pack_start(*chk, Gtk::PACK_SHRINK);

            notebook_.append_page(*box, "Video");
        }

        get_content_area()->pack_start(notebook_, Gtk::PACK_EXPAND_WIDGET);
        show_all_children();

        signal_response().connect([this](int) { hide(); });
    }

private:
    Gtk::Notebook notebook_;
};

// ---------------------------
// Main window
// ---------------------------
class PlayerWindow : public Gtk::Window {
public:
    PlayerWindow()
        : vbox_(Gtk::ORIENTATION_VERTICAL),
          main_paned_(Gtk::ORIENTATION_HORIZONTAL) {

        set_title("mpv GUI (gtkmm)");
        set_default_size(1000, 650);

        add(vbox_);

        build_menus();
        vbox_.pack_start(menubar_, Gtk::PACK_SHRINK);

        // Center area: Paned => left video, right playlist
        mpv_area_.set_hexpand(true);
        mpv_area_.set_vexpand(true);

        // Playlist TreeView
        playlist_store_ = Gtk::ListStore::create(cols_);
        playlist_view_.set_model(playlist_store_);
        playlist_view_.append_column("Title", cols_.col_title);
        playlist_view_.set_headers_visible(false);

        playlist_scroller_.add(playlist_view_);
        playlist_scroller_.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
        playlist_scroller_.set_min_content_width(260);

        // Put GLArea in a frame (gives a clean border like classic players)
        video_frame_.set_shadow_type(Gtk::SHADOW_IN);
        video_frame_.add(mpv_area_);

        main_paned_.pack1(video_frame_, /*resize*/ true, /*shrink*/ true);
        main_paned_.pack2(playlist_scroller_, /*resize*/ false, /*shrink*/ false);

        vbox_.pack_start(main_paned_, Gtk::PACK_EXPAND_WIDGET);

        // Double-click playlist item to play
        playlist_view_.signal_row_activated().connect(
            sigc::mem_fun(*this, &PlayerWindow::on_playlist_activated)
        );

        show_all_children();
        apply_playlist_visibility();
    }

private:
    // Layout
    Gtk::Box vbox_;
    Gtk::MenuBar menubar_;
    Gtk::Paned main_paned_;
    Gtk::Frame video_frame_;
    MpvGLArea mpv_area_;

    Gtk::ScrolledWindow playlist_scroller_;
    Gtk::TreeView playlist_view_;
    Glib::RefPtr<Gtk::ListStore> playlist_store_;
    PlaylistColumns cols_;

    bool playlist_visible_ = true;

    // Menus
    Gtk::MenuItem file_item_{"File"};
    Gtk::MenuItem edit_item_{"Edit"};
    Gtk::MenuItem view_item_{"View"};
    Gtk::MenuItem help_item_{"Help"};

    Gtk::Menu file_menu_;
    Gtk::Menu edit_menu_;
    Gtk::Menu view_menu_;
    Gtk::Menu help_menu_;

    Gtk::CheckMenuItem view_fullscreen_{"Fullscreen"};
    Gtk::CheckMenuItem view_playlist_{"Show Playlist"};

    void build_menus() {
        // File menu items
        auto mi_open = Gtk::make_managed<Gtk::MenuItem>("Open File(s)...");
        mi_open->signal_activate().connect(sigc::mem_fun(*this, &PlayerWindow::on_open_files));

        auto mi_open_tv = Gtk::make_managed<Gtk::MenuItem>("Open TV://");
        mi_open_tv->signal_activate().connect(sigc::mem_fun(*this, &PlayerWindow::on_open_tv));

        auto mi_quit = Gtk::make_managed<Gtk::MenuItem>("Quit");
        mi_quit->signal_activate().connect([this]() { hide(); });

        file_menu_.append(*mi_open);
        file_menu_.append(*mi_open_tv);
        file_menu_.append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
        file_menu_.append(*mi_quit);
        file_menu_.show_all();

        file_item_.set_submenu(file_menu_);
        menubar_.append(file_item_);

        // Edit menu
        auto mi_prefs = Gtk::make_managed<Gtk::MenuItem>("Preferences...");
        mi_prefs->signal_activate().connect(sigc::mem_fun(*this, &PlayerWindow::on_preferences));

        edit_menu_.append(*mi_prefs);
        edit_menu_.show_all();

        edit_item_.set_submenu(edit_menu_);
        menubar_.append(edit_item_);

        // View menu
        view_fullscreen_.signal_toggled().connect(sigc::mem_fun(*this, &PlayerWindow::on_toggle_fullscreen));
        view_playlist_.set_active(true);
        view_playlist_.signal_toggled().connect(sigc::mem_fun(*this, &PlayerWindow::on_toggle_playlist));

        view_menu_.append(view_fullscreen_);
        view_menu_.append(view_playlist_);
        view_menu_.show_all();

        view_item_.set_submenu(view_menu_);
        menubar_.append(view_item_);

        // Help menu (placeholder)
        auto mi_about = Gtk::make_managed<Gtk::MenuItem>("About");
        mi_about->signal_activate().connect([this]() {
            Gtk::MessageDialog dlg(*this, "mpv GUI (gtkmm)", false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
            dlg.set_secondary_text("A minimal gtkmm-3.0 frontend using libmpv render API.");
            dlg.run();
        });

        help_menu_.append(*mi_about);
        help_menu_.show_all();

        help_item_.set_submenu(help_menu_);
        menubar_.append(help_item_);
    }

    void apply_playlist_visibility() {
        // Gtk::Paned doesn't have a direct "hide second pane" API; simplest is show/hide the widget.
        if (playlist_visible_) {
            playlist_scroller_.show();
            // give a sensible divider position (video gets most space)
            main_paned_.set_position(std::max(1, get_allocated_width() - 320));
        } else {
            playlist_scroller_.hide();
            main_paned_.set_position(get_allocated_width()); // effectively all to video
        }
    }

    void add_playlist_item(const std::string& title, const std::string& url) {
        auto row = *(playlist_store_->append());
        row[cols_.col_title] = title;
        row[cols_.col_url] = url;
    }

    void play_row(const Gtk::TreeModel::Row& row) {
        Glib::ustring u = row[cols_.col_url];
        mpv_area_.load_and_play(u.raw());
    }

    // Callbacks
    void on_open_files() {
        Gtk::FileChooserDialog dlg(*this, "Open File(s)", Gtk::FILE_CHOOSER_ACTION_OPEN);
        dlg.set_select_multiple(true);
        dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
        dlg.add_button("_Open", Gtk::RESPONSE_OK);

        auto filter_media = Gtk::FileFilter::create();
        filter_media->set_name("Media files");
        filter_media->add_pattern("*");
        dlg.add_filter(filter_media);

        if (dlg.run() != Gtk::RESPONSE_OK) return;

        const auto files = dlg.get_filenames();
        if (files.empty()) return;

        // Populate playlist
        for (const auto& f : files) {
            // Use basename as title
            auto base = Glib::path_get_basename(f);
            add_playlist_item(base, f);
        }

        // Start playing the first selected file immediately
        mpv_area_.load_and_play(files.front());

        // Ensure playlist visible if user just loaded multiple
        if (files.size() > 1) {
            playlist_visible_ = true;
            view_playlist_.set_active(true);
            apply_playlist_visibility();
        }

    }

    void on_open_tv() {
        auto conf = find_mpv_channels_conf();
        if (!conf) {
            Gtk::MessageDialog dlg(*this, "channels.conf not found", false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK, true);
            dlg.set_secondary_text("Looked in ~/.config/mpv/channels.conf, ~/.mpv/channels.conf, and /etc/mpv/channels.conf");
            dlg.run();
            return;
        }

        auto entries = parse_channels_conf(*conf);
        if (entries.empty()) {
            Gtk::MessageDialog dlg(*this, "No channels found", false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK, true);
            dlg.set_secondary_text("channels.conf was found but no valid NAME: lines were parsed.");
            dlg.run();
            return;
        }

        // Clear and repopulate playlist
        playlist_store_->clear();
        for (const auto& [name, url] : entries) {
            add_playlist_item(name, url);
        }

        playlist_visible_ = true;
        view_playlist_.set_active(true);
        apply_playlist_visibility();

        // Autoplay first channel
        auto children = playlist_store_->children();
        if (!children.empty()) {
            play_row(*children.begin());
        }
    }

    void on_preferences() {
        PreferencesDialog dlg(*this);
        dlg.run();
    }

    void on_toggle_fullscreen() {
        if (view_fullscreen_.get_active()) {
            fullscreen();
        } else {
            unfullscreen();
        }
    }

    void on_toggle_playlist() {
        playlist_visible_ = view_playlist_.get_active();
        apply_playlist_visibility();
    }

    void on_playlist_activated(const Gtk::TreeModel::Path& path, Gtk::TreeViewColumn*) {
        auto iter = playlist_store_->get_iter(path);
        if (!iter) return;
        play_row(*iter);
    }
};

// ---------------------------
// main
// ---------------------------
int main(int argc, char** argv) {
    auto app = Gtk::Application::create(argc, argv, "com.example.mpvgui");
    PlayerWindow win;
    return app->run(win);
}
