#include "spice/SpiceDisplaySession.h"

#include <memory>

#include <spice-client.h>
#include <spice/vd_agent.h>

#include <algorithm>
#include <cstring>

namespace rdc {

namespace {

bool isLoopback(const std::string& host)
{
    return host == "localhost" || host == "::1" || host.rfind("127.", 0) == 0;
}

} // namespace

struct SpiceDisplaySession::Callbacks {
    static SpiceDisplaySession* self(gpointer data) { return static_cast<SpiceDisplaySession*>(data); }

    template <class Fn>
    static void on(SpiceDisplaySession* s, gpointer object, const char* signal, Fn handler)
    {
        g_signal_connect(object, signal, G_CALLBACK(handler), s);
        if (std::find(s->connected_.begin(), s->connected_.end(), object) == s->connected_.end()) {
            s->connected_.push_back(G_OBJECT(object));
            g_object_ref(object); // alive until handlers are disconnected
        }
    }

    static gboolean reconnect(gpointer data)
    {
        SpiceDisplaySession* s = self(data);
        s->reconnectSource_ = 0;
        spice_session_disconnect(s->session_);
        if (!spice_session_connect(s->session_)) s->listener_.onStateChanged(SessionState::Error);
        return G_SOURCE_REMOVE;
    }

    static void channelNew(SpiceSession*, SpiceChannel* channel, gpointer data)
    {
        SpiceDisplaySession* s = self(data);
        int id = 0;
        g_object_get(channel, "channel-id", &id, nullptr);
        if (SPICE_IS_MAIN_CHANNEL(channel)) {
            s->main_ = SPICE_MAIN_CHANNEL(channel);
            on(s, channel, "channel-event", mainEvent);
            on(s, channel, "main-agent-update", agentUpdate);
            on(s, channel, "main-clipboard-selection-grab", clipboardGrab);
            on(s, channel, "main-clipboard-selection", clipboardData);
            on(s, channel, "main-clipboard-selection-request", clipboardRequest);
        } else if (SPICE_IS_DISPLAY_CHANNEL(channel) && id == 0) { // shortcut: first monitor only, add multi-monitor when needed
            on(s, channel, "channel-event", displayEvent);
            on(s, channel, "display-primary-create", primaryCreate);
            on(s, channel, "display-primary-destroy", primaryDestroy);
            on(s, channel, "display-invalidate", invalidate);
            spice_channel_connect(channel);
        } else if (SPICE_IS_INPUTS_CHANNEL(channel)) {
            s->inputs_ = SPICE_INPUTS_CHANNEL(channel);
            spice_channel_connect(channel);
        } else if (SPICE_IS_CURSOR_CHANNEL(channel)) {
            on(s, channel, "cursor-set", cursorSet);
            on(s, channel, "cursor-hide", cursorHide);
            on(s, channel, "cursor-reset", cursorReset);
            spice_channel_connect(channel);
        } else if (SPICE_IS_USBREDIR_CHANNEL(channel)) {
            ++s->usbChannels_;
            spice_channel_connect(channel);
        } else if (SPICE_IS_WEBDAV_CHANNEL(channel) && !s->connection_.option("sharedFolder").empty()) {
            spice_channel_connect(channel); // spice-glib shares ~/Public by default
        }
    }

    static void channelDestroy(SpiceSession*, SpiceChannel* channel, gpointer data)
    {
        SpiceDisplaySession* s = self(data);
        if (SPICE_IS_MAIN_CHANNEL(channel)) s->main_ = nullptr;
        if (SPICE_IS_INPUTS_CHANNEL(channel)) s->inputs_ = nullptr;
        if (SPICE_IS_USBREDIR_CHANNEL(channel)) --s->usbChannels_;
    }

    static void displayEvent(SpiceChannel* channel, SpiceChannelEvent event, gpointer data)
    {
        static constexpr std::pair<const char*, gint> codecs[] = {{"mjpeg", SPICE_VIDEO_CODEC_TYPE_MJPEG}, {"vp8", SPICE_VIDEO_CODEC_TYPE_VP8},
                                                                  {"vp9", SPICE_VIDEO_CODEC_TYPE_VP9}, {"h264", SPICE_VIDEO_CODEC_TYPE_H264},
                                                                  {"h265", SPICE_VIDEO_CODEC_TYPE_H265}};
        if (event != SPICE_CHANNEL_OPENED) return; // needs the server's capabilities
        const std::string wanted = self(data)->connection_.option("videoCodec");
        for (auto [name, type] : codecs)
            if (wanted == name) spice_display_channel_change_preferred_video_codec_type(channel, type);
    }

    static void mainEvent(SpiceChannel* channel, SpiceChannelEvent event, gpointer data)
    {
        SpiceDisplaySession* s = self(data);
        switch (event) {
        case SPICE_CHANNEL_OPENED:
            s->listener_.onStateChanged(SessionState::Connected);
            break;
        case SPICE_CHANNEL_CLOSED:
            s->listener_.onStateChanged(s->failed_ ? SessionState::Error : SessionState::Disconnected);
            break;
        case SPICE_CHANNEL_ERROR_AUTH: {
            Credentials creds;
            creds.needsUser = false;
            if (++s->authTries_ <= 3 && s->listener_.askCredentials("SPICE password", creds)) {
                g_object_set(s->session_, "password", creds.password.c_str(), nullptr);
                if (!s->reconnectSource_) s->reconnectSource_ = g_idle_add(reconnect, s); // not inside the channel's callback
            } else {
                s->listener_.onStateChanged(SessionState::Disconnected);
            }
            break;
        }
        default: {
            const GError* error = spice_channel_get_error(channel);
            s->failed_ = true;
            s->listener_.onError(std::string("SPICE connection failed") + (error ? ": " + std::string(error->message) : "."));
            s->listener_.onStateChanged(SessionState::Error);
            break;
        }
        }
    }

    static void agentUpdate(SpiceChannel*, gpointer data)
    {
        SpiceDisplaySession* s = self(data);
        if (s->wantWidth_ > 0) s->resizeDesktop(s->wantWidth_, s->wantHeight_);
    }

    static void primaryCreate(SpiceChannel*, gint format, gint width, gint height, gint stride, gint, gpointer imgdata,
                              gpointer data)
    {
        SpiceDisplaySession* s = self(data);
        if (format != SPICE_SURFACE_FMT_32_xRGB && format != SPICE_SURFACE_FMT_32_ARGB) {
            s->listener_.onError("Unsupported SPICE surface format " + std::to_string(format) + ".");
            return;
        }
        s->primary_ = static_cast<const uint8_t*>(imgdata);
        s->stride_ = stride;
        const Rect all{0, 0, width, height};
        s->framebuffer_.resize(width, height);
        s->framebuffer_.update(all, s->primary_, stride);
        s->listener_.onResize(width, height);
        s->listener_.onFrameUpdated(all);
    }

    static void primaryDestroy(SpiceChannel*, gpointer data) { self(data)->primary_ = nullptr; }

    static void invalidate(SpiceChannel*, gint x, gint y, gint w, gint h, gpointer data)
    {
        SpiceDisplaySession* s = self(data);
        if (!s->primary_) return;
        const Rect r{x, y, w, h};
        s->framebuffer_.update(r, s->primary_, s->stride_);
        s->listener_.onFrameUpdated(r);
    }

    static void cursorSet(SpiceCursorChannel*, gint width, gint height, gint hotX, gint hotY, gpointer rgba, gpointer data)
    {
        if (!rgba || width <= 0 || height <= 0) return;
        std::vector<uint8_t> bgra(static_cast<const uint8_t*>(rgba), static_cast<const uint8_t*>(rgba) + size_t(width) * height * 4);
        for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
        self(data)->listener_.onCursor(std::move(bgra), width, height, hotX, hotY);
    }

    static void cursorHide(SpiceCursorChannel*, gpointer data) { self(data)->listener_.onCursor(std::vector<uint8_t>(4, 0), 1, 1, 0, 0); }
    static void cursorReset(SpiceCursorChannel*, gpointer data) { self(data)->listener_.onCursor({}, 0, 0, 0, 0); }

    static gboolean clipboardGrab(SpiceMainChannel* main, guint selection, gpointer types, guint ntypes, gpointer)
    {
        if (selection != VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD) return TRUE;
        const auto* t = static_cast<const guint32*>(types);
        for (guint i = 0; i < ntypes; ++i)
            if (t[i] == VD_AGENT_CLIPBOARD_UTF8_TEXT) {
                spice_main_channel_clipboard_selection_request(main, selection, VD_AGENT_CLIPBOARD_UTF8_TEXT);
                break;
            }
        return TRUE;
    }

    static void clipboardData(SpiceMainChannel*, guint selection, guint type, gpointer bytes, guint size, gpointer data)
    {
        if (selection != VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD || type != VD_AGENT_CLIPBOARD_UTF8_TEXT) return;
        SpiceDisplaySession* s = self(data);
        std::string text(static_cast<const char*>(bytes), strnlen(static_cast<const char*>(bytes), size));
        s->localClipboard_ = text; // the UI echoes it back; sendClipboardText then ignores it
        s->listener_.onClipboardText(std::move(text));
    }

    static gboolean clipboardRequest(SpiceMainChannel* main, guint selection, guint type, gpointer data)
    {
        SpiceDisplaySession* s = self(data);
        if (type != VD_AGENT_CLIPBOARD_UTF8_TEXT) return FALSE;
        spice_main_channel_clipboard_selection_notify(main, selection, type,
                                                      reinterpret_cast<const guchar*>(s->localClipboard_.data()),
                                                      s->localClipboard_.size());
        return TRUE;
    }
};

SpiceDisplaySession::SpiceDisplaySession(DisplayListener& listener, Connection connection)
    : listener_(listener), connection_(std::move(connection))
{
}

void SpiceDisplaySession::connect()
{
    if (session_) return;
    listener_.onStateChanged(SessionState::Connecting);
    const Connection& c = connection_;
    const int tlsPort = c.number("tlsPort");
    if (tlsPort == 0 && !isLoopback(c.host) &&
        !listener_.askTrust("This SPICE connection is not encrypted. Anyone on the network path can see the screen "
                            "and what you type. Connect anyway?",
                            "Use a TLS port or an SSH tunnel (Edit connection).")) {
        listener_.onStateChanged(SessionState::Disconnected);
        return;
    }

    session_ = spice_session_new();
    g_object_set(session_, "host", c.host.c_str(), "enable-audio", gboolean(c.flag("audio")), nullptr);
    if (tlsPort == 0 || c.port > 0) // TLS-only: never fall back to plain
        g_object_set(session_, "port", std::to_string(c.effectivePort()).c_str(), nullptr);
    if (tlsPort > 0) g_object_set(session_, "tls-port", std::to_string(tlsPort).c_str(), nullptr);
    auto setIf = [&](const char* property, const std::string& value) {
        if (!value.empty()) g_object_set(session_, property, value.c_str(), nullptr);
    };
    setIf("password", c.password);
    setIf("ca-file", c.option("caFile"));
    setIf("cert-subject", c.option("hostSubject"));
    setIf("proxy", c.option("proxy"));
    setIf("shared-dir", c.option("sharedFolder"));
    g_object_set(session_, "share-dir-ro", gboolean(c.flag("sharedFolderReadOnly")), nullptr);
    static constexpr std::pair<const char*, int> compressions[] = {
        {"auto-glz", SPICE_IMAGE_COMPRESSION_AUTO_GLZ}, {"auto-lz", SPICE_IMAGE_COMPRESSION_AUTO_LZ},
        {"quic", SPICE_IMAGE_COMPRESSION_QUIC}, {"glz", SPICE_IMAGE_COMPRESSION_GLZ}, {"lz", SPICE_IMAGE_COMPRESSION_LZ},
        {"lz4", SPICE_IMAGE_COMPRESSION_LZ4}, {"off", SPICE_IMAGE_COMPRESSION_OFF}};
    for (auto [name, value] : compressions)
        if (c.option("compression") == name) g_object_set(session_, "preferred-compression", value, nullptr);
    Callbacks::on(this, session_, "channel-new", Callbacks::channelNew);
    Callbacks::on(this, session_, "channel-destroy", Callbacks::channelDestroy);
    if (c.flag("audio")) spice_audio_get(session_, nullptr);
    usb_ = spice_usb_device_manager_get(session_, nullptr); // null: spice-glib built without usbredir
    if (usb_) g_object_set(usb_, "auto-connect", FALSE, nullptr); // a device plugged in later stays here
    if (!spice_session_connect(session_)) {
        listener_.onError("Could not start the SPICE connection.");
        listener_.onStateChanged(SessionState::Error);
    }
}

void SpiceDisplaySession::disconnect()
{
    if (!session_) return;
    if (reconnectSource_) g_source_remove(reconnectSource_);
    reconnectSource_ = 0;
    // spice-glib may still deliver queued signals after this; none must reach a destroyed session.
    for (GObject* object : connected_) {
        g_signal_handlers_disconnect_by_data(object, this);
        g_object_unref(object);
    }
    connected_.clear();
    spice_session_disconnect(session_);
    g_object_unref(session_);
    session_ = nullptr;
    main_ = nullptr;
    inputs_ = nullptr;
    usb_ = nullptr;
    usbChannels_ = 0;
    primary_ = nullptr;
    listener_.onStateChanged(SessionState::Disconnected);
}

static std::string usbName(SpiceUsbDevice* device)
{
    gchar* text = spice_usb_device_get_description(device, "%s %s %s at %d-%d");
    std::string name = text ? text : "?";
    g_free(text);
    return name;
}

std::vector<DisplaySession::UsbDevice> SpiceDisplaySession::usbDevices()
{
    std::vector<UsbDevice> list;
    if (!usb_) return list;
    GPtrArray* devices = spice_usb_device_manager_get_devices(usb_);
    for (guint i = 0; devices && i < devices->len; ++i) {
        auto* device = static_cast<SpiceUsbDevice*>(g_ptr_array_index(devices, i));
        const std::string name = usbName(device); // includes bus-address: unique
        list.push_back({name, name, bool(spice_usb_device_manager_is_device_connected(usb_, device))});
    }
    if (devices) g_ptr_array_unref(devices);
    return list;
}

void SpiceDisplaySession::redirectUsb(const std::string& id, bool on, std::function<void(std::string)> done)
{
    if (!usb_) return done("No USB redirection in this session.");
    GPtrArray* devices = spice_usb_device_manager_get_devices(usb_);
    SpiceUsbDevice* device = nullptr;
    for (guint i = 0; devices && i < devices->len && !device; ++i)
        if (usbName(static_cast<SpiceUsbDevice*>(g_ptr_array_index(devices, i))) == id)
            device = static_cast<SpiceUsbDevice*>(g_ptr_array_index(devices, i));
    GError* error = nullptr;
    if (!device) done("The device was unplugged.");
    else if (on && !spice_usb_device_manager_can_redirect_device(usb_, device, &error)) done(error ? error->message : "Cannot redirect it.");
    else {
        auto* callback = new std::function<void(std::string)>(std::move(done));
        auto finished = [](GObject* manager, GAsyncResult* result, gpointer data) {
            std::unique_ptr<std::function<void(std::string)>> done(static_cast<std::function<void(std::string)>*>(data));
            GError* failure = nullptr;
            spice_usb_device_manager_connect_device_finish(SPICE_USB_DEVICE_MANAGER(manager), result, &failure);
            (*done)(failure ? failure->message : "");
            if (failure) g_error_free(failure);
        };
        auto dropped = [](GObject* manager, GAsyncResult* result, gpointer data) {
            std::unique_ptr<std::function<void(std::string)>> done(static_cast<std::function<void(std::string)>*>(data));
            GError* failure = nullptr;
            spice_usb_device_manager_disconnect_device_finish(SPICE_USB_DEVICE_MANAGER(manager), result, &failure);
            (*done)(failure ? failure->message : "");
            if (failure) g_error_free(failure);
        };
        if (on) spice_usb_device_manager_connect_device_async(usb_, device, nullptr, finished, callback);
        else spice_usb_device_manager_disconnect_device_async(usb_, device, nullptr, dropped, callback);
    }
    if (error) g_error_free(error);
    if (devices) g_ptr_array_unref(devices);
}

void SpiceDisplaySession::sendFiles(const std::vector<std::string>& paths, std::function<void(std::string)> done)
{
    if (!agentConnected()) return done("The VM's SPICE agent (spice-vdagent) isn't running.");
    std::vector<GFile*> files;
    for (const std::string& path : paths) files.push_back(g_file_new_for_path(path.c_str()));
    files.push_back(nullptr);
    auto finished = [](GObject* main, GAsyncResult* result, gpointer data) {
        std::unique_ptr<std::function<void(std::string)>> done(static_cast<std::function<void(std::string)>*>(data));
        GError* failure = nullptr;
        spice_main_channel_file_copy_finish(SPICE_MAIN_CHANNEL(main), result, &failure);
        (*done)(failure ? failure->message : "");
        if (failure) g_error_free(failure);
    };
    spice_main_channel_file_copy_async(main_, files.data(), G_FILE_COPY_NONE, nullptr, nullptr, nullptr, finished,
                                       new std::function<void(std::string)>(std::move(done)));
    for (GFile* file : files)
        if (file) g_object_unref(file);
}

bool SpiceDisplaySession::agentConnected() const
{
    gboolean agent = FALSE;
    if (main_) g_object_get(main_, "agent-connected", &agent, nullptr);
    return agent;
}

void SpiceDisplaySession::sendKey(const KeyEvent& e)
{
    if (!inputs_ || e.scancode == 0) return; // XT scancode with 0x100 for E0 keys: what spice-glib expects
    if (e.pressed)
        spice_inputs_channel_key_press(inputs_, e.scancode);
    else
        spice_inputs_channel_key_release(inputs_, e.scancode);
}

void SpiceDisplaySession::syncLocks(uint8_t locks)
{
    if (inputs_) spice_inputs_channel_set_key_locks(inputs_, locks);
}

void SpiceDisplaySession::sendPointer(const PointerEvent& e)
{
    if (!inputs_) return;
    int mode = SPICE_MOUSE_MODE_CLIENT;
    if (main_) g_object_get(main_, "mouse-mode", &mode, nullptr);
    if (mode == SPICE_MOUSE_MODE_CLIENT)
        spice_inputs_channel_position(inputs_, e.x, e.y, 0, e.buttons);
    else // shortcut: server mouse mode drifts without a grab
        spice_inputs_channel_motion(inputs_, e.x - lastPointer_.x, e.y - lastPointer_.y, e.buttons);
    static constexpr std::pair<uint8_t, int> buttons[] = {{PointerEvent::Left, SPICE_MOUSE_BUTTON_LEFT},
                                                          {PointerEvent::Middle, SPICE_MOUSE_BUTTON_MIDDLE},
                                                          {PointerEvent::Right, SPICE_MOUSE_BUTTON_RIGHT}};
    for (auto [bit, button] : buttons)
        if ((e.buttons ^ lastPointer_.buttons) & bit) {
            if (e.buttons & bit)
                spice_inputs_channel_button_press(inputs_, button, e.buttons);
            else
                spice_inputs_channel_button_release(inputs_, button, e.buttons);
        }
    for (wheelRemainder_ += e.wheelDelta; std::abs(wheelRemainder_) >= 120;) {
        const int button = wheelRemainder_ > 0 ? SPICE_MOUSE_BUTTON_UP : SPICE_MOUSE_BUTTON_DOWN;
        spice_inputs_channel_button_press(inputs_, button, e.buttons);
        spice_inputs_channel_button_release(inputs_, button, e.buttons);
        wheelRemainder_ -= wheelRemainder_ > 0 ? 120 : -120;
    }
    lastPointer_ = e;
}

void SpiceDisplaySession::sendClipboardText(const std::string& text)
{
    if (text == localClipboard_) return;
    localClipboard_ = text;
    guint32 type = VD_AGENT_CLIPBOARD_UTF8_TEXT;
    if (agentConnected()) spice_main_channel_clipboard_selection_grab(main_, VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD, &type, 1);
}

void SpiceDisplaySession::resizeDesktop(int width, int height)
{
    wantWidth_ = width;
    wantHeight_ = height;
    if (!agentConnected()) return; // only spice-vdagent can resize the guest; agentUpdate retries
    // display 0 must be enabled or spice-glib omits it and the agent keeps its size
    spice_main_channel_update_display_enabled(main_, 0, TRUE, FALSE);
    spice_main_channel_update_display(main_, 0, 0, 0, width, height, FALSE);
    spice_main_channel_send_monitor_config(main_);
}

} // namespace rdc
