/*
 *   Copyright (c) Robin E.R. Davies
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#pragma once

// TONE3000 account session held by the daemon.
//
// Sign-in uses the OAuth 2.0 device authorization grant (RFC 8628): the daemon asks
// TONE3000 for a device code, the web UI shows verification_uri_complete as a QR code
// plus the user code, the user approves on a phone, and a background thread polls the
// token endpoint until it answers. Tokens are stored in a 0600 file in the daemon's
// private storage, refreshed 60 s before expiry and once after a 401.
//
// Web clients that still download in the browser ask for a short-lived access token over
// the websocket (t3kAuthGetAccessToken); the refresh token never leaves the daemon.
//
// Nothing here runs on the audio thread. GetAccessToken()/AuthorizedGet() block on the
// network, so call them from a background thread.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "json.hpp"

namespace pipedal
{
    struct Tone3000HttpResponse
    {
        int status = 0;             // HTTP status; 0 = the request never got an answer.
        std::string body;
        std::string transportError; // why, when status == 0.
    };

    struct Tone3000Tokens
    {
        std::string accessToken;
        std::string refreshToken;
        int64_t expiresAtMs = 0; // unix epoch, ms.
    };

    struct Tone3000DeviceAuthorization
    {
        std::string deviceCode;
        std::string userCode;
        std::string verificationUri;
        std::string verificationUriComplete; // falls back to verificationUri.
        int64_t expiresInS = 900;
        int64_t intervalS = 5;
    };

    // Pure parsers (exposed for tests). Return nullopt on malformed / incomplete input.
    std::optional<Tone3000DeviceAuthorization> ParseTone3000DeviceAuthorization(const std::string &body);
    std::optional<Tone3000Tokens> ParseTone3000TokenResponse(const std::string &body, int64_t nowMs);
    // The OAuth "error" member of a response body, or "" if there is none.
    std::string ParseTone3000OAuthError(const std::string &body);

    // scheme://host[:port] of an absolute http(s) URL, lower-cased, with the default port filled in.
    struct Tone3000UrlOrigin
    {
        std::string scheme;
        std::string host;
        int port = 0;
    };
    // nullopt for anything but a plain http(s) URL whose host is a DNS name or dotted IPv4 address
    // (user info, IPv6 literals, percent-encoded or odd characters in the host, whitespace or
    // backslashes anywhere: refused).
    std::optional<Tone3000UrlOrigin> ParseTone3000UrlOrigin(const std::string &url);
    // Whether `url` is on `origin` (same scheme, host and port).
    bool IsTone3000SameOrigin(const std::string &url, const std::string &origin);

    // The RFC 8628 polling state machine, without I/O.
    class Tone3000DevicePoller
    {
    public:
        static constexpr int64_t SLOW_DOWN_INCREMENT_MS = 5000;

        enum class Action
        {
            Continue, // wait IntervalMs() and poll again.
            Success,
            Failed,
        };
        struct Result
        {
            Action action = Action::Continue;
            Tone3000Tokens tokens;
            std::string error; // OAuth error code: expired_token, access_denied, ...
        };

        Tone3000DevicePoller(const Tone3000DeviceAuthorization &authorization, int64_t nowMs);

        int64_t IntervalMs() const { return intervalMs; }
        bool Expired(int64_t nowMs) const { return nowMs >= deadlineMs; }
        Result OnTokenResponse(const Tone3000HttpResponse &response, int64_t nowMs);

    private:
        int64_t intervalMs;
        int64_t deadlineMs;
    };

    // Status pushed to web clients (message "onT3kAuthStatusChanged").
    class Tone3000AuthStatus
    {
    public:
        // deviceState values.
        static constexpr const char *IDLE = "idle";
        static constexpr const char *REQUESTING = "requesting";
        static constexpr const char *WAITING = "waiting";       // showing the QR code; polling.
        static constexpr const char *SUCCEEDED = "succeeded";
        static constexpr const char *FAILED = "failed";         // expired / denied / network: retry offered.
        static constexpr const char *UNAVAILABLE = "unavailable"; // device endpoint refused: use the popup sign-in.

        bool signedIn_ = false;
        std::string deviceState_ = IDLE;
        std::string userCode_;
        std::string verificationUri_;
        std::string verificationUriComplete_;
        int64_t expiresAtMs_ = 0; // when the user code expires.
        std::string error_;       // human-readable, for FAILED / UNAVAILABLE.

        DECLARE_JSON_MAP(Tone3000AuthStatus);
    };

    // Body of t3kAuthGetAccessToken: either a bare bool (forceRefresh; older clients) or
    // {"forceRefresh": bool, "rejectedAccessToken": string}.
    struct Tone3000AccessTokenRequest
    {
        bool forceRefresh = false;
        std::string rejectedAccessToken; // the token a 401 was received for ("" = unknown).
    };
    // Throws on anything else.
    Tone3000AccessTokenRequest ReadTone3000AccessTokenRequest(json_reader &reader);

    // Reply to t3kAuthGetAccessToken.
    class Tone3000AccessTokenReply
    {
    public:
        bool ok_ = false;
        std::string accessToken_;
        int64_t expiresAtMs_ = 0;
        std::string error_;
        DECLARE_JSON_MAP(Tone3000AccessTokenReply);
    };

    class Tone3000Auth : public std::enable_shared_from_this<Tone3000Auth>
    {
    public:
        static constexpr const char *DEFAULT_ORIGIN = "https://www.tone3000.com";
        static constexpr int64_t REFRESH_MARGIN_MS = 60 * 1000;
        // A forced refresh this soon after the last successful one returns that one's tokens
        // instead (several clients retrying after the same 401 cause one refresh, not a burst).
        static constexpr int64_t FORCED_REFRESH_COALESCE_MS = 5 * 1000;
        // How long Close() waits for background threads (device flow, tracked operations,
        // in-flight listener calls) before giving up.
        static constexpr std::chrono::milliseconds CLOSE_TIMEOUT{2000};

        // Injected I/O so tests can drive everything without the network or a real clock.
        struct Dependencies
        {
            // POST application/x-www-form-urlencoded.
            std::function<Tone3000HttpResponse(const std::string &url, const std::string &formBody)> postForm;
            // GET with extra request headers ("Name: value").
            std::function<Tone3000HttpResponse(const std::string &url, const std::vector<std::string> &headers)> get;
            // A body-less PUT or DELETE with extra request headers. May be empty (then only GETs work).
            std::function<Tone3000HttpResponse(const std::string &method, const std::string &url, const std::vector<std::string> &headers)> request;
            // Unix epoch, ms.
            std::function<int64_t()> nowMs;
            // Sleep for the poll interval. Return false to stop polling. Empty = an internal
            // wait that wakes early on cancel/close.
            std::function<bool(std::chrono::milliseconds)> wait;
            // Called by Close(): make the requests in flight return at once, and later ones fail.
            // May be empty.
            std::function<void()> abort;
        };
        // curl (each call gets its own CurlCancellation, which Close() fires) + system clock.
        static Dependencies DefaultDependencies();

        using StatusListener = std::function<void(const Tone3000AuthStatus &status)>;

        static std::shared_ptr<Tone3000Auth> Create(
            const std::filesystem::path &tokenFile,
            Dependencies dependencies = DefaultDependencies(),
            const std::string &origin = DEFAULT_ORIGIN,
            const std::string &clientId = "");                 // "" = PiPedal's publishable key.

        ~Tone3000Auth();

        void SetStatusListener(StatusListener listener);
        Tone3000AuthStatus GetStatus();
        bool IsSignedIn();

        // Device flow. StartDeviceFlow returns immediately; progress arrives via the status listener.
        // `ownerId` identifies the client that started the flow (0 = anonymous). CancelDeviceFlow
        // only acts on a flow in progress (REQUESTING or WAITING); otherwise it changes nothing and
        // returns false. With a non-zero ownerId it only cancels a flow that client started (so one
        // client closing its sign-in dialog doesn't cancel another client's sign-in), or an orphaned
        // flow (owner 0); ownerId 0 cancels any flow in progress. Returns whether it cancelled.
        void StartDeviceFlow(uint64_t ownerId = 0);
        bool CancelDeviceFlow(uint64_t ownerId = 0);
        // The client `ownerId` went away (socket closed). Its flow keeps running (a phone browser
        // drops the socket while the user enters the code elsewhere), but is orphaned: any client
        // may then cancel it. A reconnecting client has a new id.
        void ReleaseDeviceFlowOwner(uint64_t ownerId);
        // Runs one complete device flow on the calling thread (used by StartDeviceFlow's thread, and by tests).
        void RunDeviceFlow();

        void SignOut();

        // A valid access token: the stored one, or a refreshed one when within 60 s of expiry
        // (or when forceRefresh). Throws std::runtime_error when not signed in or refresh fails.
        // A rejected refresh token signs the session out.
        // A forced refresh within FORCED_REFRESH_COALESCE_MS of the last refresh returns the current
        // token instead, unless it is `rejectedAccessToken` (the token the server just answered 401 to).
        std::string GetAccessToken(bool forceRefresh = false, const std::string &rejectedAccessToken = "");
        Tone3000Tokens GetTokens(bool forceRefresh = false, const std::string &rejectedAccessToken = "");

        // Authenticated GET (url absolute, or a path starting with "/api/..." relative to origin).
        // Only URLs on the TONE3000 origin are accepted; anything else throws.
        // Retries once with a refreshed token after a 401.
        Tone3000HttpResponse AuthorizedGet(const std::string &url);
        // As AuthorizedGet, for a body-less "GET", "PUT" or "DELETE" (anything else throws).
        Tone3000HttpResponse AuthorizedRequest(const std::string &method, const std::string &url);
        // GET without credentials (public endpoints). Same origin restriction.
        Tone3000HttpResponse AnonymousGet(const std::string &url);

        const std::string &Origin() const { return origin; }

        // A background operation (a thread started by a caller that uses this object and whose
        // completion may reach the owner, e.g. a websocket reply). Close() waits (bounded) until
        // every operation token has been released. Throws std::runtime_error after Close().
        // Release the token last, after dropping anything that refers to the owner.
        using OperationToken = std::shared_ptr<void>;
        OperationToken BeginOperation();

        // Stop polling, abort the requests in flight (Dependencies::abort) and drop the listener.
        // Waits (up to CLOSE_TIMEOUT) for the poll thread, for tracked operations, and for
        // listener calls in progress, so that once it returns the listener's owner is not called
        // again; never blocks on curl for long.
        // Returns false if it timed out (an operation stuck in curl).
        bool Close();
        // Test support: wait until no device-flow thread is running.
        bool WaitForDeviceFlowThreads(std::chrono::milliseconds timeout);
        // Test support: wait until no tracked operation is running.
        bool WaitForOperations(std::chrono::milliseconds timeout);

    private:
        Tone3000Auth(const std::filesystem::path &tokenFile, Dependencies dependencies,
                     const std::string &origin, const std::string &clientId);

        std::optional<Tone3000Tokens> LoadTokens();
        void SaveTokens(const Tone3000Tokens &tokens);
        void DeleteTokens();

        Tone3000Tokens Refresh(const Tone3000Tokens &tokens);
        bool IsOriginUrl(const std::string &url) const;
        std::string CheckedOriginUrl(const std::string &url) const;
        Tone3000HttpResponse Send(const std::string &method, const std::string &url, const std::vector<std::string> &headers);
        void RunFlow(uint64_t generation);

        bool Wait(std::chrono::milliseconds duration, uint64_t generation);
        bool IsCurrent(uint64_t generation);
        // Update the device-flow status if `generation` is still the current flow.
        bool PublishDevice(uint64_t generation, const std::function<void(Tone3000AuthStatus &)> &update);
        void Publish();

        std::filesystem::path tokenFile;
        Dependencies deps;
        std::string origin;
        std::string clientId;

        std::mutex mutex; // guards everything below, and the token file.
        std::condition_variable cv;
        std::optional<Tone3000Tokens> tokens;
        Tone3000AuthStatus status;
        StatusListener listener;
        uint64_t flowGeneration = 0; // bumping it cancels the flow in flight.
        uint64_t flowOwner = 0;      // ownerId passed to the StartDeviceFlow that started it.
        int activeFlowThreads = 0;
        int activeOperations = 0;    // BeginOperation() tokens outstanding.
        int activeListenerCalls = 0; // Publish() calls running the listener outside the lock.
        int64_t lastRefreshAtMs = 0; // last successful Refresh() of the current session; 0 = none.
        bool closed = false;

        std::mutex refreshMutex; // one refresh at a time.
    };
}
