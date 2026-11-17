#include "stream/Telnet.h"

namespace rdc::telnet {

static bool weDo(unsigned char option) { return option == WindowSize || option == TerminalType || option == Binary; }
static bool heMay(unsigned char option) { return option == Echo || option == SuppressGoAhead || option == Binary; }

static void putByte(std::string& out, unsigned char b)
{
    out += char(b);
    if (b == IAC) out += char(IAC);
}

std::string Protocol::command(unsigned char verb, unsigned char option) { return {char(IAC), char(verb), char(option)}; }

std::string Protocol::windowSize() const
{
    std::string out = {char(IAC), char(SB), char(WindowSize)};
    for (int v : {cols_, rows_}) {
        putByte(out, static_cast<unsigned char>(v >> 8));
        putByte(out, static_cast<unsigned char>(v));
    }
    return out + char(IAC) + char(SE);
}

std::string Protocol::start()
{
    std::string out;
    for (unsigned char option : {WindowSize, TerminalType}) {
        offeredUs_.insert(option);
        out += command(WILL, option);
    }
    askedHim_.insert(SuppressGoAhead);
    return out + command(DO, SuppressGoAhead);
}

std::string Protocol::resize(int cols, int rows)
{
    cols_ = cols;
    rows_ = rows;
    return us_.count(WindowSize) ? windowSize() : std::string();
}

std::string Protocol::encode(std::string_view keys) const
{
    std::string out;
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto b = static_cast<unsigned char>(keys[i]);
        putByte(out, b);
        if (b == '\r' && !us_.count(Binary) && (i + 1 == keys.size() || keys[i + 1] != '\n')) out += '\0'; // NVT: bare CR is CR NUL
    }
    return out;
}

std::string Protocol::receive(std::string_view in, std::string& text)
{
    std::string reply;
    for (const char c : in) {
        const auto b = static_cast<unsigned char>(c);
        switch (state_) {
        case State::Cr:
            state_ = State::Data;
            if (b == 0) break;
            [[fallthrough]];
        case State::Data:
            if (b == IAC) state_ = State::Iac;
            else {
                text += c;
                if (b == '\r' && !him_.count(Binary)) state_ = State::Cr;
            }
            break;
        case State::Iac:
            if (b == IAC) {
                text += c;
                state_ = State::Data;
            } else if (b == SB) {
                state_ = State::SbOption;
            } else if (b >= WILL && b <= DONT) {
                verb_ = b;
                state_ = State::Verb;
            } else {
                state_ = State::Data; // NOP, GA, AYT…: nothing to do
            }
            break;
        case State::Verb: {
            state_ = State::Data;
            const bool local = verb_ == DO || verb_ == DONT, on = verb_ == DO || verb_ == WILL;
            std::set<unsigned char>& enabled = local ? us_ : him_;
            std::set<unsigned char>& pending = local ? offeredUs_ : askedHim_;
            const bool supported = local ? weDo(b) : heMay(b);
            const bool answer = pending.erase(b) > 0;
            if (on && supported) {
                if (enabled.insert(b).second && !answer) reply += command(local ? WILL : DO, b);
                if (local && b == WindowSize) reply += windowSize();
            } else if (on) {
                reply += command(local ? WONT : DONT, b);
            } else if (enabled.erase(b) && !answer) {
                reply += command(local ? WONT : DONT, b);
            }
            break;
        }
        case State::SbOption:
            sbOption_ = b;
            sb_.clear();
            state_ = State::SbData;
            break;
        case State::SbData:
            if (b == IAC) state_ = State::SbIac;
            else sb_ += c;
            break;
        case State::SbIac:
            if (b == IAC) {
                sb_ += c;
                state_ = State::SbData;
                break;
            }
            state_ = State::Data;
            if (b == SE && sbOption_ == TerminalType && sb_ == std::string(1, '\1')) { // SEND
                reply += std::string{char(IAC), char(SB), char(TerminalType), '\0'};
                for (const char t : terminalType_) putByte(reply, static_cast<unsigned char>(t));
                reply += std::string{char(IAC), char(SE)};
            }
            break;
        }
    }
    return reply;
}

} // namespace rdc::telnet
