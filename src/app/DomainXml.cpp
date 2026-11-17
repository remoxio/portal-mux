#include "app/DomainXml.h"

#include <QXmlStreamReader>

#include <algorithm>

using rdc::Connection;
using rdc::Protocol;

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
