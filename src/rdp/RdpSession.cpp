#include "rdp/RdpSession.h"

#include <freerdp/channels/disp.h>
#include <freerdp/client.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/client_cliprdr_file.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/client/disp.h>
#include <freerdp/codec/color.h>
#include <freerdp/event.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/graphics.h>
#include <freerdp/locale/locale.h>
#include <freerdp/settings.h>
#include <freerdp/utils/cliprdr_utils.h>
#include <winpr/clipboard.h>
#include <winpr/string.h>
#include <winpr/synch.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <climits>
#include <cmath>
#include <sstream>

namespace rdc {

namespace {

struct RdpContext {
    rdpClientContext common; // must be first: FreeRDP casts
    RdpSession* self;
};

struct RdpPointer {
    rdpPointer base; // must be first
    BYTE* bgra;
};

std::string str(const char* s) { return s ? s : ""; }

char* dupOrNull(const std::string& s) { return s.empty() ? nullptr : _strdup(s.c_str()); }

std::string replaceAll(std::string s, const std::string& from, const std::string& to)
{
    for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size()) s.replace(pos, from.size(), to);
    return s;
}

std::string percentEncode(const std::string& path)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : path) {
        if (std::isalnum(ch) || ch == '/' || ch == '-' || ch == '.' || ch == '_' || ch == '~') {
            out += char(ch);
        } else {
            out += '%';
            out += hex[ch >> 4];
            out += hex[ch & 15];
        }
    }
    return out;
}

std::string certDetails(const char* host, UINT16 port, const char* commonName, const char* subject,
                        const char* issuer, const char* fingerprint, DWORD flags)
{
    return "Host: " + str(host) + ":" + std::to_string(port) + "\nCommon name: " + str(commonName) +
           "\nSubject: " + str(subject) + "\nIssuer: " + str(issuer) +
           (flags & VERIFY_CERT_FLAG_FP_IS_PEM ? "\nCertificate:\n" : "\nFingerprint: ") + str(fingerprint);
}

} // namespace

// nested for private access
struct RdpSession::Callbacks {
    static RdpSession* self(rdpContext* c) { return reinterpret_cast<RdpContext*>(c)->self; }

    static BOOL clientNew(freerdp* instance, rdpContext*)
    {
        instance->PreConnect = preConnect;
        instance->PostConnect = postConnect;
        instance->PostDisconnect = postDisconnect;
        instance->AuthenticateEx = authenticate;
        instance->VerifyCertificateEx = verifyCertificate;
        instance->VerifyChangedCertificateEx = verifyChangedCertificate;
        instance->LoadChannels = freerdp_client_load_channels;
        return TRUE;
    }

    static BOOL preConnect(freerdp* instance)
    {
        rdpContext* ctx = instance->context;
        return PubSub_SubscribeChannelConnected(ctx->pubSub, channelConnected) >= 0 &&
               PubSub_SubscribeChannelDisconnected(ctx->pubSub, channelDisconnected) >= 0;
    }

    static BOOL postConnect(freerdp* instance)
    {
        rdpContext* ctx = instance->context;
        if (!gdi_init(instance, PIXEL_FORMAT_BGRX32)) return FALSE;

        rdpPointer pointer{};
        pointer.size = sizeof(RdpPointer);
        pointer.New = pointerNew;
        pointer.Free = pointerFree;
        pointer.Set = pointerSet;
        pointer.SetNull = pointerSetNull;
        pointer.SetDefault = pointerSetDefault;
        pointer.SetPosition = pointerSetPosition;
        graphics_register_pointer(ctx->graphics, &pointer);

        ctx->update->BeginPaint = beginPaint;
        ctx->update->EndPaint = endPaint;
        ctx->update->DesktopResize = desktopResize;
        publishWholeScreen(ctx);
        return TRUE;
    }

    static void postDisconnect(freerdp* instance) { gdi_free(instance); }

    static void publishWholeScreen(rdpContext* ctx)
    {
        rdpGdi* gdi = ctx->gdi;
        RdpSession* s = self(ctx);
        const Rect all{0, 0, gdi->width, gdi->height};
        s->framebuffer_.resize(gdi->width, gdi->height);
        s->framebuffer_.update(all, gdi->primary_buffer, int(gdi->stride));
        s->listener_.onResize(gdi->width, gdi->height);
        s->listener_.onFrameUpdated(all);
    }

    static BOOL beginPaint(rdpContext* ctx)
    {
        HGDI_WND hwnd = ctx->gdi->primary->hdc->hwnd;
        hwnd->invalid->null = TRUE;
        hwnd->ninvalid = 0;
        return TRUE;
    }

    static BOOL endPaint(rdpContext* ctx)
    {
        rdpGdi* gdi = ctx->gdi;
        HGDI_RGN invalid = gdi->primary->hdc->hwnd->invalid;
        if (invalid->null) return TRUE;
        const Rect r{invalid->x, invalid->y, invalid->w, invalid->h};
        RdpSession* s = self(ctx);
        s->framebuffer_.update(r, gdi->primary_buffer, int(gdi->stride));
        s->listener_.onFrameUpdated(r);
        return TRUE;
    }

    static BOOL desktopResize(rdpContext* ctx)
    {
        rdpSettings* st = ctx->settings;
        if (!gdi_resize(ctx->gdi, freerdp_settings_get_uint32(st, FreeRDP_DesktopWidth),
                        freerdp_settings_get_uint32(st, FreeRDP_DesktopHeight)))
            return FALSE;
        publishWholeScreen(ctx);
        return TRUE;
    }

    static BOOL pointerNew(rdpContext* ctx, rdpPointer* p)
    {
        auto* rp = reinterpret_cast<RdpPointer*>(p);
        rp->bgra = nullptr;
        if (p->width == 0 || p->height == 0) return TRUE;
        rp->bgra = static_cast<BYTE*>(calloc(size_t(p->width) * p->height, 4));
        if (!rp->bgra) return FALSE;
        return freerdp_image_copy_from_pointer_data(rp->bgra, PIXEL_FORMAT_BGRA32, 0, 0, 0, p->width, p->height,
                                                    p->xorMaskData, p->lengthXorMask, p->andMaskData,
                                                    p->lengthAndMask, p->xorBpp, &ctx->gdi->palette);
    }

    static void pointerFree(rdpContext*, rdpPointer* p)
    {
        auto* rp = reinterpret_cast<RdpPointer*>(p);
        free(rp->bgra);
        rp->bgra = nullptr;
    }

    static BOOL pointerSet(rdpContext* ctx, rdpPointer* p)
    {
        auto* rp = reinterpret_cast<RdpPointer*>(p);
        std::vector<uint8_t> image;
        if (rp->bgra) image.assign(rp->bgra, rp->bgra + size_t(p->width) * p->height * 4);
        self(ctx)->listener_.onCursor(std::move(image), int(p->width), int(p->height), int(p->xPos), int(p->yPos));
        return TRUE;
    }

    static BOOL pointerSetNull(rdpContext* ctx)
    {
        self(ctx)->listener_.onCursor(std::vector<uint8_t>(4, 0), 1, 1, 0, 0); // one transparent pixel hides it
        return TRUE;
    }

    static BOOL pointerSetDefault(rdpContext* ctx)
    {
        self(ctx)->listener_.onCursor({}, 0, 0, 0, 0);
        return TRUE;
    }

    static BOOL pointerSetPosition(rdpContext*, UINT32, UINT32) { return TRUE; }

    static BOOL authenticate(freerdp* instance, char** username, char** password, char** domain,
                             rdp_auth_reason reason)
    {
        RdpSession* s = self(instance->context);
        Credentials c{str(*username), str(*password), str(*domain), true, true};
        const bool gateway = reason == GW_AUTH_HTTP || reason == GW_AUTH_RDG || reason == GW_AUTH_RPC;
        if (!s->listener_.askCredentials(gateway ? "Log in to the RD Gateway" : "Log in to the remote computer", c)) {
            s->cancelled_ = true;
            return FALSE;
        }
        free(*username);
        free(*password);
        free(*domain);
        *username = dupOrNull(c.username);
        *password = _strdup(c.password.c_str());
        *domain = dupOrNull(c.domain);
        return *password != nullptr;
    }

    static DWORD verifyCertificate(freerdp* instance, const char* host, UINT16 port, const char* commonName,
                                   const char* subject, const char* issuer, const char* fingerprint, DWORD flags)
    {
        RdpSession* s = self(instance->context);
        if (s->alwaysTrust_) return 1;
        const char* title = flags & VERIFY_CERT_FLAG_MISMATCH
                                ? "The server certificate does not match the host name. Trust it anyway?"
                                : "The server certificate is not known. Trust it?";
        if (s->listener_.askTrust(title, certDetails(host, port, commonName, subject, issuer, fingerprint, flags),
                                  &s->alwaysTrust_))
            return 1;
        s->cancelled_ = true;
        return 0;
    }

    static DWORD verifyChangedCertificate(freerdp* instance, const char* host, UINT16 port, const char* commonName,
                                          const char* subject, const char* issuer, const char* fingerprint,
                                          const char* oldSubject, const char* oldIssuer, const char* oldFingerprint,
                                          DWORD flags)
    {
        RdpSession* s = self(instance->context);
        if (s->alwaysTrust_) return 1;
        std::string details = "NEW\n" + certDetails(host, port, commonName, subject, issuer, fingerprint, flags) +
                              "\n\nPREVIOUSLY TRUSTED\nSubject: " + str(oldSubject) + "\nIssuer: " + str(oldIssuer) +
                              "\nFingerprint: " + str(oldFingerprint);
        if (s->listener_.askTrust("WARNING: the server certificate has CHANGED. Someone may be intercepting the "
                                  "connection. Trust the new certificate?",
                                  details, &s->alwaysTrust_))
            return 1;
        s->cancelled_ = true;
        return 0;
    }

    static void channelConnected(void* context, const ChannelConnectedEventArgs* e)
    {
        auto* ctx = static_cast<rdpContext*>(context);
        RdpSession* s = self(ctx);
        if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
            auto* c = static_cast<CliprdrClientContext*>(e->pInterface);
            c->MonitorReady = clipMonitorReady;
            c->ServerCapabilities = clipServerCapabilities;
            c->ServerFormatList = clipServerFormatList;
            c->ServerFormatListResponse = [](CliprdrClientContext*, const CLIPRDR_FORMAT_LIST_RESPONSE*) -> UINT {
                return CHANNEL_RC_OK;
            };
            c->ServerFormatDataRequest = clipDataRequest;
            c->ServerFormatDataResponse = clipDataResponse;
            cliprdr_file_context_init(s->fileContext_, c); // sets c->custom, serves FileContents
            std::lock_guard lock(s->clipMutex_);
            s->cliprdr_ = c;
        } else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0) {
            auto* d = static_cast<DispClientContext*>(e->pInterface);
            d->custom = s;
            d->DisplayControlCaps = dispCaps;
        } else {
            freerdp_client_OnChannelConnectedEventHandler(context, e);
        }
    }

    static void channelDisconnected(void* context, const ChannelDisconnectedEventArgs* e)
    {
        RdpSession* s = self(static_cast<rdpContext*>(context));
        if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
            cliprdr_file_context_uninit(s->fileContext_, static_cast<CliprdrClientContext*>(e->pInterface));
            std::lock_guard lock(s->clipMutex_);
            s->cliprdr_ = nullptr;
        } else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0) {
            s->post([s] {
                s->disp_ = nullptr;
                s->dispReady_ = false;
            });
        } else {
            freerdp_client_OnChannelDisconnectedEventHandler(context, e);
        }
    }

    static UINT dispCaps(DispClientContext* d, UINT32, UINT32, UINT32)
    {
        auto* s = static_cast<RdpSession*>(d->custom);
        s->post([s, d] {
            s->disp_ = d;
            s->dispReady_ = true;
            s->sendLayout();
        });
        return CHANNEL_RC_OK;
    }

    // files follow FreeRDP's SDL client: FileGroupDescriptorW <-> uri-list via WinPR

    static RdpSession* clipSelf(CliprdrClientContext* c)
    {
        return static_cast<RdpSession*>(cliprdr_file_context_get_context(static_cast<CliprdrFileContext*>(c->custom)));
    }

    static UINT clipMonitorReady(CliprdrClientContext* c, const CLIPRDR_MONITOR_READY*)
    {
        RdpSession* s = clipSelf(c);
        CLIPRDR_GENERAL_CAPABILITY_SET general{};
        general.capabilitySetType = CB_CAPSTYPE_GENERAL;
        general.capabilitySetLength = 12;
        general.version = CB_CAPS_VERSION_2;
        general.generalFlags = CB_USE_LONG_FORMAT_NAMES | cliprdr_file_context_current_flags(s->fileContext_);
        CLIPRDR_CAPABILITIES caps{};
        caps.cCapabilitiesSets = 1;
        caps.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(&general);
        if (getenv("PORTAL_MUX_DEBUG")) fprintf(stderr, "cliprdr: client caps flags=0x%x (server 0x%x)\n", general.generalFlags, cliprdr_file_context_remote_get_flags(s->fileContext_));
        if (UINT rc = c->ClientCapabilities(c, &caps); rc != CHANNEL_RC_OK) return rc;
        s->announceClipboard();
        return CHANNEL_RC_OK;
    }

    static UINT clipServerCapabilities(CliprdrClientContext* c, const CLIPRDR_CAPABILITIES* caps)
    {
        RdpSession* s = clipSelf(c);
        if (!cliprdr_file_context_remote_set_flags(s->fileContext_, 0)) return ERROR_INTERNAL_ERROR;
        auto* p = reinterpret_cast<const BYTE*>(caps->capabilitySets);
        for (UINT32 i = 0; i < caps->cCapabilitiesSets; ++i) {
            auto* set = reinterpret_cast<const CLIPRDR_CAPABILITY_SET*>(p);
            if (set->capabilitySetType == CB_CAPSTYPE_GENERAL &&
                !cliprdr_file_context_remote_set_flags(
                    s->fileContext_, reinterpret_cast<const CLIPRDR_GENERAL_CAPABILITY_SET*>(set)->generalFlags))
                return ERROR_INTERNAL_ERROR;
            p += set->capabilitySetLength;
        }
        return CHANNEL_RC_OK;
    }

    static UINT clipServerFormatList(CliprdrClientContext* c, const CLIPRDR_FORMAT_LIST* list)
    {
        RdpSession* s = clipSelf(c);
        CLIPRDR_FORMAT_LIST_RESPONSE response{};
        response.common.msgType = CB_FORMAT_LIST_RESPONSE;
        response.common.msgFlags = CB_RESPONSE_OK;
        if (UINT rc = c->ClientFormatListResponse(c, &response); rc != CHANNEL_RC_OK) return rc;

        bool text = false;
        {
            std::lock_guard lock(s->clipMutex_);
            s->serverFileGroupId_ = 0;
            for (UINT32 i = 0; i < list->numFormats; ++i) {
                const CLIPRDR_FORMAT& f = list->formats[i];
                if (f.formatName && strcmp(f.formatName, "FileGroupDescriptorW") == 0) s->serverFileGroupId_ = f.formatId;
                if (f.formatId == CF_UNICODETEXT) text = true;
            }
            s->pendingFormat_ = s->serverFileGroupId_ ? s->serverFileGroupId_ : text ? CF_UNICODETEXT : 0;
        }
        if (UINT rc = cliprdr_file_context_notify_new_server_format_list(s->fileContext_); rc != CHANNEL_RC_OK) return rc;
        if (!s->pendingFormat_) return CHANNEL_RC_OK;
        // eager, so the local clipboard mirrors the remote
        CLIPRDR_FORMAT_DATA_REQUEST request{};
        request.common.msgType = CB_FORMAT_DATA_REQUEST;
        request.requestedFormatId = s->pendingFormat_;
        return c->ClientFormatDataRequest(c, &request);
    }

    static UINT clipDataResponse(CliprdrClientContext* c, const CLIPRDR_FORMAT_DATA_RESPONSE* r)
    {
        RdpSession* s = clipSelf(c);
        if (!(r->common.msgFlags & CB_RESPONSE_OK) || !r->requestedFormatData) return CHANNEL_RC_OK;
        std::unique_lock lock(s->clipMutex_);
        if (s->pendingFormat_ != 0 && s->pendingFormat_ == s->serverFileGroupId_) {
            std::string uris;
            ClipboardLock(s->clipboard_);
            // WinPR's FileGroupDescriptorW -> uri-list returns nothing in 3.32; built here
            FILEDESCRIPTORW* files = nullptr;
            UINT32 count = 0;
            if (cliprdr_file_context_has_local_support(s->fileContext_) &&
                cliprdr_file_context_update_server_data(s->fileContext_, s->clipboard_, r->requestedFormatData,
                                                        r->common.dataLen) &&
                cliprdr_parse_file_list(r->requestedFormatData, r->common.dataLen, &files, &count) == CHANNEL_RC_OK) {
                const char* base = ClipboardGetDelegate(s->clipboard_)->basePath;
                for (UINT32 i = 0; base && i < count; ++i) {
                    char* name = ConvertWCharNToUtf8Alloc(files[i].cFileName, ARRAYSIZE(files[i].cFileName), nullptr);
                    if (name && !strchr(name, '\\')) // top level only; folders bring their contents along
                        uris += "file://" + percentEncode(std::string(base) + "/" + name) + "\r\n";
                    free(name);
                }
            }
            free(files);
            ClipboardUnlock(s->clipboard_);
            lock.unlock();
            if (!uris.empty()) s->listener_.onClipboardUris(std::move(uris));
            return CHANNEL_RC_OK;
        }
        size_t len = 0;
        char* utf8 = ConvertWCharNToUtf8Alloc(reinterpret_cast<const WCHAR*>(r->requestedFormatData),
                                              r->common.dataLen / sizeof(WCHAR), &len);
        if (!utf8) return CHANNEL_RC_OK;
        std::string text = replaceAll(std::string(utf8, strnlen(utf8, len)), "\r\n", "\n");
        free(utf8);
        s->localClipboard_ = text; // the UI's echo then sees no change
        s->localUris_.clear();
        lock.unlock();
        s->listener_.onClipboardText(std::move(text));
        return CHANNEL_RC_OK;
    }

    static UINT clipDataRequest(CliprdrClientContext* c, const CLIPRDR_FORMAT_DATA_REQUEST* request)
    {
        RdpSession* s = clipSelf(c);
        CLIPRDR_FORMAT_DATA_RESPONSE response{};
        response.common.msgType = CB_FORMAT_DATA_RESPONSE;
        response.common.msgFlags = CB_RESPONSE_FAIL;
        BYTE* data = nullptr;
        UINT32 size = 0;
        {
            std::lock_guard lock(s->clipMutex_);
            if (request->requestedFormatId == s->fileGroupId_ && !s->localUris_.empty()) {
                ClipboardLock(s->clipboard_);
                UINT32 len = 0;
                FILEDESCRIPTORW* descriptors = nullptr;
                if (cliprdr_file_context_update_client_data(s->fileContext_, s->localUris_.c_str(), s->localUris_.size()) &&
                    ClipboardSetData(s->clipboard_, s->uriListId_, s->localUris_.c_str(), UINT32(s->localUris_.size() + 1)))
                    descriptors = static_cast<FILEDESCRIPTORW*>(ClipboardGetData(s->clipboard_, s->fileGroupId_, &len));
                ClipboardEmpty(s->clipboard_);
                ClipboardUnlock(s->clipboard_);
                if (descriptors &&
                    cliprdr_serialize_file_list_ex(cliprdr_file_context_remote_get_flags(s->fileContext_), descriptors,
                                                   len / sizeof(FILEDESCRIPTORW), &data, &size) != CHANNEL_RC_OK)
                    data = nullptr;
                free(descriptors);
            } else if (request->requestedFormatId == CF_UNICODETEXT) {
                size_t wlen = 0;
                data = reinterpret_cast<BYTE*>(ConvertUtf8ToWCharAlloc(replaceAll(s->localClipboard_, "\n", "\r\n").c_str(), &wlen));
                size = UINT32((wlen + 1) * sizeof(WCHAR));
            }
        }
        if (getenv("PORTAL_MUX_DEBUG"))
            fprintf(stderr, "cliprdr: server requests format %u (our file list id %u): %s, %u bytes\n", request->requestedFormatId,
                    s->fileGroupId_, data ? "ok" : "FAIL", size);
        if (data) {
            response.common.msgFlags = CB_RESPONSE_OK;
            response.common.dataLen = size;
            response.requestedFormatData = data;
        }
        UINT rc = c->ClientFormatDataResponse(c, &response);
        free(data);
        return rc;
    }
};

RdpSession::RdpSession(DisplayListener& listener, Connection connection, std::string configDir)
    : listener_(listener), connection_(std::move(connection)), configDir_(std::move(configDir)),
      alwaysTrust_(connection_.flag("alwaysTrust"))
{
}

RdpSession::~RdpSession()
{
    disconnect();
    if (context_) freerdp_client_context_free(context_);
    if (fileContext_) cliprdr_file_context_free(fileContext_);
    if (clipboard_) ClipboardDestroy(clipboard_);
    if (wakeEvent_) CloseHandle(wakeEvent_);
}

void RdpSession::connect()
{
    if (context_) return;
    RDP_CLIENT_ENTRY_POINTS entry{};
    entry.Size = sizeof(entry);
    entry.Version = RDP_CLIENT_INTERFACE_VERSION;
    entry.ContextSize = sizeof(RdpContext);
    entry.ClientNew = Callbacks::clientNew;
    context_ = freerdp_client_context_new(&entry);
    wakeEvent_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    clipboard_ = ClipboardCreate();
    fileContext_ = cliprdr_file_context_new(this);
    if (clipboard_) {
        fileGroupId_ = ClipboardRegisterFormat(clipboard_, "FileGroupDescriptorW");
        uriListId_ = ClipboardRegisterFormat(clipboard_, "text/uri-list");
    }
    if (fileContext_) (void)cliprdr_file_context_set_locally_available(fileContext_, TRUE);
    if (!context_ || !wakeEvent_ || !clipboard_ || !fileContext_) {
        listener_.onError("Could not initialise FreeRDP.");
        listener_.onStateChanged(SessionState::Error);
        return;
    }
    reinterpret_cast<RdpContext*>(context_)->self = this;
    worker_ = std::jthread([this](std::stop_token st) { run(st); });
}

void RdpSession::disconnect()
{
    if (!worker_.joinable()) return;
    worker_.request_stop();
    freerdp_abort_connect_context(context_);
    SetEvent(wakeEvent_);
    worker_.join();
}

void RdpSession::post(std::function<void()> task)
{
    tasks_.post(std::move(task));
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void RdpSession::sendKey(const KeyEvent& e)
{
    if (e.scancode == 0) return;
    post([this, e] {
        if (connected_) freerdp_input_send_keyboard_event_ex(context_->input, e.pressed, FALSE, e.scancode);
    });
}

void RdpSession::syncLocks(uint8_t locks)
{
    post([this, locks] {
        if (connected_) freerdp_input_send_focus_in_event(context_->input, locks);
    });
}

void RdpSession::sendPointer(const PointerEvent& e)
{
    post([this, e] {
        if (connected_) handlePointer(e);
    });
}

void RdpSession::sendClipboardText(const std::string& text)
{
    {
        std::lock_guard lock(clipMutex_);
        if (text == localClipboard_ && localUris_.empty()) return;
        localClipboard_ = text;
        localUris_.clear();
    }
    post([this] { announceClipboard(); });
}

void RdpSession::sendClipboardUris(const std::string& uriList)
{
    {
        std::lock_guard lock(clipMutex_);
        if (uriList == localUris_) return;
        localUris_ = uriList;
        localClipboard_.clear();
    }
    post([this] { announceClipboard(); });
}

void RdpSession::resizeDesktop(int width, int height)
{
    // even width, 200..8192 per side
    width = std::clamp(width & ~1, 200, 8192);
    height = std::clamp(height, 200, 8192);
    post([this, width, height] {
        wantWidth_ = width;
        wantHeight_ = height;
        sendLayout();
    });
}

void RdpSession::setPixelRatio(double ratio)
{
    post([this, ratio] {
        pixelRatio_ = ratio;
        sendLayout();
    });
}

void RdpSession::handlePointer(const PointerEvent& e)
{
    rdpInput* input = context_->input;
    const auto x = UINT16(std::max(e.x, 0)), y = UINT16(std::max(e.y, 0));
    if (e.x != lastPointer_.x || e.y != lastPointer_.y)
        freerdp_input_send_mouse_event(input, PTR_FLAGS_MOVE, x, y);
    static constexpr std::pair<uint8_t, UINT16> buttons[] = {{PointerEvent::Left, PTR_FLAGS_BUTTON1},
                                                             {PointerEvent::Right, PTR_FLAGS_BUTTON2},
                                                             {PointerEvent::Middle, PTR_FLAGS_BUTTON3}};
    for (auto [bit, flag] : buttons)
        if ((e.buttons ^ lastPointer_.buttons) & bit)
            freerdp_input_send_mouse_event(input, UINT16(flag | (e.buttons & bit ? PTR_FLAGS_DOWN : 0)), x, y);
    for (int delta = e.wheelDelta; delta != 0;) {
        const int step = std::clamp(delta, -255, 255); // 9-bit two's complement on the wire
        const UINT16 flags = PTR_FLAGS_WHEEL | (step < 0 ? PTR_FLAGS_WHEEL_NEGATIVE | ((0x100 + step) & 0xFF) : step);
        freerdp_input_send_mouse_event(input, flags, x, y);
        delta -= step;
    }
    lastPointer_ = e;
    lastPointer_.wheelDelta = 0;
}

void RdpSession::announceClipboard()
{
    std::lock_guard lock(clipMutex_);
    if (!cliprdr_) return; // MonitorReady announces once the channel is up
    static char fileGroupName[] = "FileGroupDescriptorW";
    CLIPRDR_FORMAT format = localUris_.empty() ? CLIPRDR_FORMAT{CF_UNICODETEXT, nullptr} : CLIPRDR_FORMAT{fileGroupId_, fileGroupName};
    CLIPRDR_FORMAT_LIST list{};
    list.common.msgType = CB_FORMAT_LIST;
    list.numFormats = localUris_.empty() && localClipboard_.empty() ? 0 : 1;
    list.formats = &format;
    if (cliprdr_file_context_notify_new_client_format_list(fileContext_) != CHANNEL_RC_OK) return;
    (void)cliprdr_->ClientFormatList(cliprdr_, &list);
}

// the setting is relative to this display: 100 = the size of its own apps
static int desktopScale(int setting, double pixelRatio) { return std::clamp(int(std::lround(setting * pixelRatio)), 100, 500); }

static UINT32 deviceScale(int desktopScale) { return desktopScale >= 180 ? 180 : desktopScale >= 140 ? 140 : 100; } // spec allows only these

void RdpSession::sendLayout()
{
    if (!dispReady_ || !disp_ || !connected_) return;
    rdpGdi* gdi = context_->gdi;
    const int scale = desktopScale(connection_.number("scale"), pixelRatio_);
    if (gdi && gdi->width == wantWidth_ && gdi->height == wantHeight_ && scale == sentScale_) return;
    DISPLAY_CONTROL_MONITOR_LAYOUT monitor{};
    monitor.Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
    monitor.Width = UINT32(wantWidth_);
    monitor.Height = UINT32(wantHeight_);
    monitor.DesktopScaleFactor = UINT32(scale);
    monitor.DeviceScaleFactor = deviceScale(scale);
    if (disp_->SendMonitorLayout(disp_, 1, &monitor) == CHANNEL_RC_OK) sentScale_ = scale;
}

// "x,y,width,height,primary,pixel ratio %;…" in this computer's pixels, from MainWindow's screens
static std::vector<rdpMonitor> parseMonitors(const std::string& text, int scale)
{
    std::vector<rdpMonitor> monitors;
    std::stringstream list(text);
    for (std::string item; std::getline(list, item, ';');) {
        rdpMonitor m{};
        int primary = 0, ratio = 100;
        if (std::sscanf(item.c_str(), "%d,%d,%d,%d,%d,%d", &m.x, &m.y, &m.width, &m.height, &primary, &ratio) < 5 || m.width <= 0 ||
            m.height <= 0)
            continue;
        m.is_primary = primary ? 1 : 0;
        m.attributes.desktopScaleFactor = UINT32(desktopScale(scale, ratio / 100.0));
        m.attributes.deviceScaleFactor = deviceScale(int(m.attributes.desktopScaleFactor));
        monitors.push_back(m);
    }
    if (monitors.size() < 2) monitors.clear();
    return monitors;
}

bool RdpSession::applySettings()
{
    rdpSettings* st = context_->settings;
    const Connection& c = connection_;
    auto setString = [&](FreeRDP_Settings_Keys_String key, const std::string& v) {
        return freerdp_settings_set_string(st, key, v.empty() ? nullptr : v.c_str());
    };
    auto setBool = [&](FreeRDP_Settings_Keys_Bool key, bool v) { return freerdp_settings_set_bool(st, key, v ? TRUE : FALSE); };
    auto setU32 = [&](FreeRDP_Settings_Keys_UInt32 key, int v) { return freerdp_settings_set_uint32(st, key, UINT32(std::max(v, 0))); };
    const bool fixed = c.number("width") > 0 && c.number("height") > 0;
    const std::vector<rdpMonitor> monitors = parseMonitors(c.option("monitors"), c.number("scale"));
    const bool resize = monitors.empty() && (c.option("scaling") == "resize" || (c.option("scaling") == "scale" && !fixed));
    const std::string audio = c.option("audio");
    const int scale = desktopScale(c.number("scale"), pixelRatio_);
    sentScale_ = scale;

    bool ok = setString(FreeRDP_ServerHostname, c.host) && setU32(FreeRDP_ServerPort, c.effectivePort()) &&
              setString(FreeRDP_Username, c.username) && setString(FreeRDP_Password, c.password) &&
              setString(FreeRDP_Domain, c.option("domain")) &&
              setU32(FreeRDP_DesktopWidth, wantWidth_) && setU32(FreeRDP_DesktopHeight, wantHeight_) &&
              setU32(FreeRDP_ColorDepth, c.number("colorDepth")) && setU32(FreeRDP_DesktopScaleFactor, scale) &&
              setU32(FreeRDP_DeviceScaleFactor, deviceScale(scale)) &&
              setBool(FreeRDP_SoftwareGdi, true) && setBool(FreeRDP_SupportDisplayControl, resize) &&
              setBool(FreeRDP_DynamicResolutionUpdate, resize) &&
              setBool(FreeRDP_SupportGraphicsPipeline, c.flag("gfx")) && setBool(FreeRDP_GfxH264, c.flag("h264")) &&
              setBool(FreeRDP_GfxAVC444, c.flag("h264")) &&
              setBool(FreeRDP_RedirectClipboard, true) /* RemoteView gates it: sharing can be switched on mid-session */ && setBool(FreeRDP_AudioPlayback, audio == "local") &&
              setBool(FreeRDP_RemoteConsoleAudio, audio == "remote") && setBool(FreeRDP_AudioCapture, c.flag("microphone")) &&
              setU32(FreeRDP_KeyboardLayout, c.number("keyboardLayout") ? c.number("keyboardLayout") : int(freerdp_get_system_locale_id())) &&
              setU32(FreeRDP_TlsSecLevel, c.number("tlsSecLevel")) &&
              setBool(FreeRDP_RestrictedAdminModeRequired, c.flag("restrictedAdmin")) &&
              setBool(FreeRDP_RemoteCredentialGuard, c.flag("remoteCredentialGuard")) &&
              setBool(FreeRDP_ConsoleSession, c.flag("console")) && setString(FreeRDP_AlternateShell, c.option("alternateShell")) &&
              setString(FreeRDP_ShellWorkingDirectory, c.option("workingDir")) &&
              setU32(FreeRDP_TcpConnectTimeout, c.number("timeout") * 1000) &&
              setBool(FreeRDP_AutoReconnectionEnabled, c.flag("autoReconnect")) &&
              setString(FreeRDP_ConfigPath, configDir_ + "/freerdp");
    if (ok && !monitors.empty()) { // the desktop spans this computer's monitors
        int left = INT32_MAX, top = INT32_MAX, right = INT32_MIN, bottom = INT32_MIN;
        for (const rdpMonitor& m : monitors) {
            if (m.is_primary)
                ok = ok && setU32(FreeRDP_DesktopScaleFactor, int(m.attributes.desktopScaleFactor)) &&
                     setU32(FreeRDP_DeviceScaleFactor, int(m.attributes.deviceScaleFactor));
            left = std::min(left, m.x);
            top = std::min(top, m.y);
            right = std::max(right, m.x + m.width);
            bottom = std::max(bottom, m.y + m.height);
        }
        ok = ok && setBool(FreeRDP_UseMultimon, true) && setBool(FreeRDP_SupportMonitorLayoutPdu, true) &&
             setBool(FreeRDP_HasMonitorAttributes, true) &&
             freerdp_settings_set_monitor_def_array_sorted(st, monitors.data(), monitors.size()) &&
             setU32(FreeRDP_DesktopWidth, right - left) && setU32(FreeRDP_DesktopHeight, bottom - top);
    }

    static constexpr std::pair<const char*, UINT32> connectionTypes[] = {
        {"auto", CONNECTION_TYPE_AUTODETECT}, {"lan", CONNECTION_TYPE_LAN}, {"broadband-high", CONNECTION_TYPE_BROADBAND_HIGH},
        {"wan", CONNECTION_TYPE_WAN}, {"broadband-low", CONNECTION_TYPE_BROADBAND_LOW}, {"satellite", CONNECTION_TYPE_SATELLITE},
        {"modem", CONNECTION_TYPE_MODEM}, {"custom", CONNECTION_TYPE_LAN}};
    const std::string type = c.option("connectionType");
    for (auto [name, value] : connectionTypes)
        if (type == name) ok = ok && freerdp_set_connection_type(st, value);
    if (type == "custom")
        ok = ok && setBool(FreeRDP_DisableWallpaper, !c.flag("wallpaper")) && setBool(FreeRDP_DisableThemes, !c.flag("themes")) &&
             setBool(FreeRDP_AllowFontSmoothing, c.flag("fontSmoothing")) &&
             setBool(FreeRDP_AllowDesktopComposition, c.flag("composition")) &&
             setBool(FreeRDP_DisableFullWindowDrag, !c.flag("fullWindowDrag")) &&
             setBool(FreeRDP_DisableMenuAnims, !c.flag("menuAnimations"));

    if (const std::string sec = c.option("security"); sec != "auto")
        ok = ok && setBool(FreeRDP_NlaSecurity, sec == "nla" || sec == "ext") && setBool(FreeRDP_TlsSecurity, sec == "tls") &&
             setBool(FreeRDP_RdpSecurity, sec == "rdp") && setBool(FreeRDP_ExtSecurity, sec == "ext");

    if (const std::string lb = c.option("loadBalanceInfo"); ok && !lb.empty())
        ok = freerdp_settings_set_pointer_len(st, FreeRDP_LoadBalanceInfo, lb.data(), lb.size());
    if (const std::string blob = c.option("preconnectionBlob"); ok && !blob.empty())
        ok = setString(FreeRDP_PreconnectionBlob, blob) && setBool(FreeRDP_SendPreconnectionPdu, true);

    for (size_t start = 0, end; ok && start <= c.option("drives").size(); start = end + 1) {
        const std::string drives = c.option("drives");
        end = drives.find(';', start);
        if (end == std::string::npos) end = drives.size();
        const std::string path = drives.substr(start, end - start);
        if (path.empty()) continue;
        std::string name = std::filesystem::path(path).filename().string();
        if (name.empty()) name = "share";
        const char* params[] = {"drive", name.c_str(), path.c_str()};
        ok = freerdp_client_add_device_channel(st, 3, params);
    }
    if (ok && c.flag("printers")) {
        const char* params[] = {"printer"};
        ok = freerdp_client_add_device_channel(st, 1, params);
    }
    if (ok && c.flag("smartcards")) {
        const char* params[] = {"smartcard"};
        ok = freerdp_client_add_device_channel(st, 1, params);
    }

    if (const std::string gw = c.option("gateway.host"); ok && !gw.empty()) {
        // shortcut: gateway untested against a real RD Gateway, test when one is available
        const std::string transport = c.option("gateway.transport");
        const bool same = c.flag("gateway.sameCredentials");
        ok = setBool(FreeRDP_GatewayEnabled, true) && setString(FreeRDP_GatewayHostname, gw) &&
             setU32(FreeRDP_GatewayPort, c.number("gateway.port")) && setBool(FreeRDP_GatewayUseSameCredentials, same) &&
             (same || (setString(FreeRDP_GatewayUsername, c.option("gateway.user")) &&
                       setString(FreeRDP_GatewayDomain, c.option("gateway.domain")) &&
                       setString(FreeRDP_GatewayPassword, c.option("gateway.password")))) &&
             setBool(FreeRDP_GatewayHttpTransport, transport != "rpc") && setBool(FreeRDP_GatewayRpcTransport, transport != "http") &&
             freerdp_set_gateway_usage_method(st, c.option("gateway.usage") == "detect" ? TSC_PROXY_MODE_DETECT : TSC_PROXY_MODE_DIRECT);
    }
    return ok;
}

// dropped: reconnect with the cookie; client_auto_reconnect_ex() stalls closing the tab
bool RdpSession::reconnect(std::stop_token stop)
{
    const UINT32 info = freerdp_error_info(context_->instance);
    if (stop.stop_requested() || !connection_.flag("autoReconnect") ||
        (info != ERRINFO_SUCCESS && info != ERRINFO_GRAPHICS_SUBSYSTEM_FAILED))
        return false;
    listener_.onStateChanged(SessionState::Connecting);
    std::mutex mutex;
    std::condition_variable_any stopped;
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::unique_lock lock(mutex);
        if (attempt > 0 && stopped.wait_for(lock, stop, std::chrono::seconds(2), [] { return false; })) return false;
        if (stop.stop_requested()) return false;
        if (freerdp_reconnect(context_->instance)) return true;
        switch (freerdp_get_last_error(context_)) { // server said no: retrying won't help
        case FREERDP_ERROR_CONNECT_CANCELLED:
        case FREERDP_ERROR_AUTHENTICATION_FAILED:
        case FREERDP_ERROR_CONNECT_LOGON_FAILURE:
        case FREERDP_ERROR_CONNECT_WRONG_PASSWORD:
        case FREERDP_ERROR_CONNECT_ACCESS_DENIED:
            return false;
        default:
            break;
        }
    }
    return false;
}

void RdpSession::run(std::stop_token stop)
{
    listener_.onStateChanged(SessionState::Connecting);
    tasks_.drain();

    if (!applySettings()) {
        listener_.onError("Invalid connection settings.");
        listener_.onStateChanged(SessionState::Error);
        return;
    }

    freerdp* instance = context_->instance;
    bool failed = !freerdp_connect(instance);
    if (!failed) {
        connected_ = true;
        listener_.onStateChanged(SessionState::Connected);
        tasks_.drain();
        sendLayout();
        while (!stop.stop_requested() && !freerdp_shall_disconnect_context(context_)) {
            HANDLE handles[MAXIMUM_WAIT_OBJECTS];
            handles[0] = wakeEvent_;
            const DWORD count = freerdp_get_event_handles(context_, &handles[1], ARRAYSIZE(handles) - 1);
            if (count == 0 || WaitForMultipleObjects(count + 1, handles, FALSE, 100) == WAIT_FAILED ||
                !freerdp_check_event_handles(context_)) {
                connected_ = false;
                if (reconnect(stop)) {
                    connected_ = true;
                    listener_.onStateChanged(SessionState::Connected);
                    sendLayout();
                    continue;
                }
                failed = true;
                break;
            }
            ResetEvent(wakeEvent_);
            tasks_.drain();
        }
        connected_ = false;
    }

    const UINT32 error = freerdp_get_last_error(context_);
    const UINT32 info = freerdp_error_info(instance);
    freerdp_disconnect(instance);

    const bool quiet = stop.stop_requested() || cancelled_ || error == FREERDP_ERROR_CONNECT_CANCELLED ||
                       info == ERRINFO_LOGOFF_BY_USER || info == ERRINFO_RPC_INITIATED_DISCONNECT ||
                       info == ERRINFO_RPC_INITIATED_LOGOFF;
    if (failed && !quiet) {
        const char* text = freerdp_get_last_error_string(error);
        listener_.onError(text && error != FREERDP_ERROR_SUCCESS ? text : "The connection was closed.");
        listener_.onStateChanged(SessionState::Error);
    } else {
        listener_.onStateChanged(SessionState::Disconnected);
    }
}

} // namespace rdc
