#pragma once

#include "core/Connection.h"

#include <QByteArray>
#include <QString>

#include <optional>

// On failure the result is empty and *error says why

std::optional<rdc::Connection> connectionFromAddress(const QString& text, rdc::Protocol fallback, QString* error);

// Saved passwords in them are machine-encrypted, so not read
std::optional<rdc::Connection> connectionFromFile(const QString& path, QString* error);

// Empty if not a MAC address
QByteArray magicPacket(const QString& mac);

// The folder in a terminal title as "user@host: ~/dir" (many prompts set it); ~ is home. Empty if none.
QString titleFolder(const QString& title, const QString& home);
