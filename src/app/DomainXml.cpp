#include "app/DomainXml.h"

#include <QDomDocument>
#include <QTextStream>
#include <QXmlStreamReader>

#include <algorithm>
#include <vector>

using rdc::Connection;
using rdc::Protocol;
using namespace Qt::StringLiterals;

std::optional<Connection> connectionFromDomainXml(const QString& xml)
{
    QXmlStreamReader r(xml);
    Connection c;
    std::optional<Connection> display;
    int depth = 0;
    bool inGraphics = false, tcp = true;
    while (!r.atEnd()) {
        const QXmlStreamReader::TokenType token = r.readNext();
        if (token == QXmlStreamReader::EndElement) {
            if (inGraphics && depth == 3) {
                if (tcp && (c.port > 0 || c.number("tlsPort") > 0) && !display) display = c;
                inGraphics = false;
            }
            --depth;
            continue;
        }
        if (token != QXmlStreamReader::StartElement) continue;
        ++depth;
        const auto a = r.attributes();
        const QStringView name = r.name();
        if (depth == 2 && name == u"name") {
            c.name = r.readElementText().toStdString();
            --depth; // readElementText() consumed the end element
        } else if (depth == 2 && name == u"uuid") {
            c.id = "libvirt:" + r.readElementText().toStdString();
            --depth;
        } else if (depth == 3 && name == u"graphics") {
            const QStringView type = a.value(u"type");
            if (type != u"spice" && type != u"vnc") continue;
            inGraphics = true;
            tcp = true;
            c.protocol = type == u"spice" ? Protocol::SPICE : Protocol::VNC;
            c.port = std::max(0, a.value(u"port").toInt()); // -1: none (SPICE with only a TLS port)
            c.options.clear();
            if (const int tls = a.value(u"tlsPort").toInt(); tls > 0 && c.protocol == Protocol::SPICE)
                c.options["tlsPort"] = std::to_string(tls); // shortcut: no CA or subject set, spice TLS untested
            c.password = a.value(u"passwd").toString().toStdString(); // only with VIR_DOMAIN_XML_SECURE
            c.host = a.value(u"listen").toString().toStdString();
        } else if (inGraphics && depth == 4 && name == u"listen") {
            const QStringView type = a.value(u"type");
            if (type == u"socket" || type == u"none") tcp = false;
            if (a.hasAttribute(u"address")) c.host = a.value(u"address").toString().toStdString();
        }
    }
    if (!display || r.hasError()) return std::nullopt;
    const std::string& host = display->host;
    if (host.empty() || host == "0.0.0.0" || host == "::") display->host = "127.0.0.1"; // any address: reach it here
    display->name = c.name;
    display->id = c.id;
    return display;
}

namespace {

QDomElement firstCd(const QDomElement& root)
{
    for (QDomElement d = root.firstChildElement(u"devices"_s).firstChildElement(u"disk"_s); !d.isNull(); d = d.nextSiblingElement(u"disk"_s))
        if (d.attribute(u"device"_s) == u"cdrom") return d;
    return {};
}

// devices with a <boot order>, first to boot first
std::vector<QDomElement> bootDevices(const QDomElement& root)
{
    std::vector<QDomElement> devices;
    for (QDomElement d = root.firstChildElement(u"devices"_s).firstChildElement(); !d.isNull(); d = d.nextSiblingElement())
        if (!d.firstChildElement(u"boot"_s).isNull()) devices.push_back(d);
    std::stable_sort(devices.begin(), devices.end(), [](const QDomElement& a, const QDomElement& b) {
        return a.firstChildElement(u"boot"_s).attribute(u"order"_s).toInt() < b.firstChildElement(u"boot"_s).attribute(u"order"_s).toInt();
    });
    return devices;
}

QStringList osBoot(const QDomElement& root)
{
    QStringList devs;
    for (QDomElement b = root.firstChildElement(u"os"_s).firstChildElement(u"boot"_s); !b.isNull(); b = b.nextSiblingElement(u"boot"_s))
        devs << b.attribute(u"dev"_s);
    return devs;
}

void setText(QDomElement e, const QString& text)
{
    while (e.hasChildNodes()) e.removeChild(e.firstChild());
    e.appendChild(e.ownerDocument().createTextNode(text));
}

int attr(const QDomElement& e, const QString& name) { return e.hasAttribute(name) ? e.attribute(name).toInt() : 1; }

} // namespace

VmSettings domainSettings(const QString& xml)
{
    QDomDocument doc;
    if (!doc.setContent(xml)) return {};
    const QDomElement root = doc.documentElement();
    VmSettings s;
    const QDomElement vcpu = root.firstChildElement(u"vcpu"_s);
    s.cpus = vcpu.attribute(u"current"_s, vcpu.text()).toInt();
    s.cpusFixed = !root.firstChildElement(u"vcpus"_s).isNull();
    s.memoryMiB = int(root.firstChildElement(u"memory"_s).text().toLongLong() / 1024); // libvirt writes KiB
    s.memoryFixed = !root.firstChildElement(u"maxMemory"_s).isNull() || !root.firstChildElement(u"cpu"_s).firstChildElement(u"numa"_s).isNull();
    const QDomElement cd = firstCd(root);
    s.hasCd = !cd.isNull();
    const QDomElement source = cd.firstChildElement(u"source"_s);
    s.iso = source.attribute(u"file"_s, source.attribute(u"dev"_s));
    const std::vector<QDomElement> devices = bootDevices(root);
    s.cdFirst = s.hasCd && (devices.empty() ? osBoot(root).value(0) == u"cdrom" : devices.front() == cd);
    return s;
}

QString withSettings(const QString& xml, const VmSettings& s)
{
    QDomDocument doc;
    if (!doc.setContent(xml)) return {};
    QDomElement root = doc.documentElement();
    const VmSettings was = domainSettings(xml);
    if (s.cpus != was.cpus && !was.cpusFixed) {
        QDomElement vcpu = root.firstChildElement(u"vcpu"_s);
        vcpu.removeAttribute(u"current"_s);
        setText(vcpu, QString::number(s.cpus));
        if (QDomElement t = root.firstChildElement(u"cpu"_s).firstChildElement(u"topology"_s); !t.isNull()) {
            const int others = attr(t, u"sockets"_s) * attr(t, u"dies"_s) * attr(t, u"clusters"_s) * attr(t, u"threads"_s);
            if (s.cpus % others) { // keeps its sockets and threads where they fit
                for (const QString& a : {u"sockets"_s, u"dies"_s, u"clusters"_s, u"threads"_s})
                    if (t.hasAttribute(a)) t.setAttribute(a, 1);
                t.setAttribute(u"cores"_s, s.cpus);
            } else {
                t.setAttribute(u"cores"_s, s.cpus / others);
            }
        }
    }
    if (s.memoryMiB != was.memoryMiB && !was.memoryFixed)
        for (const QString& name : {u"memory"_s, u"currentMemory"_s})
            if (QDomElement m = root.firstChildElement(name); !m.isNull()) {
                m.setAttribute(u"unit"_s, u"KiB"_s);
                setText(m, QString::number(qint64(s.memoryMiB) * 1024));
            }
    if (const QDomElement cd = firstCd(root); s.cdFirst != was.cdFirst && !cd.isNull()) {
        if (std::vector<QDomElement> devices = bootDevices(root); !devices.empty()) {
            std::erase(devices, cd);
            devices.insert(s.cdFirst ? devices.begin() : devices.end(), cd); // not first: last, still bootable
            for (size_t i = 0; i < devices.size(); ++i) {
                QDomElement boot = devices[i].firstChildElement(u"boot"_s);
                if (boot.isNull()) boot = devices[i].appendChild(doc.createElement(u"boot"_s)).toElement();
                boot.setAttribute(u"order"_s, int(i + 1));
            }
        } else {
            QDomElement os = root.firstChildElement(u"os"_s);
            QStringList devs = osBoot(root);
            while (!os.firstChildElement(u"boot"_s).isNull()) os.removeChild(os.firstChildElement(u"boot"_s));
            if (devs.isEmpty()) devs << u"hd"_s; // libvirt's default
            devs.removeAll(u"cdrom"_s);
            devs.insert(s.cdFirst ? 0 : devs.size(), u"cdrom"_s);
            for (const QString& dev : devs) os.appendChild(doc.createElement(u"boot"_s)).toElement().setAttribute(u"dev"_s, dev);
        }
    }
    return doc.toString(2);
}

QString cdromXml(const QString& xml, const QString& iso)
{
    QDomDocument doc;
    if (!doc.setContent(xml)) return {};
    QDomElement cd = firstCd(doc.documentElement());
    if (cd.isNull()) return {};
    cd.removeChild(cd.firstChildElement(u"source"_s));
    if (!iso.isEmpty()) {
        cd.setAttribute(u"type"_s, u"file"_s);
        cd.insertBefore(doc.createElement(u"source"_s), cd.firstChildElement(u"target"_s)).toElement().setAttribute(u"file"_s, iso);
    }
    QString out;
    QTextStream stream(&out);
    cd.save(stream, 2);
    return out;
}
