#include "android/ScrcpySession.h"

#include "android/ScrcpyMessages.h"
#include "core/Properties.h"
#include "media/AudioDecoder.h"
#include "media/VideoDecoder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <random>

namespace rdc {

// stream format: scrcpy app/src/demuxer.c, server.c
namespace {
constexpr const char* DeviceServerPath = "/data/local/tmp/scrcpy-server.jar";
constexpr uint64_t ConfigFlag = UINT64_C(1) << 62, KeyFrameFlag = UINT64_C(1) << 61, PtsMask = KeyFrameFlag - 1;
constexpr uint32_t CodecH264 = 0x68323634, CodecH265 = 0x68323635, CodecAV1 = 0x00617631;
constexpr uint32_t CodecOpus = 0x6f707573, CodecAAC = 0x00616163, CodecFLAC = 0x666c6163, CodecRaw = 0x00726177;
constexpr uint32_t MaxPacket = 64 << 20;

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

bool debug() { return std::getenv("PORTAL_MUX_DEBUG") != nullptr; }
} // namespace

ScrcpySession::ScrcpySession(DisplayListener& listener, Connection connection, std::string serverPath)
    : listener_(listener), connection_(std::move(connection)), serverPath_(std::move(serverPath)),
      hid_(connection_.option("keyboard") == "hid")
{
}

void ScrcpySession::connect()
{
    if (!worker_.joinable()) worker_ = std::jthread([this](std::stop_token st) { run(st); });
}

void ScrcpySession::disconnect()
{
    if (!worker_.joinable()) return;
    worker_.request_stop();
    {
        std::lock_guard lock(socketsMutex_);
        for (adb::Socket* s : {&shell_, &video_, &audio_, &control_}) s->shutdown(); // unblocks every reader
    }
    worker_.join();
}

void ScrcpySession::run(std::stop_token stop)
{
    listener_.onStateChanged(SessionState::Connecting);
    std::string error;
    const bool started = start(stop, &error);
    if (started) {
        listener_.onStateChanged(SessionState::Connected);
        controlReady_ = true;
        if (hid_)
            send(scrcpy::uhidCreate(scrcpy::HidKeyboardId, "Portal Mux", scrcpy::HidKeyboardDescriptor,
                                    sizeof scrcpy::HidKeyboardDescriptor));
        if (connection_.flag("turnScreenOff")) send(scrcpy::displayPower(false));
        if (audio_.valid()) audioThread_ = std::thread([this] { readAudio(); });
        controlThread_ = std::thread([this] { readControl(); });
        readVideo();
        if (!stop.stop_requested()) error = "The connection to the device was lost.";
    }
    controlReady_ = false;
    {
        std::lock_guard lock(socketsMutex_);
        for (adb::Socket* s : {&shell_, &video_, &audio_, &control_}) s->shutdown(); // the server exits with them
    }
    for (std::thread* t : {&audioThread_, &controlThread_, &shellThread_})
        if (t->joinable()) t->join();
    if (!stop.stop_requested() && !error.empty()) {
        if (const std::string out = serverOutput(); !out.empty()) error += "\n\nscrcpy server:\n" + out;
        listener_.onError(error);
        listener_.onStateChanged(started ? SessionState::Disconnected : SessionState::Error);
    } else {
        listener_.onStateChanged(SessionState::Disconnected);
    }
}

bool ScrcpySession::start(std::stop_token stop, std::string* error)
{
    const Connection& c = connection_;
    const std::string adbPath = adb::findAdb(c.option("adb"));
    std::string serial = c.host;

    std::vector<std::string> present = adb::devices(adbPath, error);
    if (!error->empty()) return false;
    auto listed = [&] { return std::find(present.begin(), present.end(), serial) != present.end(); };
    if (!serial.empty() && !listed() && serial.find(':') != std::string::npos) {
        if (!adb::connectDevice(adbPath, serial, error)) return false;
        present = adb::devices(adbPath, error);
    }
    std::string names;
    for (const std::string& s : present) names += (names.empty() ? "" : ", ") + s;
    if (serial.empty()) {
        if (present.size() != 1) {
            *error = present.empty() ? "No Android device found. Connect one with USB debugging enabled, or enter the "
                                       "address (host:port) of a device with wireless debugging."
                                     : "More than one Android device is connected (" + names +
                                           "). Enter the serial of the one to use as the connection's device.";
            return false;
        }
        serial = present.front();
    } else if (!listed()) {
        *error = "Android device \"" + serial + "\" is not connected" +
                 (names.empty() ? "." : " (adb lists: " + names + ").") +
                 " Check the cable or wireless debugging, and that the device allowed this computer.";
        return false;
    }

    if (!adb::push(adbPath, serial, serverPath_, DeviceServerPath, error)) return false;

    // values end up in a device shell command
    auto choice = [&](const char* key, std::initializer_list<const char*> allowed) {
        const std::string v = c.option(key);
        for (const char* a : allowed)
            if (v == a) return v;
        return std::string(*allowed.begin());
    };
    char scid[9];
    std::snprintf(scid, sizeof scid, "%08x", unsigned(std::random_device{}() & 0x7FFFFFFF));
    std::string cmd = std::string("CLASSPATH=") + DeviceServerPath +
                      " app_process / com.genymobile.scrcpy.Server " SCRCPY_SERVER_VERSION " scid=" + scid +
                      " log_level=info tunnel_forward=true";
    cmd += " video_bit_rate=" + std::to_string(std::max(1, c.number("videoBitRate")) * 1'000'000);
    const std::string vcodec = choice("videoCodec", {"h264", "h265", "av1"});
    videoCodec_ = vcodec == "h265" ? CodecH265 : vcodec == "av1" ? CodecAV1 : CodecH264;
    if (vcodec != "h264") cmd += " video_codec=" + vcodec;
    if (c.number("maxSize") > 0) cmd += " max_size=" + std::to_string(c.number("maxSize"));
    if (c.number("maxFps") > 0) cmd += " max_fps=" + std::to_string(c.number("maxFps"));
    if (c.flag("audio")) {
        if (const std::string a = choice("audioCodec", {"opus", "aac", "flac", "raw"}); a != "opus") cmd += " audio_codec=" + a;
        if (const std::string a = choice("audioSource", {"output", "playback", "mic"}); a != "output") cmd += " audio_source=" + a;
        cmd += " audio_bit_rate=" + std::to_string(std::max(8, c.number("audioBitRate")) * 1000);
    } else {
        cmd += " audio=false";
    }
    if (c.number("displayId") > 0) cmd += " display_id=" + std::to_string(c.number("displayId"));
    if (c.flag("showTouches")) cmd += " show_touches=true";
    if (c.flag("stayAwake")) cmd += " stay_awake=true";
    if (c.flag("powerOffOnClose")) cmd += " power_off_on_close=true";
    if (debug()) std::fprintf(stderr, "scrcpy: %s\n", cmd.c_str());

    {
        adb::Socket shell = adb::open(adbPath, serial, "shell:" + cmd, error);
        if (!shell.valid()) return false;
        std::lock_guard lock(socketsMutex_);
        if (stop.stop_requested()) return false;
        shell_ = std::move(shell);
    }
    shellThread_ = std::thread([this] { readShell(); });

    // first socket gets a dummy byte: tells the server from a bare adb connection
    const std::string socketName = std::string("localabstract:scrcpy_") + scid;
    auto openSocket = [&](adb::Socket& into, bool first) {
        for (int attempt = 0; attempt < 100 && !stop.stop_requested(); ++attempt) {
            std::string ignored;
            adb::Socket s = adb::open(adbPath, serial, socketName, &ignored);
            char dummy;
            if (s.valid() && (!first || s.readAll(&dummy, 1))) {
                std::lock_guard lock(socketsMutex_);
                if (stop.stop_requested()) return false;
                into = std::move(s);
                return true;
            }
            if (!first || !shellThread_.joinable() || serverExited_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    };
    if (!openSocket(video_, true) || (c.flag("audio") && !openSocket(audio_, false)) || !openSocket(control_, false)) {
        if (!stop.stop_requested()) *error = "The scrcpy server on the device did not start.";
        return false;
    }

    uint8_t deviceName[64], codec[4];
    if (!video_.readAll(deviceName, sizeof deviceName) || !video_.readAll(codec, sizeof codec)) {
        *error = "The scrcpy server closed the connection.";
        return false;
    }
    if (be32(codec) != videoCodec_) {
        *error = be32(codec) == 0 ? "The device disabled video." : "The device could not start the video encoder.";
        return false;
    }
    return true;
}

void ScrcpySession::readVideo()
{
    VideoDecoder decoder;
    std::string error;
    const auto codec = videoCodec_ == CodecH265 ? VideoDecoder::Codec::H265
                     : videoCodec_ == CodecAV1  ? VideoDecoder::Codec::AV1
                                                : VideoDecoder::Codec::H264;
    if (!decoder.open(codec, connection_.flag("hardwareDecoding"), &error)) {
        listener_.onError(error);
        return;
    }
    const bool mergeConfig = codec != VideoDecoder::Codec::AV1; // prepend H.26x parameter sets to the next frame
    std::vector<uint8_t> config, packet;
    uint8_t header[12];
    while (video_.readAll(header, sizeof header)) {
        if (header[0] & 0x80) continue; // session packet: frames carry their size
        const uint64_t ptsFlags = be64(header);
        const uint32_t len = be32(header + 8);
        if (len == 0 || len > MaxPacket) break;
        packet.resize(len);
        if (!video_.readAll(packet.data(), len)) break;
        if (ptsFlags & ConfigFlag) {
            if (mergeConfig) config = packet;
            continue;
        }
        if (!config.empty()) {
            packet.insert(packet.begin(), config.begin(), config.end());
            config.clear();
        }
        const int oldWidth = decoder.width(), oldHeight = decoder.height();
        if (decoder.decode(packet.data(), packet.size(), int64_t(ptsFlags & PtsMask), framebuffer_) > 0) {
            if (decoder.width() != oldWidth || decoder.height() != oldHeight) {
                width_ = decoder.width();
                height_ = decoder.height();
                listener_.onResize(decoder.width(), decoder.height());
            }
            listener_.onFrameUpdated({0, 0, decoder.width(), decoder.height()});
        }
    }
}

void ScrcpySession::readAudio()
{
    uint8_t id[4];
    if (!audio_.readAll(id, sizeof id)) return;
    const uint32_t codecId = be32(id);
    AudioDecoder::Codec codec;
    switch (codecId) {
    case CodecOpus: codec = AudioDecoder::Codec::Opus; break;
    case CodecAAC: codec = AudioDecoder::Codec::AAC; break;
    case CodecFLAC: codec = AudioDecoder::Codec::FLAC; break;
    case CodecRaw: codec = AudioDecoder::Codec::Raw; break;
    default: // 0: disabled (Android < 11), 1: capture failed
        if (debug()) std::fprintf(stderr, "scrcpy: no audio (codec id %u)\n", codecId);
        return;
    }
    AudioDecoder decoder;
    std::string error;
    if (!decoder.open(codec, 48000, 2, &error)) {
        if (debug()) std::fprintf(stderr, "scrcpy: %s\n", error.c_str());
        return;
    }
    std::vector<uint8_t> packet;
    std::vector<int16_t> pcm;
    bool audioSeen = false;
    uint8_t header[12];
    while (audio_.readAll(header, sizeof header)) {
        const uint64_t ptsFlags = be64(header);
        const uint32_t len = be32(header + 8);
        if (len == 0 || len > MaxPacket) break;
        packet.resize(len);
        if (!audio_.readAll(packet.data(), len)) break;
        if (ptsFlags & ConfigFlag) continue; // FFmpeg's decoders do without codec headers
        decoder.decode(packet.data(), packet.size(), pcm);
        if (!pcm.empty() && debug() && !audioSeen) std::fprintf(stderr, "scrcpy: audio playing (codec %08x)\n", codecId);
        audioSeen = audioSeen || !pcm.empty();
        if (!pcm.empty()) listener_.onAudio(pcm.data(), pcm.size() / 2, 48000, 2);
        pcm.clear();
    }
}

void ScrcpySession::readControl()
{
    uint8_t type;
    while (control_.readAll(&type, 1)) {
        if (type == 0) {
            uint8_t len[4];
            if (!control_.readAll(len, 4) || be32(len) > MaxPacket) break;
            std::string text(be32(len), '\0');
            if (!text.empty() && !control_.readAll(text.data(), text.size())) break;
            listener_.onClipboardText(std::move(text));
        } else if (type == 1) {
            uint8_t sequence[8];
            if (!control_.readAll(sequence, 8)) break;
        } else if (type == 2) {
            uint8_t head[4];
            if (!control_.readAll(head, 4)) break;
            std::vector<uint8_t> data(size_t(head[2]) << 8 | head[3]);
            if (!data.empty() && !control_.readAll(data.data(), data.size())) break;
            if ((head[0] << 8 | head[1]) == scrcpy::HidKeyboardId && !data.empty()) deviceLeds_ = data[0];
        } else {
            break; // unknown message: can't follow the stream
        }
    }
}

void ScrcpySession::readShell()
{
    char buf[4096];
    for (long n; (n = shell_.readSome(buf, sizeof buf)) > 0;) {
        if (debug()) std::fwrite(buf, 1, size_t(n), stderr);
        std::lock_guard lock(outputMutex_);
        output_.append(buf, size_t(n));
        if (output_.size() > 16384) output_.erase(0, output_.size() - 16384);
    }
    serverExited_ = true;
}

std::string ScrcpySession::serverOutput()
{
    std::lock_guard lock(outputMutex_);
    std::string out = output_;
    size_t start = out.size(), lines = 0;
    while (start > 0 && lines < 12)
        if (out[--start] == '\n' && start + 1 < out.size()) ++lines;
    out = out.substr(start);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

void ScrcpySession::send(const std::vector<uint8_t>& message)
{
    if (!controlReady_) return;
    std::lock_guard lock(sendMutex_);
    control_.writeAll(message.data(), message.size()); // a failed write ends the session via the readers
}

void ScrcpySession::sendKey(const KeyEvent& e)
{
    if (hid_) {
        sendHidKey(e);
        return;
    }
    if (const uint32_t meta = scrcpy::metaOf(e.keysym)) metaState_ = e.pressed ? metaState_ | meta : metaState_ & ~meta;
    if (const uint32_t code = scrcpy::androidKeycode(e.keysym)) {
        uint32_t meta = metaState_;
        const bool upper = e.keysym >= 'A' && e.keysym <= 'Z';
        if (upper && !(meta & scrcpy::MetaShift)) meta |= scrcpy::MetaCapsLock; // Caps Lock made it upper case
        send(scrcpy::keycode(e.pressed ? scrcpy::KeyDown : scrcpy::KeyUp, code, meta));
        return;
    }
    // as text, like scrcpy's mixed mode; with Ctrl/Alt/Meta it would be a shortcut
    const bool shortcut = metaState_ & (scrcpy::MetaCtrl | scrcpy::MetaAlt | scrcpy::MetaMeta);
    if (e.pressed && !shortcut && !e.text.empty() && uint8_t(e.text[0]) >= 0x20 && e.text[0] != 0x7F)
        send(scrcpy::text(e.text));
}

void ScrcpySession::sendHidKey(const KeyEvent& e)
{
    const uint8_t usage = scrcpy::hidUsage(e.scancode);
    if (usage >= 0xE0 && usage <= 0xE7) {
        const auto bit = uint8_t(1 << (usage - 0xE0));
        hidMods_ = e.pressed ? hidMods_ | bit : hidMods_ & ~bit;
    } else if (usage > 0 && usage < scrcpy::HidKeyboardKeys) {
        if (hidKeys_[usage] == e.pressed) return; // auto-repeat: the device repeats held keys itself
        const bool asciiLetter = (e.keysym >= 'a' && e.keysym <= 'z') || (e.keysym >= 'A' && e.keysym <= 'Z');
        if (e.pressed && asciiLetter) {
            // Caps Lock may differ on the device: align it first
            const bool shift = hidMods_ & (0x02 | 0x20);
            const bool localCaps = (e.keysym <= 'Z') != shift;
            if (localCaps != bool(deviceLeds_ & scrcpy::HidLedCapsLock)) {
                hidKeys_[scrcpy::HidCapsLock] = true;
                sendHidReport();
                hidKeys_[scrcpy::HidCapsLock] = false;
                sendHidReport();
                deviceLeds_ ^= scrcpy::HidLedCapsLock; // until the LED report confirms it
            }
        }
        if (usage == scrcpy::HidCapsLock && e.pressed) deviceLeds_ ^= scrcpy::HidLedCapsLock;
        hidKeys_[usage] = e.pressed;
    } else {
        return;
    }
    sendHidReport();
}

void ScrcpySession::sendHidReport()
{
    uint8_t report[8] = {hidMods_, 0};
    int count = 0;
    for (int usage = 0; usage < scrcpy::HidKeyboardKeys; ++usage) {
        if (!hidKeys_[usage]) continue;
        if (count == 6) { // more than 6 keys: rollover error
            std::fill(report + 2, report + 8, uint8_t(1));
            break;
        }
        report[2 + count++] = uint8_t(usage);
    }
    send(scrcpy::uhidInput(scrcpy::HidKeyboardId, report, sizeof report));
}

void ScrcpySession::sendPointer(const PointerEvent& e)
{
    const int w = width_, h = height_;
    if (!w || !h) return;
    const int x = std::clamp(e.x, 0, w - 1), y = std::clamp(e.y, 0, h - 1);
    const bool left = e.buttons & PointerEvent::Left;
    const uint32_t buttons = left ? uint32_t(scrcpy::ButtonPrimary) : 0;
    if (e.wheelDelta) {
        send(scrcpy::scroll(x, y, w, h, 0, e.wheelDelta / 120.0f, buttons));
        return;
    }
    const uint8_t changed = e.buttons ^ buttons_;
    buttons_ = e.buttons;
    if (changed & PointerEvent::Left)
        send(scrcpy::touch(left ? scrcpy::TouchDown : scrcpy::TouchUp, x, y, w, h, left, scrcpy::ButtonPrimary, buttons));
    if (changed & PointerEvent::Right)
        send(scrcpy::backOrScreenOn(e.buttons & PointerEvent::Right ? scrcpy::KeyDown : scrcpy::KeyUp));
    if (changed & PointerEvent::Middle)
        send(scrcpy::keycode(e.buttons & PointerEvent::Middle ? scrcpy::KeyDown : scrcpy::KeyUp, scrcpy::KeycodeHome, 0));
    if (!changed) send(scrcpy::touch(left ? scrcpy::TouchMove : scrcpy::HoverMove, x, y, w, h, left, 0, buttons));
}

void ScrcpySession::sendClipboardText(const std::string& text)
{
    send(scrcpy::setClipboard(++clipboardSequence_, text));
}

} // namespace rdc
