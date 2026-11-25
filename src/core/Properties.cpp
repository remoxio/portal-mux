#include "core/Properties.h"
#include "terminal/ColorSchemes.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#ifndef _WIN32
#include <pwd.h>
#include <unistd.h>
#endif

namespace rdc {

using K = PropertyKind;

const std::vector<KeyboardLayout>& keyboardLayouts()
{
    static const std::vector<KeyboardLayout> layouts = {
        {"us", "English (US)", 0x409}, {"gb", "English (UK)", 0x809},   {"de", "German", 0x407},
        {"ch", "Swiss German", 0x807}, {"fr", "French", 0x40C},         {"be", "Belgian French", 0x80C},
        {"it", "Italian", 0x410},      {"es", "Spanish", 0x40A},        {"pt", "Portuguese", 0x816},
        {"br", "Portuguese (Brazil)", 0x416}, {"nl", "Dutch", 0x413},   {"dk", "Danish", 0x406},
        {"no", "Norwegian", 0x414},    {"se", "Swedish", 0x41D},        {"fi", "Finnish", 0x40B},
        {"pl", "Polish", 0x415},       {"cz", "Czech", 0x405},          {"sk", "Slovak", 0x41B},
        {"hu", "Hungarian", 0x40E},    {"ro", "Romanian", 0x418},       {"hr", "Croatian", 0x41A},
        {"si", "Slovenian", 0x424},    {"ru", "Russian", 0x419},        {"ua", "Ukrainian", 0x422},
        {"tr", "Turkish", 0x41F},      {"gr", "Greek", 0x408},          {"jp", "Japanese", 0x411},
        {"kr", "Korean", 0x412},
    };
    return layouts;
}

static std::vector<PropertyDef> displayCommon(bool canResizeRemote)
{
    std::vector<Choice> scaling = {{"scale", "Fit to window"}, {"none", "1:1 with scrollbars"}};
    if (canResizeRemote) scaling.insert(scaling.begin() + 1, {"resize", "Resize the remote desktop, no scaling"});
    return {
        {"scaling", "Window mode", "Display", K::Choice, "scale", scaling, 0, 0,
         canResizeRemote ? "Fit: the remote desktop takes the tab's size where it can (RDP without a fixed size, VNC servers that allow it, "
                           "SPICE with the guest agent); what can't follow is scaled, keeping its proportions"
                         : "Fit: scaled to the tab, keeping its proportions"},
        {"viewOnly", "View only (send no keyboard or mouse input)", "General", K::Bool, "false"},
        {"clipboard", "Share clipboard", "General", K::Bool, "true"},
    };
}

static void addDisplayCommon(std::vector<PropertyDef>& p, bool canResizeRemote)
{
    std::vector<PropertyDef> common = displayCommon(canResizeRemote);
    p.insert(p.begin(), common.front());
    p.insert(p.end(), common.begin() + 1, common.end());
}

static std::vector<PropertyDef> tunnelCommon()
{
    return {
        // an SSH server of Settings (its id); an imported "[user@]host[:port]" still works
        {"tunnel", "SSH server", "SSH tunnel", K::Choice, "", {{"", "None: connect directly"}, {"group", "As the group"}}, 0, 0,
         "The connection goes through this SSH server; servers are added in Settings → SSH Servers"},
    };
}

static void addStartCommon(std::vector<PropertyDef>& p, bool wakeOnLan)
{
    if (wakeOnLan)
        p.push_back({"wakeOnLan", "Wake-on-LAN MAC address", "Advanced", K::Text, "", {}, 0, 0,
                     "e.g. 00:11:22:33:44:55; sent before connecting, then the connection waits up to two minutes for the host"});
    p.push_back({"preCommand", "Command before connecting", "Advanced", K::Text, "", {}, 0, 0,
                 "Run by the shell, and connecting waits for it; PORTAL_MUX_NAME, PORTAL_MUX_HOST, PORTAL_MUX_PORT and PORTAL_MUX_USER are set"});
    p.push_back({"postCommand", "Command after disconnecting", "Advanced", K::Text, "", {}, 0, 0, "Run by the shell when the tab closes"});
}

static std::vector<PropertyDef> rdpProperties()
{
    std::vector<Choice> layouts = {{"0", "Automatic"}};
    for (const KeyboardLayout& l : keyboardLayouts()) layouts.push_back({std::to_string(l.klid), l.name});

    std::vector<PropertyDef> p = {
        {"domain", "Domain", "General", K::Text, ""},
        {"width", "Desktop width", "Display", K::Int, "0", {}, 0, 8192, "Used when the window mode is not 'Resize'; 0 = window size at connect"},
        {"height", "Desktop height", "Display", K::Int, "0", {}, 0, 8192},
        {"multimon", "Use all monitors", "Display", K::Bool, "false", {}, 0, 0,
         "The remote desktop spans this computer's monitors; Full Screen shows each part on its own monitor"},
        {"colorDepth", "Color depth", "Display", K::Choice, "32", {{"32", "32 bit"}, {"24", "24 bit"}, {"16", "16 bit"}, {"15", "15 bit"}, {"8", "8 bit"}}},
        {"scale", "Remote scale (%)", "Display", K::Int, "100", {}, 50, 500,
         "Size of the remote's text and icons; 100 = as here (the server gets 200% on a Retina display); "
         "with a fixed desktop size, the server's own"},
        {"connectionType", "Connection type", "Experience", K::Choice, "auto",
         {{"auto", "Detect automatically"}, {"lan", "LAN"}, {"broadband-high", "High-speed broadband"}, {"wan", "WAN"},
          {"broadband-low", "Low-speed broadband"}, {"satellite", "Satellite"}, {"modem", "Modem"}, {"custom", "Custom (options below)"}}},
        {"wallpaper", "Desktop background", "Experience", K::Bool, "true", {}, 0, 0, "Custom connection type only"},
        {"themes", "Visual styles", "Experience", K::Bool, "true", {}, 0, 0, "Custom connection type only"},
        {"fontSmoothing", "Font smoothing", "Experience", K::Bool, "true", {}, 0, 0, "Custom connection type only"},
        {"composition", "Desktop composition", "Experience", K::Bool, "true", {}, 0, 0, "Custom connection type only"},
        {"fullWindowDrag", "Show window contents while dragging", "Experience", K::Bool, "true", {}, 0, 0, "Custom connection type only"},
        {"menuAnimations", "Menu and window animations", "Experience", K::Bool, "true", {}, 0, 0, "Custom connection type only"},
        {"gfx", "Graphics pipeline (RDP 8+)", "Experience", K::Bool, "true"},
        {"h264", "H.264 / AVC444 video", "Experience", K::Bool, "true"},
        {"audio", "Remote audio", "Local resources", K::Choice, "local", {{"local", "Play on this computer"}, {"remote", "Play on the remote computer"}, {"off", "Do not play"}}},
        {"microphone", "Microphone", "Local resources", K::Bool, "false"},
        {"drives", "Shared folders", "Local resources", K::Text, "", {}, 0, 0, "Local folders shown as drives on the remote side, separated by ';'"},
        {"printers", "Printers", "Local resources", K::Bool, "false"},
        {"smartcards", "Smart cards", "Local resources", K::Bool, "false"},
        {"keyboardLayout", "Keyboard layout", "Local resources", K::Choice, "0", layouts},
        {"security", "Security protocol", "Security", K::Choice, "auto",
         {{"auto", "Negotiate"}, {"nla", "NLA (CredSSP)"}, {"tls", "TLS"}, {"rdp", "Standard RDP"}, {"ext", "Extended NLA"},
          {"aad", "Microsoft Entra ID (sign in in the browser)"}},
         0, 0, "Entra ID: for Entra-joined Windows; the sign-in opens in the browser"},
        {"tlsSecLevel", "OpenSSL security level", "Security", K::Int, "1", {}, 0, 5},
        {"restrictedAdmin", "Restricted admin mode", "Security", K::Bool, "false"},
        {"remoteCredentialGuard", "Remote Credential Guard", "Security", K::Bool, "false"},
        {"alwaysTrust", "Always trust the server certificate", "Security", K::Bool, "false", {}, 0, 0,
         "No question when it is unknown, doesn't match the host name or has changed (also the gateway's)"},
        {"gateway.host", "Gateway host", "Gateway", K::Text, "", {}, 0, 0, "Empty = no RD Gateway"},
        {"gateway.port", "Gateway port", "Gateway", K::Int, "443", {}, 1, 65535},
        {"gateway.usage", "Use gateway", "Gateway", K::Choice, "direct", {{"direct", "Always"}, {"detect", "Bypass for local addresses"}}},
        {"gateway.transport", "Transport", "Gateway", K::Choice, "auto", {{"auto", "Automatic"}, {"http", "HTTP"}, {"rpc", "RPC"}}},
        {"gateway.sameCredentials", "Use the connection's credentials", "Gateway", K::Bool, "true"},
        {"gateway.user", "Gateway user name", "Gateway", K::Text, ""},
        {"gateway.domain", "Gateway domain", "Gateway", K::Text, ""},
        {"gateway.password", "Gateway password", "Gateway", K::Secret, ""},
        {"console", "Connect to the console (admin) session", "Advanced", K::Bool, "false"},
        {"alternateShell", "Start program", "Advanced", K::Text, ""},
        {"remoteApp", "RemoteApp program", "Advanced", K::Text, "", {}, 0, 0,
         "Only this program instead of the desktop: its published alias (||calc) or its path on the server"},
        {"remoteAppArgs", "RemoteApp arguments", "Advanced", K::Text, ""},
        {"workingDir", "Start in folder", "Advanced", K::Text, ""},
        {"loadBalanceInfo", "Load balance info", "Advanced", K::Text, "", {}, 0, 0, "Routing token for RD Connection Broker"},
        {"preconnectionBlob", "Pre-connection blob", "Advanced", K::Text, "", {}, 0, 0, "Hyper-V VM id for direct VM console connections"},
        {"timeout", "Connect timeout (s)", "Advanced", K::Int, "15", {}, 1, 300},
        {"autoReconnect", "Reconnect automatically when the network drops", "Advanced", K::Bool, "true", {}, 0, 0,
         "Back into the same remote session, without asking for the password again"},
    };
    addDisplayCommon(p, true);
    addStartCommon(p, true);
    for (auto& t : tunnelCommon()) p.push_back(t);
    return p;
}

static std::vector<PropertyDef> vncProperties()
{
    std::vector<PropertyDef> p = {
        {"jpegQuality", "JPEG quality", "Display", K::Int, "8", {}, 0, 9, "Tight encoding; lower = less bandwidth"},
        {"compression", "Compression level", "Display", K::Int, "5", {}, 0, 9},
        {"encodings", "Encodings (in order)", "Display", K::Text, "tight zrle ultra copyrect hextile zlib corre rre raw"},
        {"remoteCursor", "Draw the cursor locally", "Display", K::Bool, "true"},
        {"shared", "Shared session (keep other viewers connected)", "General", K::Bool, "true"},
        {"security", "Allowed security", "Security", K::Choice, "any", {{"any", "Any the server offers"}, {"encrypted", "Encrypted only (VeNCrypt/TLS)"}}},
        {"timeout", "Connect timeout (s)", "Advanced", K::Int, "10", {}, 1, 300},
    };
    addDisplayCommon(p, true);
    addStartCommon(p, true);
    for (auto& t : tunnelCommon()) p.push_back(t);
    return p;
}

static std::vector<PropertyDef> spiceProperties()
{
    std::vector<PropertyDef> p = {
        {"tlsPort", "TLS port", "Security", K::Int, "0", {}, 0, 65535, "0 = plain connection on the port"},
        {"caFile", "CA certificate", "Security", K::File, "", {}, 0, 0, "Empty = system trust store"},
        {"hostSubject", "Certificate subject", "Security", K::Text, "", {}, 0, 0, "e.g. C=XX,L=city,O=org,CN=host"},
        {"proxy", "HTTP proxy", "Advanced", K::Text, "", {}, 0, 0, "http://host:port (Proxmox spiceproxy)"},
        {"audio", "Audio playback and recording", "Local resources", K::Bool, "true"},
        {"sharedFolder", "Shared folder", "Local resources", K::Directory, "", {}, 0, 0, "Needs spice-webdavd in the guest"},
        {"sharedFolderReadOnly", "Shared folder is read-only", "Local resources", K::Bool, "false"},
        {"compression", "Image compression", "Display", K::Choice, "auto",
         {{"auto", "Server default"}, {"auto-glz", "Auto GLZ"}, {"auto-lz", "Auto LZ"}, {"quic", "QUIC"}, {"glz", "GLZ"}, {"lz", "LZ"}, {"lz4", "LZ4"}, {"off", "Off"}}},
        {"videoCodec", "Preferred video codec", "Display", K::Choice, "auto",
         {{"auto", "Server default"}, {"mjpeg", "MJPEG"}, {"vp8", "VP8"}, {"vp9", "VP9"}, {"h264", "H.264"}, {"h265", "H.265"}}},
    };
    addDisplayCommon(p, true);
    addStartCommon(p, true);
    for (auto& t : tunnelCommon()) p.push_back(t);
    return p;
}

static void addTerminalCommon(std::vector<PropertyDef>& p)
{
    p.push_back({"term", "Terminal type", "Terminal", K::Text, "xterm-256color"});
    p.push_back({"font", "Font", "Terminal", K::Text, "", {}, 0, 0, "Empty = Settings → Font → SSH terminal"});
    p.push_back({"fontSize", "Font size (pt)", "Terminal", K::Int, "0", {}, 0, 72, "0 = Settings → Font → SSH terminal"});
    p.push_back({"scrollback", "Scrollback lines", "Terminal", K::Int, "10000", {}, 0, 1000000});
    std::vector<Choice> colors{{"", "As in Settings"}};
    for (const ColorScheme& s : colorSchemes) colors.push_back({s.key, s.label});
    p.push_back({"colors", "Colors", "Terminal", K::Choice, "", colors, 0, 0, "Settings → Font → SSH terminal"});
}

// /etc/shells, then well-known shells on PATH (fish, nu… are often not listed); one entry per path
static std::vector<Choice> shellChoices()
{
    std::vector<Choice> choices{{"", "Your login shell"}};
#ifndef _WIN32
    namespace fs = std::filesystem;
    if (const passwd* user = getpwuid(getuid()); user && user->pw_shell && *user->pw_shell)
        choices[0].label += std::string(" (") + user->pw_shell + ")";
    std::vector<std::string> candidates;
    std::ifstream shells("/etc/shells");
    for (std::string line; std::getline(shells, line);)
        if (!line.empty() && line[0] == '/') candidates.push_back(line);
    const char* path = std::getenv("PATH");
    std::stringstream dirs(path ? path : "");
    std::vector<std::string> pathDirs;
    for (std::string dir; std::getline(dirs, dir, ':');)
        if (!dir.empty()) pathDirs.push_back(dir);
    for (const char* name : {"zsh", "fish", "nu", "dash", "ksh", "mksh", "tcsh", "xonsh", "elvish", "pwsh"})
        for (const std::string& dir : pathDirs)
            if (access((dir + "/" + name).c_str(), X_OK) == 0) {
                candidates.push_back(dir + "/" + name);
                break;
            }
    std::set<std::string> seen;
    for (const std::string& shell : candidates) {
        const fs::path file(shell);
        const std::string name = file.filename().string();
        std::error_code ec;
        const fs::path dir = fs::canonical(file.parent_path(), ec); // /bin and /usr/bin are often one folder
        if (ec || name == "nologin" || name == "false" || name == "git-shell" || access(shell.c_str(), X_OK) != 0) continue;
        if (!seen.insert((dir / name).string()).second) continue;
        choices.push_back({shell, name + " (" + shell + ")"});
    }
#endif
    return choices;
}

static std::vector<PropertyDef> localProperties()
{
    std::vector<PropertyDef> p = {
        {"shell", "Shell", "General", K::Choice, "", shellChoices(), 0, 0, "Also runs the command below, if one is given"},
        {"command", "Run command instead of a shell", "General", K::Text, "", {}, 0, 0, "Empty = the shell, interactive"},
        {"workingDir", "Start in folder", "General", K::Directory, "", {}, 0, 0, "Empty = your home folder"},
    };
    addTerminalCommon(p);
    return p;
}

static std::vector<PropertyDef> telnetProperties()
{
    std::vector<PropertyDef> p = {
        {"raw", "Raw TCP, no telnet options", "General", K::Bool, "false", {}, 0, 0, "For console servers and devices that send plain bytes"},
    };
    addTerminalCommon(p);
    addStartCommon(p, true);
    return p;
}

static std::vector<PropertyDef> serialProperties()
{
    std::vector<PropertyDef> p = {
        {"baud", "Speed (baud)", "General", K::Choice, "115200",
         {{"1200", "1200"}, {"2400", "2400"}, {"4800", "4800"}, {"9600", "9600"}, {"19200", "19200"}, {"38400", "38400"},
          {"57600", "57600"}, {"115200", "115200"}, {"230400", "230400"}, {"460800", "460800"}, {"921600", "921600"}}},
        {"dataBits", "Data bits", "General", K::Choice, "8", {{"5", "5"}, {"6", "6"}, {"7", "7"}, {"8", "8"}}},
        {"parity", "Parity", "General", K::Choice, "none", {{"none", "None"}, {"even", "Even"}, {"odd", "Odd"}}},
        {"stopBits", "Stop bits", "General", K::Choice, "1", {{"1", "1"}, {"2", "2"}}},
        {"flowControl", "Flow control", "General", K::Choice, "none",
         {{"none", "None"}, {"hardware", "Hardware (RTS/CTS)"}, {"software", "Software (XON/XOFF)"}}},
    };
    addTerminalCommon(p);
    addStartCommon(p, false);
    return p;
}

static std::vector<PropertyDef> sshProperties()
{
    std::vector<PropertyDef> p = {
        {"keyFile", "Private key", "General", K::File, "", {}, 0, 0, "Empty = SSH agent and ~/.ssh default keys"},
        // an SSH server of Settings (its id); ~/.ssh/config's "[user@]host[:port][,...]" still works
        {"jumpHost", "Jump host", "General", K::Choice, "", {{"", "None: connect directly"}, {"group", "As the group"}}, 0, 0,
         "The connection goes through this SSH server (ProxyJump); servers are added in Settings → SSH Servers"},
        {"command", "Run command instead of a shell", "General", K::Text, ""},
        {"compression", "Compression", "Advanced", K::Bool, "false"},
        {"keepalive", "Keepalive interval (s)", "Advanced", K::Int, "0", {}, 0, 3600, "0 = off"},
        {"localForwards", "Local forwards (ssh -L)", "Forwarding", K::Text, "", {}, 0, 0,
         "[bind:]port:host:hostport, several separated by commas; e.g. 5432:db.internal:5432"},
        {"remoteForwards", "Remote forwards (ssh -R)", "Forwarding", K::Text, "", {}, 0, 0,
         "[bind:]port:host:hostport: the server's port leads to host:hostport as seen from here"},
        {"socksPort", "SOCKS proxy port (ssh -D)", "Forwarding", K::Int, "0", {}, 0, 65535, "0 = off; a SOCKS5 proxy on this computer"},
        {"forwardAgent", "Forward the SSH agent (ssh -A)", "Forwarding", K::Bool, "false", {}, 0, 0,
         "Programs on the server log in elsewhere with this computer's agent keys; only for servers you trust"},
        {"forwardX11", "X11 forwarding (ssh -X)", "Forwarding", K::Bool, "false", {}, 0, 0,
         "Graphical programs started on the server open their windows here; needs an X display here (XWayland, XQuartz) "
         "and xauth on the server"},
    };
    addTerminalCommon(p);
    addStartCommon(p, true);
    return p;
}

static std::vector<PropertyDef> androidProperties()
{
    std::vector<PropertyDef> p = {
        {"keyboard", "Keyboard", "General", K::Choice, "text",
         {{"text", "Text input (no setup; some characters such as ő cannot be typed)"},
          {"hid", "Hardware keyboard (every character, with the device's keyboard layout)"}},
         0, 0, "Hardware keyboard: choose its layout once on the device, under Settings > System > Keyboard > "
               "Physical keyboard > Portal Mux"},
        {"maxSize", "Maximum size (px)", "Display", K::Int, "0", {}, 0, 8192, "Longer side of the video; 0 = the device's resolution"},
        {"videoCodec", "Video codec", "Display", K::Choice, "h264", {{"h264", "H.264"}, {"h265", "H.265 (HEVC)"}, {"av1", "AV1"}}},
        {"videoBitRate", "Video bit rate (Mbit/s)", "Display", K::Int, "8", {}, 1, 200},
        {"maxFps", "Maximum frame rate", "Display", K::Int, "0", {}, 0, 240, "0 = as fast as the device sends"},
        {"hardwareDecoding", "Hardware video decoding", "Display", K::Bool, "true", {}, 0, 0, "On the GPU when it supports the codec; software otherwise"},
        {"audio", "Forward audio (Android 11 or newer)", "Audio", K::Bool, "true"},
        {"audioSource", "Source", "Audio", K::Choice, "output",
         {{"output", "Device sound output"}, {"playback", "App playback (device keeps playing)"}, {"mic", "Microphone"}}},
        {"audioCodec", "Codec", "Audio", K::Choice, "opus", {{"opus", "Opus"}, {"aac", "AAC"}, {"flac", "FLAC"}, {"raw", "Uncompressed"}}},
        {"audioBitRate", "Bit rate (kbit/s)", "Audio", K::Int, "128", {}, 8, 1024},
        {"turnScreenOff", "Turn the device's screen off while connected", "Device", K::Bool, "false"},
        {"stayAwake", "Keep the device awake while connected (on USB)", "Device", K::Bool, "false"},
        {"showTouches", "Show touches on the device", "Device", K::Bool, "false"},
        {"powerOffOnClose", "Turn the device's screen off when disconnecting", "Device", K::Bool, "false"},
        {"displayId", "Display id", "Advanced", K::Int, "0", {}, 0, 1000, "0 = main display"},
        {"adb", "adb executable", "Advanced", K::File, "", {}, 0, 0, "Empty = the Android SDK's, or adb on PATH"},
    };
    addDisplayCommon(p, false); // device sets the size
    addStartCommon(p, false);
    return p;
}

static std::vector<PropertyDef> moonlightProperties()
{
    std::vector<PropertyDef> p = {
        {"app", "App", "General", K::Text, "Desktop", {}, 0, 0, "Name of the app on the host (Sunshine/Apollo: Applications page)"},
        {"hardwareDecoding", "Hardware video decoding", "Display", K::Bool, "true", {}, 0, 0, "On the GPU when it supports the codec; software otherwise"},
        {"resolution", "Resolution", "Display", K::Choice, "1920x1080",
         {{"window", "Window size when connecting"}, {"1280x720", "1280 × 720"}, {"1920x1080", "1920 × 1080"},
          {"2560x1440", "2560 × 1440"}, {"3840x2160", "3840 × 2160"}}},
        {"virtualDisplay", "Virtual display of this size (Apollo)", "Display", K::Bool, "false", {}, 0, 0,
         "Apollo adds a screen of exactly this resolution for the stream; Sunshine ignores it"},
        {"fps", "Frame rate", "Display", K::Int, "60", {}, 10, 240},
        {"bitrate", "Video bit rate (Mbit/s)", "Display", K::Int, "20", {}, 1, 500},
        {"videoCodec", "Video codec", "Display", K::Choice, "h264",
         {{"h264", "H.264"}, {"h265", "H.265 (HEVC) if the host can"}, {"av1", "AV1 if the host can"}}},
        {"audio", "Play the host's sound here", "Audio", K::Bool, "true"},
        {"hostAudio", "Keep playing it on the host too", "Audio", K::Bool, "false"},
        {"quitAppOnClose", "Quit the app on the host when disconnecting", "Advanced", K::Bool, "false"},
    };
    addDisplayCommon(p, false); // size set at launch
    for (PropertyDef& d : p)
        if (d.key == "clipboard") d.tip = "Text, with Apollo hosts (GameStream itself has no clipboard)";
    addStartCommon(p, true);
    return p;
}

// a login of Settings → Logins (its id), "group" or "": the user name and password typed in the connection
static std::vector<PropertyDef> withLogin(std::vector<PropertyDef> p)
{
    p.insert(p.begin(), {"login", "Login", "General", K::Choice, "", {{"", "Typed here"}, {"group", "As the group"}}, 0, 0,
                         "A user name and password many connections share; logins are added in Settings → Logins"});
    return p;
}

const std::vector<PropertyDef>& properties(Protocol protocol)
{
    static const std::vector<PropertyDef> rdp = withLogin(rdpProperties()), vnc = withLogin(vncProperties()),
                                          spice = withLogin(spiceProperties()), ssh = withLogin(sshProperties()),
                                          android = androidProperties(),
                                          moonlight = moonlightProperties(), local = localProperties(),
                                          telnet = telnetProperties(), serial = serialProperties();
    switch (protocol) {
    case Protocol::RDP: return rdp;
    case Protocol::VNC: return vnc;
    case Protocol::SPICE: return spice;
    case Protocol::SSH: return ssh;
    case Protocol::Android: return android;
    case Protocol::Moonlight: return moonlight;
    case Protocol::Local: return local;
    case Protocol::Telnet: return telnet;
    case Protocol::Serial: return serial;
    }
    return rdp;
}

const PropertyDef* findProperty(Protocol protocol, std::string_view key)
{
    for (const PropertyDef& d : properties(protocol))
        if (d.key == key) return &d;
    return nullptr;
}

std::string Connection::option(std::string_view key) const
{
    if (auto it = options.find(std::string(key)); it != options.end()) return it->second;
    const PropertyDef* d = findProperty(protocol, key);
    return d ? d->defaultValue : std::string();
}

bool Connection::flag(std::string_view key) const { return option(key) == "true"; }

int Connection::number(std::string_view key) const { return std::atoi(option(key).c_str()); }

} // namespace rdc
