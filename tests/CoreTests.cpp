// Plain assert-based tests; no framework until there is enough to justify one.
#undef NDEBUG
#include "android/ScrcpyMessages.h"
#include "app/Addresses.h"
#include "app/Azure.h"
#include "app/DomainXml.h"
#include "app/HyperV.h"
#include "app/KrdcImport.h"
#include "app/Proxmox.h"
#include "core/Connection.h"
#include "core/Framebuffer.h"
#include "core/Properties.h"
#include "media/VideoDecoder.h"
#include "moonlight/GameStream.h"
#include "ssh/SshConfig.h"
#include "ssh/SshForwards.h"
#include "ssh/SshTunnel.h"
#include "stream/Telnet.h"

#include <winpr/input.h>

#include <QFile>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <vector>

using rdc::Framebuffer;
using rdc::Rect;

static void rectTests()
{
    assert((Rect{}.united({1, 2, 3, 4}) == Rect{1, 2, 3, 4}));
    assert((Rect{0, 0, 2, 2}.united({5, 5, 1, 1}) == Rect{0, 0, 6, 6}));
    assert((Rect{0, 0, 10, 10}.intersected({5, 5, 10, 10}) == Rect{5, 5, 5, 5}));
    assert((Rect{0, 0, 2, 2}.intersected({3, 3, 1, 1}).empty()));
}

static void framebufferTests()
{
    Framebuffer fb;
    int calls = 0;
    auto take = [&] {
        Rect got;
        fb.consume([&](const uint8_t*, int, int, int, Rect d) { got = d; ++calls; });
        return got;
    };

    fb.resize(8, 4);
    assert((take() == Rect{0, 0, 8, 4}));
    assert(take().empty() && calls == 1);

    // source wider than the framebuffer (stride != width * 4)
    const int srcStride = 16 * 4;
    std::vector<uint8_t> src(size_t(srcStride) * 4);
    for (size_t i = 0; i < src.size(); ++i) src[i] = uint8_t(i);

    fb.update({1, 1, 2, 1}, src.data(), srcStride);
    fb.update({6, 2, 10, 10}, src.data(), srcStride);
    fb.consume([&](const uint8_t* px, int w, int h, int stride, Rect d) {
        assert(w == 8 && h == 4 && stride == 32);
        assert((d == Rect{1, 1, 7, 3}));
        assert(px[1 * stride + 1 * 4] == src[1 * srcStride + 1 * 4]);
        assert(px[3 * stride + 7 * 4 + 2] == src[3 * srcStride + 7 * 4 + 2]);
        assert(px[3 * stride + 7 * 4 + 3] == 0xFF); // X byte forced opaque
        assert(px[0] == 0 && px[3] == 0xFF);
    });

    fb.update({100, 100, 5, 5}, src.data(), srcStride);
    assert(take().empty());
}

static void connectionTests()
{
    rdc::Connection c;
    c.protocol = rdc::Protocol::VNC;
    assert(c.effectivePort() == 5900);
    c.port = 1;
    assert(c.effectivePort() == 5901);
    c.port = 5912;
    assert(c.effectivePort() == 5912);
    c.protocol = rdc::Protocol::RDP;
    c.port = 0;
    assert(c.effectivePort() == 3389);

    auto p = rdc::SshTunnel::parse("alice@jump.example:2222");
    assert(p.user == "alice" && p.host == "jump.example" && p.port == 2222);
    p = rdc::SshTunnel::parse("jump.example");
    assert(p.user.empty() && p.host == "jump.example" && p.port == 22);
    p = rdc::SshTunnel::parse("::1"); // bare IPv6 address: no port split
    assert(p.host == "::1" && p.port == 22);
}

static void propertyTests()
{
    using rdc::PropertyKind;
    for (auto protocol : {rdc::Protocol::RDP, rdc::Protocol::VNC, rdc::Protocol::SPICE, rdc::Protocol::SSH, rdc::Protocol::Android,
                          rdc::Protocol::Moonlight, rdc::Protocol::Local, rdc::Protocol::Telnet, rdc::Protocol::Serial}) {
        std::set<std::string> keys;
        for (const rdc::PropertyDef& d : rdc::properties(protocol)) {
            assert(keys.insert(d.key).second);
            assert(!d.label.empty() && !d.tab.empty());
            if (d.kind == PropertyKind::Bool) assert(d.defaultValue == "true" || d.defaultValue == "false");
            if (d.kind == PropertyKind::Int) {
                const int v = std::atoi(d.defaultValue.c_str());
                assert(d.min <= v && v <= d.max);
            }
            if (d.kind == PropertyKind::Choice) {
                bool found = false;
                for (const rdc::Choice& c : d.choices) found = found || c.value == d.defaultValue;
                assert(found);
            }
        }
    }
    const rdc::PropertyDef* shell = rdc::findProperty(rdc::Protocol::Local, "shell");
    assert(shell && shell->choices.front().value.empty()); // the login shell first, then existing absolute paths
    for (size_t i = 1; i < shell->choices.size(); ++i)
        assert(shell->choices[i].value.starts_with('/') && std::filesystem::exists(shell->choices[i].value));
    rdc::Connection c;
    c.protocol = rdc::Protocol::VNC;
    assert(c.option("jpegQuality") == "8" && c.flag("clipboard") && !c.flag("viewOnly"));
    c.options["jpegQuality"] = "3";
    assert(c.number("jpegQuality") == 3);
    assert(c.option("no-such-key").empty());
}

static void krdcImportTests()
{
    const QByteArray xbel = R"(<?xml version="1.0" encoding="UTF-8"?>
<xbel>
 <folder><title>History</title>
  <info><metadata owner="http://www.kde.org"><krdc-history>historyfolder</krdc-history></metadata></info>
  <bookmark href="rdp://Admin@10.0.0.5"><title>rdp://Admin@10.0.0.5</title></bookmark>
  <bookmark href="vnc://me@mac.local:5900"><title>vnc://me@mac.local:5900</title></bookmark>
  <bookmark href="rdp://sys@cloud.example?domain=AzureAD"><title>Cloud</title></bookmark>
 </folder>
 <folder><title>Lab</title>
  <bookmark href="rdp://admin@10.0.0.5"><title>Build</title></bookmark>
 </folder>
 <bookmark href="vnc://me@10.0.0.9:5901"><title>Mac</title></bookmark>
</xbel>)";
    const QByteArray rc = "[hostpreferences][rdp://admin@10.0.0.5]\nresolution=3\nacceleration=3\nshareMedia=/home/me/share\n"
                          "securityNLA=false\nsecurityTLS=true\nsecurityRDP=true\nsound=2\n"
                          "[hostpreferences][vnc://mac.local:5900]\nquality=3\nwindowedScale=true\n"
                          "[hostpreferences][rdp://sys@cloud.example?domain=AzureAD]\nresolution=1\nscaleToSize=true\n"
                          "use_ssh_tunnel=true\nssh_tunnel_user_name=jo\nssh_tunnel_port=2222\nuse_ssh_tunnel_loopback=true\n";
    const std::vector<rdc::Connection> list = importKrdc(xbel, rc);
    assert(list.size() == 4); // Admin@ history entry is a duplicate of the bookmark
    const rdc::Connection &build = list[0], &mac = list[1], &mac2 = list[2], &cloud = list[3];
    assert(build.name == "Build" && build.group == "Lab" && build.username == "admin" && build.host == "10.0.0.5");
    assert(build.option("scaling") == "scale" && !build.flag("gfx") && build.option("drives") == "/home/me/share");
    assert(build.option("security") == "tls" && build.option("audio") == "off");
    assert(mac.name == "Mac" && mac.group.empty() && mac.protocol == rdc::Protocol::VNC && mac.port == 5901);
    assert(mac2.name == "mac.local" && mac2.group == "KRDC history" && mac2.port == 0);
    assert(mac2.option("scaling") == "scale" && mac2.number("jpegQuality") == 1 && mac2.number("compression") == 9);
    assert(cloud.name == "Cloud" && cloud.option("domain") == "AzureAD" && cloud.option("scaling") == "scale");
    assert(cloud.number("width") == 1600 && cloud.number("height") == 900);
    assert(cloud.option("tunnel") == "jo@cloud.example:2222" && cloud.host == "localhost");
}

static void sshConfigTests()
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "portal-mux-sshconfig-test";
    fs::remove_all(dir);
    fs::create_directories(dir / "config.d");
    std::ofstream(dir / "key") << "x";
    std::ofstream(dir / "config") << "Compression yes\n"
                                     "Host web db # two aliases\n"
                                     "  HostName %h.example.com\n"
                                     "  User=alice\n"
                                     "Host db\n"
                                     "  User bob\n" // ignored: first value wins
                                     "  Port 2200\n"
                                     "  ProxyJump jump@gw:2222\n"
                                     "Match host web\n"
                                     "  Port 1\n" // Match blocks are skipped
                                     "Include config.d/*\n"
                                     "Host * !db\n"
                                     "  IdentityFile \"" + (dir / "key").string() + "\"\n"
                                     "  ServerAliveInterval 30\n";
    std::ofstream(dir / "config.d" / "lab") << "Host lab\n  HostName 10.0.0.7\n  IdentityFile /no/such/key\n";
    const std::vector<rdc::Connection> hosts = rdc::readSshConfig((dir / "config").string());
    fs::remove_all(dir);
    assert(hosts.size() == 3);
    const rdc::Connection &web = hosts[0], &db = hosts[1], &lab = hosts[2];
    assert(web.id == "ssh-config:web" && web.name == "web" && web.protocol == rdc::Protocol::SSH);
    assert(web.host == "web.example.com" && web.username == "alice" && web.port == 0 && web.flag("compression"));
    assert(web.option("keyFile") == (dir / "key").string() && web.number("keepalive") == 30);
    assert(db.host == "db.example.com" && db.username == "alice" && db.port == 2200 && db.option("jumpHost") == "jump@gw:2222");
    assert(db.option("keyFile").empty() && db.number("keepalive") == 0); // "!db" excludes it
    assert(lab.host == "10.0.0.7" && lab.username.empty() && lab.option("keyFile") == web.option("keyFile"));
    assert(rdc::readSshConfig((dir / "missing").string()).empty());
}

static void domainXmlTests()
{
    auto domain = [](const char* devices) {
        return QStringLiteral("<domain type='kvm' id='1'><name>Win 10</name><uuid>d074d338-31c5</uuid>"
                              "<metadata><app:x xmlns:app='urn:x'><app:name>not this</app:name></app:x></metadata>"
                              "<devices><disk><name>nor this</name></disk>%1</devices></domain>")
            .arg(QLatin1String(devices));
    };
    auto spice = connectionFromDomainXml(domain("<graphics type='spice' port='5900' autoport='yes' listen='127.0.0.1' passwd='pw'>"
                                                "<listen type='address' address='127.0.0.1'/><image compression='off'/></graphics>"));
    assert(spice && spice->protocol == rdc::Protocol::SPICE && spice->name == "Win 10" && spice->id == "libvirt:d074d338-31c5");
    assert(spice->host == "127.0.0.1" && spice->port == 5900 && spice->password == "pw" && spice->number("tlsPort") == 0);
    auto vnc = connectionFromDomainXml(domain("<graphics type='spice'><listen type='socket' socket='/run/x.sock'/></graphics>"
                                              "<graphics type='vnc' port='5901' listen='0.0.0.0'/>"));
    assert(vnc && vnc->protocol == rdc::Protocol::VNC && vnc->host == "127.0.0.1" && vnc->port == 5901 && vnc->password.empty());
    auto tls = connectionFromDomainXml(domain("<graphics type='spice' port='-1' tlsPort='5902'><listen type='address' address='192.168.1.5'/></graphics>"));
    assert(tls && tls->port == 0 && tls->number("tlsPort") == 5902 && tls->host == "192.168.1.5");
    assert(!connectionFromDomainXml(domain("<graphics type='spice' port='-1'><listen type='none'/></graphics>")));
    assert(!connectionFromDomainXml(domain("<graphics type='egl-headless'/><video/>")));
    assert(!connectionFromDomainXml(domain("")));

    const QString perDevice = QStringLiteral(
        "<domain type='kvm'><vcpu placement='static' cpuset='0-7'>8</vcpu><memory unit='KiB'>4194304</memory>"
        "<currentMemory unit='KiB'>4194304</currentMemory><os><type>hvm</type></os>"
        "<cpu mode='host-passthrough'><topology sockets='1' dies='1' clusters='1' cores='4' threads='2'/></cpu><devices>"
        "<disk type='file' device='disk'><source file='/v/a.qcow2'/><target dev='vda' bus='virtio'/><boot order='1'/></disk>"
        "<disk type='file' device='cdrom'><driver name='qemu'/><source file='/iso/old.iso'/><target dev='sda' bus='sata'/><readonly/></disk>"
        "<interface type='network'><boot order='2'/></interface><graphics type='spice' passwd='pw'/></devices></domain>");
    auto flat = [](QString xml) { return xml.replace(QRegularExpression(QStringLiteral(">\\s+<")), QStringLiteral("><")); };
    VmSettings s = domainSettings(perDevice);
    assert(s.cpus == 8 && s.memoryMiB == 4096 && !s.cpusFixed && !s.memoryFixed && s.hasCd && s.iso == "/iso/old.iso" && !s.cdFirst);
    s.cpus = 6;
    s.memoryMiB = 8192;
    s.cdFirst = true;
    QString edited = flat(withSettings(perDevice, s));
    VmSettings back = domainSettings(edited);
    assert(back.cpus == 6 && back.memoryMiB == 8192 && back.cdFirst && back.iso == "/iso/old.iso");
    assert(edited.contains("cores=\"3\"") && edited.contains("threads=\"2\"") && edited.contains("cpuset=\"0-7\"") && edited.contains("passwd=\"pw\""));
    assert(edited.contains("<currentMemory unit=\"KiB\">8388608</currentMemory>"));
    assert(edited.contains("<readonly/><boot order=\"1\"/></disk>") && edited.contains("<boot order=\"3\"/></interface>"));
    s.cpus = 5; // no longer divisible by its 2 threads
    s.cdFirst = false;
    edited = flat(withSettings(edited, s));
    assert(edited.contains("cores=\"5\"") && edited.contains("threads=\"1\"") && !domainSettings(edited).cdFirst);
    assert(edited.contains("<readonly/><boot order=\"3\"/></disk>") && edited.contains("<boot order=\"2\"/></interface>"));

    const QString legacy = QStringLiteral("<domain><vcpu current='2'>4</vcpu><memory>1048576</memory><os><type>hvm</type><boot dev='hd'/></os>"
                                          "<devices><disk device='cdrom' type='file'><target dev='hdc'/></disk></devices></domain>");
    s = domainSettings(legacy);
    assert(s.cpus == 2 && s.hasCd && s.iso.isEmpty() && !s.cdFirst);
    s.cdFirst = true;
    edited = flat(withSettings(legacy, s));
    assert(edited.contains("<boot dev=\"cdrom\"/><boot dev=\"hd\"/></os>") && edited.contains("<vcpu current=\"2\">4</vcpu>"));
    assert(flat(cdromXml(legacy, "/iso/new.iso")).trimmed() == "<disk device=\"cdrom\" type=\"file\"><source file=\"/iso/new.iso\"/><target dev=\"hdc\"/></disk>");
    assert(!cdromXml(perDevice, {}).contains("source") && cdromXml(perDevice, {}).contains("dev=\"sda\""));
    const VmSettings numa = domainSettings("<domain><vcpu>2</vcpu><memory>2097152</memory><cpu><numa><cell id='0'/></numa></cpu></domain>");
    assert(numa.memoryFixed && !numa.hasCd && !numa.cdFirst && withSettings("<broken", s).isEmpty());

    const VmSnapshot running = snapshotFromXml("<domainsnapshot><name>before update</name><description>x</description><state>running</state>"
                                               "<creationTime>1763640000</creationTime><memory snapshot='internal'/><domain><name>vm</name></domain></domainsnapshot>");
    assert(running.name == "before update" && running.description == "x" && running.memory && running.created.toSecsSinceEpoch() == 1763640000);
    assert(!snapshotFromXml("<domainsnapshot><name>a</name><state>shutoff</state><memory snapshot='no'/></domainsnapshot>").memory);
    assert(!snapshotFromXml("<domainsnapshot><name>b</name><state>running</state><memory snapshot='no'/></domainsnapshot>").memory);
}

static void videoDecoderTests()
{
    std::ifstream in(TEST_DATA_DIR "/red-64x48.h264", std::ios::binary);
    const std::vector<uint8_t> h264((std::istreambuf_iterator<char>(in)), {});
    assert(!h264.empty());
    rdc::VideoDecoder decoder;
    std::string error;
    assert(decoder.open(rdc::VideoDecoder::Codec::H264, false, &error));
    rdc::Framebuffer fb;
    assert(decoder.decode(h264.data(), h264.size(), 0, fb) == 1);
    assert(decoder.width() == 64 && decoder.height() == 48);
    bool seen = false;
    fb.consume([&](const uint8_t* px, int w, int h, int, Rect) {
        seen = w == 64 && h == 48;
        assert(px[0] < 16 && px[1] < 16 && px[2] > 235 && px[3] == 0xFF); // B, G, R, X of pure red
    });
    assert(seen);
    assert(decoder.decode(h264.data(), 3, 1, fb) == 0); // garbage is skipped, not fatal

    rdc::VideoDecoder gpu; // the same picture whether the GPU takes it or software does
    assert(gpu.open(rdc::VideoDecoder::Codec::H264, true, &error));
    rdc::Framebuffer fb2;
    assert(gpu.decode(h264.data(), h264.size(), 0, fb2) == 1);
    fb2.consume([&](const uint8_t* px, int w, int h, int, Rect) { assert(w == 64 && h == 48 && px[2] > 235 && px[1] < 16); });
    std::printf("video: GPU decoding %s\n", gpu.hardwareFrames() ? "used" : "not used (software)");
}

static void scrcpyMessageTests()
{
    namespace s = rdc::scrcpy;
    const s::Bytes key = s::keycode(s::KeyDown, 29, s::MetaShift);
    assert(key.size() == 14 && key[0] == 0 && key[1] == 0 && key[5] == 29 && key[13] == 0x41);
    const s::Bytes t = s::touch(s::TouchDown, 0x0102, 0x0304, 1080, 2400, true, s::ButtonPrimary, s::ButtonPrimary);
    assert(t.size() == 32 && t[0] == 2 && t[1] == 0);
    assert(t[2] == 0xFF && t[9] == 0xFF);
    assert(t[12] == 0x01 && t[13] == 0x02 && t[16] == 0x03 && t[17] == 0x04);
    assert(t[18] == (1080 >> 8) && t[19] == (1080 & 0xFF) && t[20] == (2400 >> 8));
    assert(t[22] == 0xFF && t[23] == 0xFF && t[27] == 1 && t[31] == 1);
    const s::Bytes sc = s::scroll(1, 2, 3, 4, 0, 1.0f, 0);
    assert(sc.size() == 21 && sc[0] == 3 && sc[15] == 0x08 && sc[16] == 0x00); // 1 notch = 1/16 -> 0x0800
    assert(s::scroll(0, 0, 1, 1, 0, -40.0f, 0)[15] == 0x80);
    const s::Bytes clip = s::setClipboard(7, "hi");
    assert(clip.size() == 1 + 8 + 1 + 4 + 2 && clip[0] == 9 && clip[8] == 7 && clip[9] == 0 && clip[13] == 2);
    const s::Bytes text = s::text(std::string(299, 'a') + "\xc3\xa9"); // 'é' straddles the 300-byte limit
    assert(text.size() == 1 + 4 + 299);
    assert(s::androidKeycode('q') == 45 && s::androidKeycode('Q') == 45 && s::androidKeycode(0xFF0D) == 66);
    assert(s::androidKeycode('1') == 0 && s::androidKeycode(0xFE03) == 0); // digits and AltGr: sent as text

    // UHID_CREATE: id, vendor, product, name, descriptor
    const s::Bytes create = s::uhidCreate(1, "Portal Mux", s::HidKeyboardDescriptor, sizeof s::HidKeyboardDescriptor);
    assert(create.size() == 1 + 6 + 1 + 10 + 2 + sizeof s::HidKeyboardDescriptor && create[0] == 12 && create[2] == 1);
    assert(create[7] == 10 && create[18] == 0 && create[19] == sizeof s::HidKeyboardDescriptor && create.back() == 0xC0);
    const uint8_t report[8] = {0x02, 0, 0x04};
    const s::Bytes input = s::uhidInput(1, report, 8);
    assert(input.size() == 13 && input[0] == 13 && input[4] == 8 && input[5] == 0x02 && input[7] == 0x04);
    assert(s::hidUsage(0x1E) == 0x04 && s::hidUsage(0x10) == 0x14 && s::hidUsage(0x0B) == 0x27);
    assert(s::hidUsage(0x56) == 0x64 && s::hidUsage(0x138) == 0xE6 && s::hidUsage(0x14B) == 0x50);
    assert(s::hidUsage(0x54) == 0 && s::hidUsage(0x200) == 0);
}

static void gameStreamTests()
{
    namespace g = rdc::gamestream;
    const std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<root status_code="200"><hostname>WIN10-MI</hostname><appversion>7.1.431.-1</appversion><HttpsPort>47984</HttpsPort>
<PairStatus>1</PairStatus><currentgame>0</currentgame><App><AppTitle>Desktop &amp; more</AppTitle><ID>881448767</ID></App></root>)";
    std::string message;
    assert(g::xmlStatus(xml, &message) == 200 && message.empty());
    assert(g::xmlValue(xml, "hostname") == "WIN10-MI" && g::xmlValue(xml, "AppTitle") == "Desktop & more");
    assert(g::xmlValue(xml, "missing").empty());
    assert(g::xmlStatus(R"(<root status_code="401" status_message="The client is not authorized">)", &message) == 401 &&
           message == "The client is not authorized");
    assert(g::xmlStatus("<html>", &message) == -1);
    assert(g::toHex(std::string("\x00\xab", 2)) == "00ab");

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "portal-mux-identity-test";
    fs::remove_all(dir);
    g::Identity first, second;
    std::string error;
    assert(g::loadIdentity(dir.string(), first, &error) && first.uniqueId.size() == 16);
    assert(first.certPem.starts_with("-----BEGIN CERTIFICATE-----") && first.keyPem.find("PRIVATE KEY") != std::string::npos);
#ifndef _WIN32 // no group bits on Windows
    assert((fs::status(dir / "key.pem").permissions() & fs::perms::group_read) == fs::perms::none);
#endif
    assert(g::loadIdentity(dir.string(), second, &error) && second.certPem == first.certPem && second.uniqueId == first.uniqueId);
    fs::remove_all(dir);

    // US-layout VK codes; modifiers as left/right codes
    assert((GetVirtualKeyCodeFromVirtualScanCode(0x1E, WINPR_KBD_TYPE_IBM_ENHANCED) & 0xFF) == 'A');
    assert((GetVirtualKeyCodeFromVirtualScanCode(0x2A, WINPR_KBD_TYPE_IBM_ENHANCED) & 0xFF) == VK_LSHIFT);
    assert((GetVirtualKeyCodeFromVirtualScanCode(0x138, WINPR_KBD_TYPE_IBM_ENHANCED) & 0xFF) == VK_RMENU);
}

static void titleFolderTests()
{
    const QString home = QStringLiteral("/home/al");
    assert(titleFolder(QStringLiteral("al@box: ~/src/my dir"), home) == QStringLiteral("/home/al/src/my dir")); // Debian
    assert(titleFolder(QStringLiteral("al@box:~"), home) == home);                                              // Fedora
    assert(titleFolder(QStringLiteral("al@box:/etc"), home) == QStringLiteral("/etc"));
    for (const char* none : {"vim notes.txt", "al@box: ~bob/x", "box: /etc", "al@box: relative/dir"})
        assert(titleFolder(QLatin1String(none), home).isEmpty());
    assert(titleFolder(QStringLiteral("al@box: ~/x"), {}).isEmpty());
}

static void addressTests()
{
    using rdc::Protocol;
    QString error;
    auto c = connectionFromAddress(QStringLiteral("myhost"), Protocol::VNC, &error);
    assert(c && c->protocol == Protocol::VNC && c->host == "myhost" && c->port == 0 && c->name == "myhost");
    c = connectionFromAddress(QStringLiteral(" rdp://bob:pw@win.example:3390/ "), Protocol::SSH, &error);
    assert(c && c->protocol == Protocol::RDP && c->host == "win.example" && c->port == 3390 && c->username == "bob" &&
           c->password == "pw" && c->name == "bob@win.example:3390");
    c = connectionFromAddress(QStringLiteral("SSH://[::1]:2222"), Protocol::RDP, &error);
    assert(c && c->protocol == Protocol::SSH && c->host == "::1" && c->port == 2222 && c->name == "[::1]:2222");
    c = connectionFromAddress(QStringLiteral("al@box:1"), Protocol::VNC, &error);
    assert(c && c->username == "al" && c->port == 1 && c->effectivePort() == 5901);
    for (const char* bad : {"", "http://x", "rdp://", "my host", "rdp://h:99999", "rdp://h/path"}) {
        error.clear();
        assert(!connectionFromAddress(QString::fromLatin1(bad), Protocol::RDP, &error) && !error.isEmpty());
    }

    QTemporaryDir dir;
    auto write = [&dir](const char* name, const QByteArray& data) {
        QFile f(dir.filePath(QLatin1String(name)));
        assert(f.open(QIODevice::WriteOnly) && f.write(data) == data.size());
        return f.fileName();
    };
    const QString rdp = QStringLiteral("screen mode id:i:2\r\nfull address:s:win.example:3391\r\nusername:s:CORP\\bob\r\n"
                                       "gatewayhostname:s:gw.example\r\ngatewayusagemethod:i:1\r\naudiomode:i:2\r\n"
                                       "redirectclipboard:i:0\r\nloadbalanceinfo:s:tsv://MS Terminal Services Plugin.1.Desk\r\nuse multimon:i:1\r\n");
    const QByteArray utf16 = QByteArray("\xFF\xFE", 2) + QByteArray(reinterpret_cast<const char*>(rdp.utf16()), rdp.size() * 2);
    c = connectionFromFile(write("Office PC.rdp", utf16), &error);
    assert(c && c->protocol == Protocol::RDP && c->name == "Office PC" && c->host == "win.example" && c->port == 3391);
    assert(c->username == "bob" && c->option("domain") == "CORP" && c->option("gateway.host") == "gw.example");
    assert(c->option("gateway.usage") == "direct" && c->option("audio") == "off" && !c->flag("clipboard"));
    assert(c->option("loadBalanceInfo") == "tsv://MS Terminal Services Plugin.1.Desk" && c->flag("multimon"));
    c = connectionFromFile(write("v6.rdp", "full address:s:[fe80::1]\nserver port:i:3390\ngatewayhostname:s:gw\ngatewayusagemethod:i:0\n"), &error);
    assert(c && c->host == "fe80::1" && c->port == 3390 && c->option("gateway.host").empty() && c->option("gateway.usage") == "direct");
    c = connectionFromFile(write("real.vnc", "[Connection]\nHost=box:1\nUserName=al\n"), &error);
    assert(c && c->protocol == Protocol::VNC && c->host == "box" && c->effectivePort() == 5901 && c->username == "al");
    c = connectionFromFile(write("tiger.tigervnc", "TigerVNC Configuration file Version 1.0\r\nServerName=box::5999\r\n"), &error);
    assert(c && c->host == "box" && c->effectivePort() == 5999);
    assert(!connectionFromFile(write("empty.rdp", "username:s:x\n"), &error) && error.contains(QLatin1String("full address")));
    assert(!connectionFromFile(write("notes.txt", "Host=box\n"), &error));
    assert(!connectionFromFile(dir.filePath(QStringLiteral("missing.rdp")), &error));

    const QByteArray packet = magicPacket(QStringLiteral("00:11:22:33:44:5f"));
    assert(packet.size() == 102 && packet.startsWith(QByteArray(6, char(0xFF))) && packet.endsWith(QByteArray("\x00\x11\x22\x33\x44\x5f", 6)));
    assert(magicPacket(QStringLiteral("00-11-22-33-44-5F")) == packet && magicPacket(QStringLiteral("0011223344 5f")).isEmpty());
    assert(magicPacket(QStringLiteral("001122334455")).size() == 102 && magicPacket(QStringLiteral("0011223344")).isEmpty());
    assert(magicPacket(QStringLiteral("zz:11:22:33:44:55")).isEmpty());
}

static void forwardTests()
{
    std::string error;
    auto f = rdc::parseForwards('L', "8080:localhost:80, 0.0.0.0:5432:db.internal:5432\n[::1]:2222:[fe80::1]:22", &error);
    assert(error.empty() && f.size() == 3);
    assert(f[0].kind == 'L' && f[0].bind.empty() && f[0].port == 8080 && f[0].host == "localhost" && f[0].hostPort == 80);
    assert(f[1].bind == "0.0.0.0" && f[1].port == 5432 && f[1].host == "db.internal");
    assert(f[2].bind == "::1" && f[2].port == 2222 && f[2].host == "fe80::1" && f[2].hostPort == 22);
    f = rdc::parseForwards('R', "9000:localhost:3000,8080:host,99999:h:1,x:h:1,1:h:0", &error);
    assert(f.size() == 1 && f[0].kind == 'R' && f[0].port == 9000 && error.find("8080:host") != std::string::npos);
    error.clear();
    assert(rdc::parseForwards('L', "  ", &error).empty() && error.empty());
}

static void telnetTests()
{
    namespace t = rdc::telnet;
    using S = std::string;
    auto cmd = [](unsigned char verb, unsigned char option) { return S{char(t::IAC), char(verb), char(option)}; };
    t::Protocol p("xterm-256color");
    assert(p.resize(80, 24).empty()); // NAWS not agreed yet
    assert(p.start() == cmd(t::WILL, t::WindowSize) + cmd(t::WILL, t::TerminalType) + cmd(t::DO, t::SuppressGoAhead));
    S text;
    const S naws = S{char(t::IAC), char(t::SB), char(t::WindowSize), 0, 80, 0, 24, char(t::IAC), char(t::SE)};
    assert(p.receive(cmd(t::DO, t::WindowSize), text) == naws); // the answer to our offer: no WILL again, the size
    assert(p.receive(cmd(t::DO, t::TerminalType) + cmd(t::WILL, t::SuppressGoAhead), text).empty());
    const S send = S{char(t::IAC), char(t::SB), char(t::TerminalType), 1, char(t::IAC), char(t::SE)};
    assert((p.receive(send, text) == S{char(t::IAC), char(t::SB), char(t::TerminalType), 0} + "xterm-256color" + S{char(t::IAC), char(t::SE)}));
    assert(p.receive(cmd(t::WILL, t::Echo), text) == cmd(t::DO, t::Echo));
    assert(p.receive(cmd(t::WILL, t::Echo), text).empty()); // no loop
    assert(p.receive(cmd(t::DO, 34), text) == cmd(t::WONT, 34) && p.receive(cmd(t::WILL, 5), text) == cmd(t::DONT, 5));
    assert(text.empty());
    // width 255 has an IAC byte: doubled
    assert((p.resize(255, 50) == S{char(t::IAC), char(t::SB), char(t::WindowSize), 0, char(255), char(255), 0, 50, char(t::IAC), char(t::SE)}));
    assert(p.receive(S("a\xff\xff" "b\r", 5) + S(1, '\0') + "c\r\n" + S{char(t::IAC), char(241)} + "d", text).empty());
    assert(text == S("a\xff" "b\rc\r\nd"));
    text.clear();
    assert(p.receive(S(1, char(t::IAC)), text).empty() && p.receive(S{char(t::DO), char(t::Binary)}, text) == cmd(t::WILL, t::Binary)); // split
    assert(p.encode("\r") == S("\r") && text.empty()); // binary: CR alone
    t::Protocol q("vt100");
    q.start();
    assert(q.encode(S("x\xffy\r", 4)) == S("x\xff\xffy\r", 5) + S(1, '\0') && q.encode("a\r\nb") == "a\r\nb");
    assert(q.receive(cmd(t::DONT, t::WindowSize), text).empty() && q.receive(cmd(t::DO, t::WindowSize), text).size() == 3 + 9);
    assert(q.receive(cmd(t::DONT, t::WindowSize), text) == cmd(t::WONT, t::WindowSize)); // was on: acknowledged
}

static void proxmoxTests()
{
    std::string host;
    int port = 0;
    assert(proxmox::parseAddress(QStringLiteral(" pve.lan "), host, port) && host == "pve.lan" && port == 8006);
    assert(proxmox::parseAddress(QStringLiteral("10.0.0.5:443"), host, port) && host == "10.0.0.5" && port == 443);
    assert(proxmox::parseAddress(QStringLiteral("[fd00::5]:8007"), host, port) && host == "fd00::5" && port == 8007);
    assert(proxmox::parseAddress(QStringLiteral("[fd00::5]"), host, port) && host == "fd00::5" && port == 8006);
    for (const char* bad : {"", "pve:0", "pve:99999", "[fd00::5", ":8006"}) assert(!proxmox::parseAddress(QString::fromLatin1(bad), host, port));
    assert(proxmox::isApiToken("root@pam!portalmux") && !proxmox::isApiToken("root@pam"));

    QString error;
    auto r = proxmox::parseResources(R"({"data":[
        {"type":"qemu","node":"pve2","vmid":101,"name":"win","status":"running","cpu":0.25,"mem":1073741824,"maxmem":4294967296},
        {"type":"node","node":"pve2","status":"online","cpu":0.07,"maxcpu":16,"mem":2147483648,"maxmem":34359738368,"uptime":90061},{"type":"storage","node":"pve1","storage":"local"},
        {"type":"lxc","node":"pve1","vmid":200,"status":"stopped"},{"type":"node","node":"pve1","status":"offline"},
        {"type":"qemu","node":"pve1","vmid":9000,"name":"tpl","status":"stopped","template":1},
        {"type":"qemu","node":"pve1","vmid":100,"name":"db","status":"stopped"}]})", &error);
    assert(r && r->nodes.size() == 2 && r->nodes[0].name == "pve1" && !r->nodes[0].online && r->nodes[1].online);
    assert(r->nodes[1].cpu == 0.07 && r->nodes[1].maxcpu == 16 && r->nodes[1].mem == 2147483648LL && r->nodes[1].maxmem == 34359738368LL &&
           r->nodes[1].uptime == 90061 && r->nodes[0].maxmem == 0);
    assert(r->guests.size() == 4 && r->guests[0].vmid == 100 && r->guests[1].vmid == 200 && r->guests[2].isTemplate);
    assert(r->guests[1].type == "lxc" && r->guests[1].name == "200" && r->guests[3].key() == "pve2/qemu/101");
    assert(r->guests[3].mem == 1073741824 && r->guests[3].maxmem == 4294967296LL && r->guests[3].cpu == 0.25);
    assert(!proxmox::parseResources("<html>", &error) && !error.isEmpty());
    error.clear();
    assert(!proxmox::parseResources(R"({"data":null})", &error) && !error.isEmpty());

    // As Proxmox sends it: the CA's line breaks as "\n" in the string.
    const QByteArray spice = R"({"data":{"type":"spice","host":"pvespiceproxy:65f1:101:pve2::ab12","password":"t1ck3t",
        "proxy":"http://pve.lan:3128","tls-port":61000,"host-subject":"OU=PVE Cluster Node,O=Proxmox Virtual Environment,CN=pve2",
        "ca":"-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n"}})";
    assert(proxmox::spiceCa(spice) == "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n");
    auto c = proxmox::spiceConnection(spice, "win", "/tmp/ca.pem", &error);
    assert(c && c->protocol == rdc::Protocol::SPICE && c->name == "win" && c->host == "pvespiceproxy:65f1:101:pve2::ab12");
    assert(c->port == 0 && c->password == "t1ck3t" && c->option("tlsPort") == "61000" && c->option("proxy") == "http://pve.lan:3128");
    assert(c->option("hostSubject") == "OU=PVE Cluster Node,O=Proxmox Virtual Environment,CN=pve2" && c->option("caFile") == "/tmp/ca.pem");
    c = proxmox::spiceConnection(R"({"data":{"host":"h","port":5900,"tls-port":"0"}})", "x", "/tmp/ca.pem", &error);
    assert(c && c->port == 5900 && c->number("tlsPort") == 0 && c->option("caFile").empty());
    error.clear();
    assert(!proxmox::spiceConnection(R"({"data":null})", "x", {}, &error) && !error.isEmpty());

    assert(proxmox::errorText(R"({"data":null,"message":"Parameter verification failed.\n","errors":{"vmid":"invalid"}})") ==
           "Parameter verification failed.; vmid: invalid");
    assert(proxmox::errorText("").isEmpty());

    auto vnc = proxmox::parseVncProxy(R"({"data":{"port":"5901","ticket":"PVEVNC:6738A1B2::c2ln","user":"root@pam","cert":"x"}})", &error);
    assert(vnc && vnc->port == 5901 && vnc->ticket == "PVEVNC:6738A1B2::c2ln" && vnc->password == vnc->ticket && vnc->user == "root@pam");
    vnc = proxmox::parseVncProxy(R"({"data":{"port":5902,"ticket":"t","password":"gen12345"}})", &error);
    assert(vnc && vnc->port == 5902 && vnc->password == "gen12345");
    error.clear();
    assert(!proxmox::parseVncProxy(R"({"data":{"port":0}})", &error) && !error.isEmpty());
    assert(proxmox::termInput("l\xc3\xa9s\r") == "0:5:l\xc3\xa9s\r" && proxmox::termInput("") == "0:0:");
    assert(proxmox::termResize(132, 43) == "1:132:43:");
    assert(proxmox::usesSpice(R"({"data":{"status":"running","spice":1}})") && proxmox::usesSpice(R"({"data":{"spice":true}})"));
    assert(!proxmox::usesSpice(R"({"data":{"status":"running"}})") && !proxmox::usesSpice("x"));
}

static void hypervTests()
{
    using hyperv::Action;
    using hyperv::State;
    QString error;
    const auto l = hyperv::parseListing(R"({"vms":[{"id":"8d1c0e2a-0b5e-4c4b-9a55-0123456789ab","name":"Xubuntu","state":"Running","cpu":12,
        "memory":2147483648,"uptime":75,"generation":2,"status":"Operating normally","ip":"192.168.122.80"},
        {"id":"9d1c0e2a-0b5e-4c4b-9a55-0123456789ab","name":"W","state":"FastSavedCritical","cpu":0,"memory":0,"uptime":0,
        "generation":1,"status":"","ip":""}],"errors":["'W' failed to start."]})", &error);
    assert(l && l->vms.size() == 2 && l->vms[0].state == State::Running && l->vms[0].memory == 2147483648LL && l->vms[0].cpu == 12);
    assert(l->vms[0].ip == "192.168.122.80" && l->vms[1].state == State::Saved && l->errors.size() == 1 && !l->noModule);
    assert(hyperv::parseListing(R"({"noModule":true})", &error)->noModule);
    assert(!hyperv::parseListing("Get-VM : You do not have the required permission", &error) && !error.isEmpty());
    assert(hyperv::parseState(QStringLiteral("Reset")) == State::Resetting && hyperv::parseState(QStringLiteral("Hibernated")) == State::Other);
    assert(hyperv::actionScript(Action::Start, "x'; Remove-Item C:\\ -Recurse; '").isEmpty()); // only GUIDs reach PowerShell
    assert(hyperv::actionScript(Action::TurnOff, "8d1c0e2a-0b5e-4c4b-9a55-0123456789ab").contains(QLatin1String("Stop-VM -VM $vm -TurnOff")));
    assert(hyperv::answerEnd("<<portal-mux ok>>") == true && hyperv::answerEnd("<<portal-mux error>>") == false && !hyperv::answerEnd("ok"));
    assert(hyperv::allowed(Action::Start, State::Saved) && !hyperv::allowed(Action::Start, State::Running));
    assert(hyperv::allowed(Action::TurnOff, State::Stopping) && !hyperv::allowed(Action::TurnOff, State::Off));
    const std::string loop = hyperv::loopCommand();
    assert(loop.rfind("powershell ", 0) == 0 && loop.size() < 8000); // cmd.exe's limit
}

static void azureTests()
{
    QString error;
    const auto page = azure::parsePage(R"({"value":[{"id":"a"}],"nextLink":"https://next"})", &error);
    assert(page && page->value.size() == 1 && page->next == "https://next");
    assert(!azure::parsePage("<html>", &error) && !error.isEmpty());

    const QJsonArray model = QJsonDocument::fromJson(R"([
        {"id":"/subscriptions/S1/resourceGroups/RG1/providers/Microsoft.Compute/virtualMachines/web","name":"web","location":"westeurope",
         "properties":{"hardwareProfile":{"vmSize":"Standard_B1s"},"storageProfile":{"osDisk":{"osType":"Linux"}},
                       "osProfile":{"adminUsername":"azureuser"},"diagnosticsProfile":{"bootDiagnostics":{"enabled":true}},
                       "networkProfile":{"networkInterfaces":[{"id":"/x/nic-b"},{"id":"/x/NIC-A","properties":{"primary":true}}]}}},
        {"id":"/subscriptions/S1/resourceGroups/rg2/providers/Microsoft.Compute/virtualMachines/App","name":"App",
         "properties":{"storageProfile":{"osDisk":{"osType":"Windows"}},"networkProfile":{"networkInterfaces":[{"id":"/x/nic-c"}]}}}
    ])").array();
    std::vector<azure::Vm> vms = azure::vms(model);
    assert(vms.size() == 2 && vms[0].name == "App" && vms[1].name == "web"); // by name, case ignored
    const azure::Vm& web = vms[1];
    assert(web.subscription == "S1" && web.resourceGroup == "RG1" && web.size == "Standard_B1s" && web.os == "Linux" && web.admin == "azureuser");
    assert(web.nics.size() == 2 && web.nics[0] == "/x/NIC-A"); // the primary first
    assert(web.bootDiagnostics && !vms[0].bootDiagnostics);

    azure::applyPower(vms, QJsonDocument::fromJson(R"([
        {"id":"/SUBSCRIPTIONS/s1/resourcegroups/rg1/providers/Microsoft.Compute/virtualMachines/WEB",
         "properties":{"instanceView":{"statuses":[{"code":"ProvisioningState/succeeded"},{"code":"PowerState/running"}]}}}
    ])").array());
    assert(vms[1].power == "running" && vms[0].power.empty()); // Azure changes the IDs' case

    azure::applyAddresses(vms, QJsonDocument::fromJson(R"([
        {"id":"/x/nic-a","properties":{"ipConfigurations":[
            {"properties":{"privateIPAddress":"10.0.0.5"}},
            {"properties":{"primary":true,"privateIPAddress":"10.0.0.4","publicIPAddress":{"id":"/x/IP1"}}}]}},
        {"id":"/x/nic-c","properties":{"ipConfigurations":[{"properties":{"privateIPAddress":"10.1.0.4"}}]}}
    ])").array(), QJsonDocument::fromJson(R"([{"id":"/x/ip1","properties":{"ipAddress":"20.1.2.3"}}])").array());
    assert(vms[1].privateIp == "10.0.0.4" && vms[1].publicIp == "20.1.2.3");
    assert(vms[0].privateIp == "10.1.0.4" && vms[0].publicIp.empty());

    assert(azure::errorText(R"({"error":{"code":"AuthorizationFailed","message":"No access."}})") == "No access.");
    assert(azure::errorText(R"({"error":"invalid_grant","error_description":"AADSTS70008: Expired.\r\nTrace ID: 1"})") == "AADSTS70008: Expired.");

    // as Python: base64.urlsafe_b64encode(hashlib.sha256(v).digest()).rstrip(b"=")
    assert(azure::challenge("dBjftJeZ4CVP-mJ92K27uhbUJU1p1r_wW1gFWFOEjXk") == "ngF5GsXcbwljx6u133FFr3Xht9xooA_DuaX_3QwODtc");
    assert(azure::newVerifier().size() == 43 && azure::newVerifier() != azure::newVerifier());

    azure::Account account;
    account.tenant = "contoso.onmicrosoft.com";
    const QUrl url = azure::authorizeUrl(account, QStringLiteral("http://localhost:4711"), "ch", QStringLiteral("st"));
    assert(url.path() == "/contoso.onmicrosoft.com/oauth2/v2.0/authorize");
    const QUrlQuery query(url);
    assert(query.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded) == "http://localhost:4711");
    assert(query.queryItemValue(QStringLiteral("client_id")) == azure::cliClientId && query.queryItemValue(QStringLiteral("state")) == "st");
    assert(query.queryItemValue(QStringLiteral("scope"), QUrl::FullyDecoded).startsWith("https://management.core.windows.net//.default "));
    account.clientId = "my-app";
    assert(azure::refreshForm(account, QStringLiteral("r")).contains("management.core.windows.net%2F%2Fuser_impersonation"));
    account.clientId.clear();
    assert(azure::refreshForm(account, QStringLiteral("a+b/c=")).contains("refresh_token=a%2Bb%2Fc%3D"));

    const auto r = azure::parseRedirect("GET /?code=0.AX-y&state=st HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert(r && r->code == "0.AX-y" && r->state == "st" && r->error.isEmpty());
    const auto denied = azure::parseRedirect("GET /?error=access_denied&error_description=The+user+said+no&state=st HTTP/1.1\r\n\r\n");
    assert(denied && denied->code.isEmpty() && denied->error == "The user said no");
    assert(!azure::parseRedirect("GET /favicon.ico HTTP/1.1\r\n\r\n"));

    const QByteArray claims = QByteArray(R"({"preferred_username":"ann@contoso.com","tid":"T1"})")
                                  .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const auto tokens = azure::parseTokens(R"({"access_token":"A","refresh_token":"R","expires_in":"3599","id_token":"h.)" + claims + R"(.s"})", &error);
    assert(tokens && tokens->access == "A" && tokens->refresh == "R" && tokens->expiresIn == 3599 && tokens->user == "ann@contoso.com" &&
           tokens->tenantId == "T1");
    assert(!azure::parseTokens(R"({"error":"invalid_grant","error_description":"AADSTS50173: Revoked."})", &error) && error == "AADSTS50173: Revoked.");
}

int main()
{
    azureTests();
    hypervTests();
    forwardTests();
    telnetTests();
    proxmoxTests();
    addressTests();
    gameStreamTests();
    scrcpyMessageTests();
    videoDecoderTests();
    sshConfigTests();
    domainXmlTests();
    titleFolderTests();
    krdcImportTests();
    propertyTests();
    rectTests();
    framebufferTests();
    connectionTests();
    std::puts("core_tests: OK");
}
