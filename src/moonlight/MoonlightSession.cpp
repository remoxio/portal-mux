#include "moonlight/MoonlightSession.h"

#include "core/Properties.h"

#include <Limelight.h>
#include <openssl/rand.h>
#include <winpr/input.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

namespace fs = std::filesystem;

namespace rdc {

struct MoonlightSession::Config : STREAM_CONFIGURATION {};

static_assert(GamepadState::South == A_FLAG && GamepadState::North == Y_FLAG && GamepadState::Guide == SPECIAL_FLAG &&
              GamepadState::Start == PLAY_FLAG && GamepadState::Back == BACK_FLAG && GamepadState::Up == UP_FLAG &&
              GamepadState::LeftShoulder == LB_FLAG && GamepadState::RightStick == RS_CLK_FLAG &&
              GamepadState::Paddle1 == PADDLE1_FLAG && GamepadState::Paddle4 == PADDLE4_FLAG &&
              GamepadState::Touchpad == TOUCHPAD_FLAG && GamepadState::Misc == MISC_FLAG);
static_assert(GamepadState::AnalogTriggers == LI_CCAP_ANALOG_TRIGGERS && GamepadState::Rumble == LI_CCAP_RUMBLE &&
              GamepadState::TriggerRumble == LI_CCAP_TRIGGER_RUMBLE);

namespace {
std::atomic<MoonlightSession*> activeSession{nullptr}; // moonlight-common-c's callbacks carry no context
bool debug() { return std::getenv("PORTAL_MUX_DEBUG") != nullptr; }
} // namespace

struct MoonlightSession::Callbacks {
    static MoonlightSession* self() { return activeSession.load(); }

    static int videoSetup(int format, int width, int height, int, void* context, int)
    {
        auto* s = static_cast<MoonlightSession*>(context);
        const auto codec = format & VIDEO_FORMAT_MASK_AV1    ? VideoDecoder::Codec::AV1
                         : format & VIDEO_FORMAT_MASK_H265 ? VideoDecoder::Codec::H265
                                                           : VideoDecoder::Codec::H264;
        std::string error;
        if (!s->video_.open(codec, s->connection_.flag("hardwareDecoding"), &error)) {
            s->stageError_ = error;
            return -1;
        }
        s->width_ = width;
        s->height_ = height;
        if (debug()) std::fprintf(stderr, "moonlight: video %dx%d, format 0x%x\n", width, height, format);
        return 0;
    }

    static int videoSubmit(PDECODE_UNIT unit)
    {
        MoonlightSession* s = self();
        s->frame_.resize(size_t(unit->fullLength));
        size_t at = 0;
        for (PLENTRY e = unit->bufferList; e; e = e->next) {
            std::memcpy(s->frame_.data() + at, e->data, size_t(e->length));
            at += size_t(e->length);
        }
        const int oldWidth = s->video_.width(), oldHeight = s->video_.height();
        if (s->video_.decode(s->frame_.data(), at, int64_t(unit->presentationTimeUs), s->framebuffer_) > 0) {
            if (s->video_.width() != oldWidth || s->video_.height() != oldHeight)
                s->listener_.onResize(s->video_.width(), s->video_.height());
            s->listener_.onFrameUpdated({0, 0, s->video_.width(), s->video_.height()});
        } else if (unit->frameType == FRAME_TYPE_IDR) {
            return DR_NEED_IDR;
        }
        return DR_OK;
    }

    static int audioInit(int, const POPUS_MULTISTREAM_CONFIGURATION opus, void* context, int)
    {
        auto* s = static_cast<MoonlightSession*>(context);
        std::string error;
        // stereo: one coupled Opus stream, no multistream header needed
        return s->audio_.open(AudioDecoder::Codec::Opus, opus->sampleRate, opus->channelCount, &error) ? 0 : -1;
    }

    static void audioSample(char* data, int length)
    {
        MoonlightSession* s = self();
        if (!data || length <= 0 || !s->connection_.flag("audio")) return;
        s->audio_.decode(reinterpret_cast<const uint8_t*>(data), size_t(length), s->pcm_);
        static std::atomic<bool> reported{false};
        if (!s->pcm_.empty() && debug() && !reported.exchange(true)) std::fputs("moonlight: audio playing\n", stderr);
        if (!s->pcm_.empty())
            s->listener_.onAudio(s->pcm_.data(), s->pcm_.size() / size_t(s->audio_.channels()), s->audio_.sampleRate(),
                                 s->audio_.channels());
        s->pcm_.clear();
    }

    static void stageFailed(int stage, int error)
    {
        self()->stageError_ = std::string("Connecting failed at \"") + LiGetStageName(stage) + "\" (error " +
                              std::to_string(error) + "). Check that the host's firewall lets UDP 47998-48000 through.";
    }

    static void terminated(int error)
    {
        MoonlightSession* s = self();
        std::lock_guard lock(s->mutex_);
        s->terminated_ = true;
        s->terminationError_ = error;
        s->ended_.notify_all();
    }

    static void rumble(unsigned short controller, unsigned short low, unsigned short high)
    {
        if (debug()) std::fprintf(stderr, "moonlight: rumble %u: %u %u\n", controller, low, high);
        self()->listener_.onGamepadRumble(controller, low, high);
    }

    static void rumbleTriggers(uint16_t controller, uint16_t left, uint16_t right)
    {
        self()->listener_.onGamepadTriggerRumble(controller, left, right);
    }

    static void log(const char* format, ...)
    {
        if (!debug()) return;
        va_list args;
        va_start(args, format);
        std::fputs("moonlight: ", stderr);
        std::vfprintf(stderr, format, args);
        va_end(args);
    }
};

MoonlightSession::MoonlightSession(DisplayListener& listener, Connection connection, std::string identityDir)
    : listener_(listener), connection_(std::move(connection)), identityDir_(std::move(identityDir)),
      config_(std::make_unique<Config>())
{
}

MoonlightSession::~MoonlightSession() { disconnect(); }

void MoonlightSession::connect()
{
    if (!worker_.joinable()) worker_ = std::jthread([this](std::stop_token st) { run(st); });
}

void MoonlightSession::disconnect()
{
    if (!worker_.joinable()) return;
    worker_.request_stop();
    cancel_.cancel();                       // a request in progress, e.g. pairing waiting for the PIN
    if (starting_) LiInterruptConnection();
    worker_.join();
}

void MoonlightSession::resizeDesktop(int width, int height)
{
    if (streaming_ || starting_) return;
    wantWidth_ = width & ~1;            // encoders want even sizes
    wantHeight_ = height & ~1;
}

void MoonlightSession::run(std::stop_token stop)
{
    listener_.onStateChanged(SessionState::Connecting);
    std::string error;
    bool started = false;
    MoonlightSession* none = nullptr;
    if (!activeSession.compare_exchange_strong(none, this)) {
        error = "Only one Moonlight stream can run at a time.";
    } else if (!prepare(&error)) {
        // empty error = a prompt was declined; the view closes the tab
    } else if (!stop.stop_requested()) {
        started = startStream(stop, &error);
    }

    if (started) {
        streaming_ = true;
        listener_.onStateChanged(SessionState::Connected);
        bool stopped = false; // a relaunch that failed has stopped the stream already
        for (;;) {
            bool relaunch = false;
            {
                std::stop_callback wake(stop, [this] {
                    std::lock_guard lock(mutex_);
                    ended_.notify_all();
                });
                std::unique_lock lock(mutex_);
                for (;;) {
                    ended_.wait(lock, [&] { return stop.stop_requested() || terminated_ || clipboardOut_ || clipboardPull_ || relaunch_; });
                    if (stop.stop_requested() || terminated_) break;
                    if ((relaunch = std::exchange(relaunch_, false))) break;
                    lock.unlock();
                    syncClipboard();
                    lock.lock();
                }
            }
            if (!relaunch) break;
            streaming_ = false;
            LiStopConnection();
            listener_.onStateChanged(SessionState::Connecting); // the view dims the picture meanwhile
            if (!launch(&error, true) || stop.stop_requested() || !startStream(stop, &error)) {
                stopped = true;
                break;
            }
            streaming_ = true;
            listener_.onStateChanged(SessionState::Connected);
        }
        streaming_ = false;
        if (!stopped) LiStopConnection();
        if (terminated_ && !stop.stop_requested() && terminationError_ != ML_ERROR_GRACEFUL_TERMINATION)
            error = terminationError_ == ML_ERROR_NO_VIDEO_TRAFFIC
                        ? "No video arrived from the host. Its firewall may block UDP ports 47998-48000."
                        : "The stream ended unexpectedly (error " + std::to_string(terminationError_) + ").";
        if (connection_.flag("quitAppOnClose")) {
            gamestream::Cancel fresh;
            std::string ignored;
            gamestream::request(host_, identity_, true, "cancel", "", 10, fresh, &ignored);
        }
    }
    if (activeSession == this) activeSession = nullptr;

    if (!error.empty() && !stop.stop_requested()) {
        listener_.onError(error);
        listener_.onStateChanged(started ? SessionState::Disconnected : SessionState::Error);
    } else {
        listener_.onStateChanged(SessionState::Disconnected);
    }
}

bool MoonlightSession::startStream(std::stop_token stop, std::string* error)
{
    {
        std::lock_guard lock(mutex_);
        terminated_ = false;
        terminationError_ = 0;
    }
    stageError_.clear();
    SERVER_INFORMATION server;
    LiInitializeServerInformation(&server);
    server.address = host_.address.c_str();
    server.serverInfoAppVersion = info_.appVersion.c_str();
    server.serverInfoGfeVersion = info_.gfeVersion.c_str();
    server.rtspSessionUrl = rtspUrl_.empty() ? nullptr : rtspUrl_.c_str();
    server.serverCodecModeSupport = info_.codecModes;
    CONNECTION_LISTENER_CALLBACKS cl;
    LiInitializeConnectionCallbacks(&cl);
    cl.stageFailed = &Callbacks::stageFailed;
    cl.connectionTerminated = &Callbacks::terminated;
    cl.logMessage = &Callbacks::log;
    cl.rumble = &Callbacks::rumble;
    cl.rumbleTriggers = &Callbacks::rumbleTriggers;
    DECODER_RENDERER_CALLBACKS dr;
    LiInitializeVideoCallbacks(&dr);
    dr.setup = &Callbacks::videoSetup;
    dr.submitDecodeUnit = &Callbacks::videoSubmit;
    AUDIO_RENDERER_CALLBACKS ar;
    LiInitializeAudioCallbacks(&ar);
    ar.init = &Callbacks::audioInit;
    ar.decodeAndPlaySample = &Callbacks::audioSample;
    starting_ = true;
    const bool started = LiStartConnection(&server, config_.get(), &cl, &dr, &ar, this, 0, this, 0) == 0;
    starting_ = false;
    if (!started && !stop.stop_requested()) *error = stageError_.empty() ? "Could not start the stream." : stageError_;
    return started;
}

bool MoonlightSession::prepare(std::string* error)
{
    if (!gamestream::loadIdentity(identityDir_, identity_, error)) return false;
    host_.address = connection_.host;
    host_.httpPort = connection_.effectivePort();
    std::string safe = host_.address + "_" + std::to_string(host_.httpPort);
    std::replace_if(safe.begin(), safe.end(), [](char c) { return !std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-'; }, '_');
    const fs::path certFile = fs::path(identityDir_) / "hosts" / (safe + ".pem");
    {
        std::ifstream in(certFile, std::ios::binary);
        host_.serverCertPem.assign(std::istreambuf_iterator<char>(in), {});
    }

    // plain HTTP first: it answers paired or not, and says where HTTPS is
    if (!gamestream::serverInfo(host_, identity_, false, info_, cancel_, error)) return false;
    host_.httpsPort = info_.httpsPort;
    gamestream::ServerInfo secure;
    std::string ignored;
    if (host_.serverCertPem.empty() || !gamestream::serverInfo(host_, identity_, true, secure, cancel_, &ignored) || !secure.paired) {
        if (cancel_.cancelled) return false;
        unsigned char random[2];
        RAND_bytes(random, sizeof random);
        char pin[5];
        std::snprintf(pin, sizeof pin, "%04u", unsigned(random[0] << 8 | random[1]) % 10000);
        const std::string where = info_.sunshine ? "open Sunshine's or Apollo's web interface (https://" + host_.address +
                                                       ":47990), go to PIN and enter:"
                                                 : "enter this PIN in the dialog GeForce Experience shows:";
        if (!listener_.askTrust("Pair with " + (info_.hostname.empty() ? host_.address : info_.hostname) + "?",
                                "On the host, " + where + "\n\n        " + pin +
                                    "\n\nThe connection continues by itself once the host accepts it.")) {
            error->clear();
            return false;
        }
        std::string cert;
        if (!gamestream::pair(host_, identity_, pin, info_.appVersion, cert, cancel_, error)) return false;
        host_.serverCertPem = cert;
        std::error_code ec;
        fs::create_directories(certFile.parent_path(), ec);
        std::ofstream(certFile, std::ios::binary) << cert;
        if (!gamestream::serverInfo(host_, identity_, true, secure, cancel_, error)) return false;
    }
    info_ = secure;

    const std::vector<gamestream::App> apps = gamestream::appList(host_, identity_, cancel_, error);
    if (apps.empty()) {
        if (error->empty()) *error = "The host offers no apps to stream.";
        return false;
    }
    auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string wanted = lower(connection_.option("app"));
    const auto app = std::find_if(apps.begin(), apps.end(), [&](const gamestream::App& a) { return lower(a.title) == wanted; });
    if (app == apps.end()) {
        std::string names;
        for (const gamestream::App& a : apps) names += (names.empty() ? "" : ", ") + a.title;
        *error = "The host has no app named \"" + connection_.option("app") + "\". It offers: " + names + ".";
        return false;
    }
    app_ = *app;
    return launch(error, false);
}

// relaunch: quits the app first, at the size relaunchAtSize() gave, for a new virtual display there
bool MoonlightSession::launch(std::string* error, bool relaunch)
{
    STREAM_CONFIGURATION& c = *config_;
    LiInitializeStreamConfiguration(&c);
    const std::string resolution = connection_.option("resolution");
    if ((relaunch || resolution == "window") && wantWidth_ >= 320 && wantHeight_ >= 200) {
        c.width = wantWidth_;
        c.height = wantHeight_;
    } else if (std::sscanf(resolution.c_str(), "%dx%d", &c.width, &c.height) != 2 || c.width < 320 || c.height < 200) {
        c.width = 1920;
        c.height = 1080;
    }
    c.fps = std::clamp(connection_.number("fps"), 10, 240);
    c.bitrate = std::clamp(connection_.number("bitrate"), 1, 500) * 1000;
    c.packetSize = 1392;
    c.streamingRemotely = STREAM_CFG_AUTO;
    c.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    const std::string codec = connection_.option("videoCodec");
    c.supportedVideoFormats = VIDEO_FORMAT_H264;
    if (codec == "h265" || codec == "av1") c.supportedVideoFormats |= VIDEO_FORMAT_H265;
    if (codec == "av1") c.supportedVideoFormats |= VIDEO_FORMAT_AV1_MAIN8;
    c.encryptionFlags = ENCFLG_ALL;
    RAND_bytes(reinterpret_cast<unsigned char*>(c.remoteInputAesKey), sizeof c.remoteInputAesKey);
    unsigned char keyId[4];
    RAND_bytes(keyId, sizeof keyId);
    keyId[0] &= 0x7F; // the host parses it as a signed int
    std::memset(c.remoteInputAesIv, 0, sizeof c.remoteInputAesIv);
    std::memcpy(c.remoteInputAesIv, keyId, sizeof keyId);
    const int rikeyid = keyId[0] << 24 | keyId[1] << 16 | keyId[2] << 8 | keyId[3];

    std::string verb = "launch";
    if (relaunch) {
        if (gamestream::request(host_, identity_, true, "cancel", "", 15, cancel_, error).empty()) return false;
    } else if (info_.currentGame == app_.id) {
        verb = "resume";
    } else if (info_.currentGame != 0) {
        if (!listener_.askTrust("Quit the app running on the host?",
                                "Another app is being streamed from the host. Quit it and start \"" + app_.title + "\"?")) {
            error->clear();
            return false;
        }
        if (gamestream::request(host_, identity_, true, "cancel", "", 15, cancel_, error).empty()) return false;
    }
    const std::string query = "appid=" + std::to_string(app_.id) + "&mode=" + std::to_string(c.width) + "x" +
                              std::to_string(c.height) + "x" + std::to_string(c.fps) + "&additionalStates=1&sops=0&rikey=" +
                              gamestream::toHex(std::string(c.remoteInputAesKey, sizeof c.remoteInputAesKey)) +
                              "&rikeyid=" + std::to_string(rikeyid) +
                              "&localAudioPlayMode=" + (connection_.flag("hostAudio") ? "1" : "0") +
                              "&surroundAudioInfo=" + std::to_string(SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(c.audioConfiguration)) +
                              "&remoteControllersBitmap=0&gcmap=0&gcpersist=0" + (connection_.flag("virtualDisplay") ? "&virtualDisplay=1" : "") +
                              LiGetLaunchUrlQueryParameters();
    const std::string xml = gamestream::request(host_, identity_, true, verb, query, 120, cancel_, error);
    if (xml.empty()) {
        if (error->starts_with("Permission denied")) // Apollo: only the first client paired gets full rights
            *error += "\n\nOn Apollo, allow this client (Portal Mux) to launch apps and send input: web interface → paired clients.";
        return false;
    }
    rtspUrl_ = gamestream::xmlValue(xml, "sessionUrl0");
    return true;
}

void MoonlightSession::relaunchAtSize(int width, int height)
{
    if (!streaming_) return;
    std::lock_guard lock(mutex_);
    wantWidth_ = width & ~1;
    wantHeight_ = height & ~1;
    relaunch_ = true;
    ended_.notify_all();
}

void MoonlightSession::sendClipboardText(const std::string& text)
{
    std::lock_guard lock(mutex_);
    clipboardOut_ = text;
    ended_.notify_all();
}

void MoonlightSession::pullClipboard()
{
    std::lock_guard lock(mutex_);
    clipboardPull_ = true;
    ended_.notify_all();
}

void MoonlightSession::syncClipboard()
{
    std::optional<std::string> out;
    bool pull = false;
    {
        std::lock_guard lock(mutex_);
        out = std::exchange(clipboardOut_, std::nullopt);
        pull = std::exchange(clipboardPull_, false);
    }
    std::string received, error;
    if (out && *out != hostClipboard_ && (info_.apolloPermissions & gamestream::apolloClipboardSet) &&
        gamestream::clipboard(host_, identity_, &*out, received, cancel_, &error))
        hostClipboard_ = *out;
    if (!pull || !(info_.apolloPermissions & gamestream::apolloClipboardRead) ||
        !gamestream::clipboard(host_, identity_, nullptr, received, cancel_, &error))
        return;
    std::string text; // Windows line ends to ours
    for (size_t i = 0; i < received.size(); ++i)
        if (received[i] != '\r' || i + 1 == received.size() || received[i + 1] != '\n') text += received[i];
    if (!text.empty() && text != hostClipboard_) listener_.onClipboardText(hostClipboard_ = text);
}

void MoonlightSession::sendKey(const KeyEvent& e)
{
    if (!streaming_ || !e.scancode) return;
    // US-layout virtual-key codes: physical positions, the host's layout applies
    const DWORD vk = GetVirtualKeyCodeFromVirtualScanCode(e.scancode, WINPR_KBD_TYPE_IBM_ENHANCED) & 0xFF;
    if (!vk) return;
    uint8_t bit = 0;
    switch (vk) {
    case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT: bit = MODIFIER_SHIFT; break;
    case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: bit = MODIFIER_CTRL; break;
    case VK_LMENU: case VK_RMENU: case VK_MENU: bit = MODIFIER_ALT; break;
    case VK_LWIN: case VK_RWIN: bit = MODIFIER_META; break;
    }
    if (bit) modifiers_ = e.pressed ? modifiers_ | bit : modifiers_ & ~bit;
    const char modifiers = char(modifiers_ | (e.scancode & 0x100 ? MODIFIER_EXTENDED : 0));
    LiSendKeyboardEvent2(short(0x8000 | vk), e.pressed ? KEY_ACTION_DOWN : KEY_ACTION_UP, modifiers, 0);
}

void MoonlightSession::sendPointer(const PointerEvent& e)
{
    const int w = width_, h = height_;
    if (!streaming_ || !w || !h) return;
    if (e.x != lastX_ || e.y != lastY_) {
        LiSendMousePositionEvent(short(std::clamp(e.x, 0, w - 1)), short(std::clamp(e.y, 0, h - 1)), short(w), short(h));
        lastX_ = e.x;
        lastY_ = e.y;
    }
    const uint8_t changed = e.buttons ^ buttons_;
    buttons_ = e.buttons;
    for (auto [mask, button] : {std::pair{PointerEvent::Left, BUTTON_LEFT}, {PointerEvent::Middle, BUTTON_MIDDLE},
                                {PointerEvent::Right, BUTTON_RIGHT}})
        if (changed & mask) LiSendMouseButtonEvent(char(e.buttons & mask ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE), button);
    if (e.wheelDelta) LiSendHighResScrollEvent(short(e.wheelDelta));
}

void MoonlightSession::sendGamepad(const GamepadState& g)
{
    if (!streaming_ || g.index < 0 || g.index > 15) return;
    const auto bit = uint16_t(1u << g.index);
    if (!g.present) {
        if (!(gamepadMask_ & bit)) return;
        gamepadMask_ &= uint16_t(~bit);
        LiSendMultiControllerEvent(short(g.index), short(gamepadMask_), 0, 0, 0, 0, 0, 0, 0);
        return;
    }
    if (!(gamepadMask_ & bit)) {
        gamepadMask_ |= bit;
        static constexpr uint8_t types[] = {LI_CTYPE_UNKNOWN, LI_CTYPE_XBOX, LI_CTYPE_PS, LI_CTYPE_NINTENDO};
        LiSendControllerArrivalEvent(uint8_t(g.index), gamepadMask_, types[int(g.type)], g.supportedButtons, g.capabilities);
    }
    LiSendMultiControllerEvent(short(g.index), short(gamepadMask_), int(g.buttons), g.leftTrigger, g.rightTrigger, g.leftX,
                               g.leftY, g.rightX, g.rightY);
}

} // namespace rdc
