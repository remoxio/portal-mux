#pragma once

#include <set>
#include <string>
#include <string_view>

namespace rdc::telnet {

enum : unsigned char { SE = 240, SB = 250, WILL = 251, WONT = 252, DO = 253, DONT = 254, IAC = 255 };
enum : unsigned char { Binary = 0, Echo = 1, SuppressGoAhead = 3, TerminalType = 24, WindowSize = 31 };

// RFC 854 framing and options: window size, terminal type, echo, SGA, binary
class Protocol {
public:
    explicit Protocol(std::string terminalType) : terminalType_(std::move(terminalType)) {}

    std::string start(); // our offers, sent once connected
    // `in` from the server: `text` for the terminal, the reply (if any) to send back
    std::string receive(std::string_view in, std::string& text);
    std::string encode(std::string_view keys) const;
    std::string resize(int cols, int rows); // empty until the server agreed to NAWS

private:
    std::string command(unsigned char verb, unsigned char option);
    std::string windowSize() const;

    std::string terminalType_;
    int cols_ = 80, rows_ = 24;
    std::set<unsigned char> us_, him_;           // enabled on our side, on the server's
    std::set<unsigned char> offeredUs_, askedHim_; // sent, no answer yet: an answer needs no reply
    enum class State { Data, Cr, Iac, Verb, SbOption, SbData, SbIac } state_ = State::Data;
    unsigned char verb_ = 0, sbOption_ = 0;
    std::string sb_;
};

} // namespace rdc::telnet
