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

#include "Tone3000Auth.hpp"
#include "Tone3000Downloader.hpp" // PIPEDAL_T3K_PUBLISHABLE_KEY
#include "Curl.hpp"
#include "HtmlHelper.hpp"
#include "Lv2Log.hpp"
#include "TemporaryFile.hpp"
#include "json_variant.hpp"
#include "ss.hpp"
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace pipedal;
namespace fs = std::filesystem;

static constexpr double DEFAULT_EXPIRES_IN_S = 3600;
// Bounds every TONE3000 request, so a refresh (which holds refreshMutex) or a poll can't hang.
// Close() doesn't wait for it: it kills the requests in flight (Dependencies::abort).
static constexpr int HTTP_MAX_TIME_SECONDS = 30;

static const char *DEVICE_CODE_GRANT = "urn:ietf:params:oauth:grant-type:device_code";

namespace
{
    std::optional<json_variant> ParseObject(const std::string &body)
    {
        try
        {
            json_variant v = json_variant::parse(body);
            if (v.is_object())
            {
                return v;
            }
        }
        catch (const std::exception &)
        {
        }
        return std::nullopt;
    }

    std::string GetString(const json_variant &v, const std::string &key)
    {
        if (!v.contains(key))
            return "";
        const json_variant &member = v[key];
        return member.is_string() ? member.as_string() : "";
    }

    std::optional<double> GetNumber(const json_variant &v, const std::string &key)
    {
        if (!v.contains(key))
            return std::nullopt;
        const json_variant &member = v[key];
        if (!member.is_number())
            return std::nullopt;
        return member.as_number();
    }

    bool IsSuccess(int status) { return status >= 200 && status < 300; }

    // The on-disk token file.
    class StoredTokens
    {
    public:
        std::string access_token_;
        std::string refresh_token_;
        int64_t expires_at_ = 0;
        DECLARE_JSON_MAP(StoredTokens);
    };
    JSON_MAP_BEGIN(StoredTokens)
    JSON_MAP_REFERENCE(StoredTokens, access_token)
    JSON_MAP_REFERENCE(StoredTokens, refresh_token)
    JSON_MAP_REFERENCE(StoredTokens, expires_at)
    JSON_MAP_END()

    std::string DescribeDeviceError(const std::string &error)
    {
        if (error == "expired_token")
            return "The sign-in code expired. Please try again.";
        if (error == "access_denied")
            return "Sign-in was declined on the other device.";
        return SS("TONE3000 sign-in failed (" << error << ").");
    }
}

std::optional<Tone3000DeviceAuthorization> pipedal::ParseTone3000DeviceAuthorization(const std::string &body)
{
    auto v = ParseObject(body);
    if (!v)
        return std::nullopt;
    Tone3000DeviceAuthorization d;
    d.deviceCode = GetString(*v, "device_code");
    d.userCode = GetString(*v, "user_code");
    d.verificationUri = GetString(*v, "verification_uri");
    d.verificationUriComplete = GetString(*v, "verification_uri_complete");
    if (d.deviceCode.empty() || d.userCode.empty() || d.verificationUri.empty())
        return std::nullopt;
    if (d.verificationUriComplete.empty())
        d.verificationUriComplete = d.verificationUri;
    if (auto n = GetNumber(*v, "expires_in"); n && *n > 0)
        d.expiresInS = (int64_t)*n;
    if (auto n = GetNumber(*v, "interval"); n)
        d.intervalS = std::max((int64_t)1, (int64_t)*n);
    return d;
}

std::optional<Tone3000Tokens> pipedal::ParseTone3000TokenResponse(const std::string &body, int64_t nowMs)
{
    auto v = ParseObject(body);
    if (!v)
        return std::nullopt;
    Tone3000Tokens t;
    t.accessToken = GetString(*v, "access_token");
    t.refreshToken = GetString(*v, "refresh_token");
    if (t.accessToken.empty())
        return std::nullopt;
    // expires_in is RECOMMENDED, not required (RFC 6749 5.1): assume an hour.
    double expiresIn = GetNumber(*v, "expires_in").value_or(DEFAULT_EXPIRES_IN_S);
    t.expiresAtMs = nowMs + (int64_t)(expiresIn * 1000.0);
    return t;
}

std::string pipedal::ParseTone3000OAuthError(const std::string &body)
{
    auto v = ParseObject(body);
    if (!v)
        return "";
    return GetString(*v, "error");
}

//////////////////////////////////////////////////////////////////////////////
// Tone3000DevicePoller

Tone3000DevicePoller::Tone3000DevicePoller(const Tone3000DeviceAuthorization &authorization, int64_t nowMs)
    : intervalMs(std::max((int64_t)1, authorization.intervalS) * 1000),
      deadlineMs(nowMs + authorization.expiresInS * 1000)
{
}

Tone3000DevicePoller::Result Tone3000DevicePoller::OnTokenResponse(const Tone3000HttpResponse &response, int64_t nowMs)
{
    Result result;
    if (response.status == 0)
    {
        // No answer (network blip). Keep polling until the code's own deadline.
        result.action = Action::Continue;
        return result;
    }
    if (IsSuccess(response.status))
    {
        auto tokens = ParseTone3000TokenResponse(response.body, nowMs);
        if (!tokens)
        {
            result.action = Action::Failed;
            result.error = "invalid_token_response";
            return result;
        }
        result.action = Action::Success;
        result.tokens = std::move(*tokens);
        return result;
    }
    std::string error = ParseTone3000OAuthError(response.body);
    if (error == "authorization_pending")
    {
        result.action = Action::Continue;
    }
    else if (error == "slow_down")
    {
        intervalMs += SLOW_DOWN_INCREMENT_MS; // RFC 8628 §3.5
        result.action = Action::Continue;
    }
    else if (error.empty() && response.status >= 500)
    {
        result.action = Action::Continue; // transient server trouble.
    }
    else
    {
        result.action = Action::Failed;
        result.error = error.empty() ? SS("http_" << response.status) : error;
    }
    return result;
}

//////////////////////////////////////////////////////////////////////////////
// Tone3000Auth

JSON_MAP_BEGIN(Tone3000AuthStatus)
JSON_MAP_REFERENCE(Tone3000AuthStatus, signedIn)
JSON_MAP_REFERENCE(Tone3000AuthStatus, deviceState)
JSON_MAP_REFERENCE(Tone3000AuthStatus, userCode)
JSON_MAP_REFERENCE(Tone3000AuthStatus, verificationUri)
JSON_MAP_REFERENCE(Tone3000AuthStatus, verificationUriComplete)
JSON_MAP_REFERENCE(Tone3000AuthStatus, expiresAtMs)
JSON_MAP_REFERENCE(Tone3000AuthStatus, error)
JSON_MAP_END()

JSON_MAP_BEGIN(Tone3000AccessTokenReply)
JSON_MAP_REFERENCE(Tone3000AccessTokenReply, ok)
JSON_MAP_REFERENCE(Tone3000AccessTokenReply, accessToken)
JSON_MAP_REFERENCE(Tone3000AccessTokenReply, expiresAtMs)
JSON_MAP_REFERENCE(Tone3000AccessTokenReply, error)
JSON_MAP_END()

Tone3000Auth::Dependencies Tone3000Auth::DefaultDependencies()
{
    Dependencies deps;
    // One per Tone3000Auth (each Create() gets fresh dependencies): Close() aborts its requests only.
    auto cancellation = std::make_shared<CurlCancellation>();
    deps.postForm = [cancellation](const std::string &url, const std::string &formBody)
    {
        Tone3000HttpResponse response;
        try
        {
            std::vector<std::string> headers{
                "Content-Type: application/x-www-form-urlencoded",
                "Accept: application/json"};
            response.status = CurlPostStrings(url, formBody, response.body, nullptr, &headers, HTTP_MAX_TIME_SECONDS, cancellation.get());
        }
        catch (const std::exception &e)
        {
            response.status = 0;
            response.transportError = e.what();
        }
        return response;
    };
    deps.get = [cancellation](const std::string &url, const std::vector<std::string> &headers)
    {
        Tone3000HttpResponse response;
        try
        {
            TemporaryFile output{GetCurlTempDirectory()};
            response.status = CurlGet(url, output.Path(), nullptr, &headers, HTTP_MAX_TIME_SECONDS, cancellation.get());
            std::ifstream f(output.Path(), std::ios::binary);
            if (f)
            {
                response.body.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            }
        }
        catch (const std::exception &e)
        {
            response.status = 0;
            response.transportError = e.what();
        }
        return response;
    };
    deps.request = [cancellation](const std::string &method, const std::string &url, const std::vector<std::string> &headers)
    {
        Tone3000HttpResponse response;
        try
        {
            TemporaryFile output{GetCurlTempDirectory()};
            response.status = CurlRequest(method, url, output.Path(), nullptr, &headers, HTTP_MAX_TIME_SECONDS, cancellation.get());
            std::ifstream f(output.Path(), std::ios::binary);
            if (f)
            {
                response.body.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            }
        }
        catch (const std::exception &e)
        {
            response.status = 0;
            response.transportError = e.what();
        }
        return response;
    };
    deps.abort = [cancellation]()
    { cancellation->Cancel(); };
    deps.nowMs = []()
    {
        return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
    return deps;
}

std::shared_ptr<Tone3000Auth> Tone3000Auth::Create(
    const fs::path &tokenFile,
    Dependencies dependencies,
    const std::string &origin,
    const std::string &clientId)
{
    return std::shared_ptr<Tone3000Auth>(new Tone3000Auth(tokenFile, std::move(dependencies), origin, clientId));
}

Tone3000Auth::Tone3000Auth(const fs::path &tokenFile, Dependencies dependencies,
                           const std::string &origin, const std::string &clientId)
    : tokenFile(tokenFile),
      deps(std::move(dependencies)),
      origin(origin),
      clientId(clientId.empty() ? std::string(PIPEDAL_T3K_PUBLISHABLE_KEY) : clientId)
{
    std::lock_guard<std::mutex> lock(mutex);
    this->tokens = LoadTokens();
    status.signedIn_ = this->tokens.has_value();
}

Tone3000Auth::~Tone3000Auth()
{
}

void Tone3000Auth::SetStatusListener(StatusListener listener)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (closed)
    {
        return; // Close() promised the listener's owner no further calls.
    }
    this->listener = std::move(listener);
}

Tone3000AuthStatus Tone3000Auth::GetStatus()
{
    std::lock_guard<std::mutex> lock(mutex);
    return status;
}

bool Tone3000Auth::IsSignedIn()
{
    std::lock_guard<std::mutex> lock(mutex);
    return tokens.has_value();
}

void Tone3000Auth::Publish()
{
    StatusListener l;
    Tone3000AuthStatus s;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!listener)
        {
            return;
        }
        l = listener;
        s = status;
        ++activeListenerCalls; // Close() waits for this call before the listener's owner goes away.
    }
    struct CallDone
    {
        Tone3000Auth *self;
        ~CallDone()
        {
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                --self->activeListenerCalls;
            }
            self->cv.notify_all();
        }
    } callDone{this};
    l(s);
}

Tone3000AccessTokenRequest pipedal::ReadTone3000AccessTokenRequest(json_reader &reader)
{
    json_variant body;
    reader.read(&body);
    Tone3000AccessTokenRequest result;
    if (body.is_bool())
    {
        result.forceRefresh = body.as_bool();
        return result;
    }
    if (!body.is_object())
    {
        throw std::invalid_argument("Invalid t3kAuthGetAccessToken request.");
    }
    if (body.contains("forceRefresh"))
    {
        const json_variant &value = body["forceRefresh"];
        if (!value.is_bool())
        {
            throw std::invalid_argument("Invalid t3kAuthGetAccessToken request.");
        }
        result.forceRefresh = value.as_bool();
    }
    if (body.contains("rejectedAccessToken"))
    {
        const json_variant &value = body["rejectedAccessToken"];
        if (!value.is_string())
        {
            throw std::invalid_argument("Invalid t3kAuthGetAccessToken request.");
        }
        result.rejectedAccessToken = value.as_string();
    }
    return result;
}

bool Tone3000Auth::IsCurrent(uint64_t generation)
{
    std::lock_guard<std::mutex> lock(mutex);
    return !closed && generation == flowGeneration;
}

bool Tone3000Auth::PublishDevice(uint64_t generation, const std::function<void(Tone3000AuthStatus &)> &update)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed || generation != flowGeneration)
        {
            return false;
        }
        update(status);
    }
    Publish();
    return true;
}

bool Tone3000Auth::Wait(std::chrono::milliseconds duration, uint64_t generation)
{
    if (deps.wait)
    {
        if (!deps.wait(duration))
            return false;
        return IsCurrent(generation);
    }
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_for(lock, duration, [this, generation]()
                { return closed || generation != flowGeneration; });
    return !closed && generation == flowGeneration;
}

void Tone3000Auth::StartDeviceFlow(uint64_t ownerId)
{
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed)
            return;
        generation = ++flowGeneration;
        flowOwner = ownerId;
        cv.notify_all(); // wake (and so stop) any older flow.
        status.deviceState_ = Tone3000AuthStatus::REQUESTING;
        status.userCode_.clear();
        status.verificationUri_.clear();
        status.verificationUriComplete_.clear();
        status.expiresAtMs_ = 0;
        status.error_.clear();
        ++activeFlowThreads;
    }
    Publish();

    // The thread owns a reference, so it may outlive Close() while curl finishes; it publishes nothing once stale.
    auto self = shared_from_this();
    std::thread([self, generation]()
                {
        try {
            self->RunFlow(generation);
        } catch (const std::exception &e) {
            std::string message = e.what();
            self->PublishDevice(generation, [&message](Tone3000AuthStatus &s) {
                s.deviceState_ = Tone3000AuthStatus::FAILED;
                s.error_ = message;
            });
        }
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            --self->activeFlowThreads;
        }
        self->cv.notify_all(); })
        .detach();
}

void Tone3000Auth::RunDeviceFlow()
{
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed)
            return;
        generation = ++flowGeneration;
        flowOwner = 0;
        cv.notify_all();
        status.deviceState_ = Tone3000AuthStatus::REQUESTING;
        status.error_.clear();
    }
    Publish();
    RunFlow(generation);
}

void Tone3000Auth::RunFlow(uint64_t generation)
{
    std::string form = HtmlFormBuilder({{"client_id", clientId}}).build();
    Tone3000HttpResponse response = deps.postForm(origin + "/api/v1/oauth/device_authorization", form);
    if (!IsCurrent(generation))
        return;

    if (response.status == 0)
    {
        std::string message = SS("Unable to reach the TONE3000 server. " << response.transportError);
        PublishDevice(generation, [&message](Tone3000AuthStatus &s)
                      {
            s.deviceState_ = Tone3000AuthStatus::FAILED;
            s.error_ = message; });
        return;
    }
    std::optional<Tone3000DeviceAuthorization> authorization;
    if (IsSuccess(response.status))
    {
        authorization = ParseTone3000DeviceAuthorization(response.body);
    }
    if (!authorization)
    {
        // The endpoint refused (e.g. device flow not enabled for this client id): the UI falls back to the popup sign-in.
        std::string error = ParseTone3000OAuthError(response.body);
        std::string message = SS("Sign-in with a phone is not available ("
                                 << (error.empty() ? SS("HTTP " << response.status) : error) << ").");
        PublishDevice(generation, [&message](Tone3000AuthStatus &s)
                      {
            s.deviceState_ = Tone3000AuthStatus::UNAVAILABLE;
            s.error_ = message; });
        return;
    }

    Tone3000DevicePoller poller(*authorization, deps.nowMs());
    int64_t expiresAtMs = deps.nowMs() + authorization->expiresInS * 1000;
    if (!PublishDevice(generation, [&](Tone3000AuthStatus &s)
                       {
            s.deviceState_ = Tone3000AuthStatus::WAITING;
            s.userCode_ = authorization->userCode;
            s.verificationUri_ = authorization->verificationUri;
            s.verificationUriComplete_ = authorization->verificationUriComplete;
            s.expiresAtMs_ = expiresAtMs;
            s.error_.clear(); }))
    {
        return;
    }

    std::string pollForm = HtmlFormBuilder({{"grant_type", DEVICE_CODE_GRANT},
                                            {"device_code", authorization->deviceCode},
                                            {"client_id", clientId}})
                               .build();
    while (true)
    {
        if (!Wait(std::chrono::milliseconds(poller.IntervalMs()), generation))
            return; // cancelled or closed.
        if (poller.Expired(deps.nowMs()))
        {
            std::string message = DescribeDeviceError("expired_token");
            PublishDevice(generation, [&message](Tone3000AuthStatus &s)
                          {
                s.deviceState_ = Tone3000AuthStatus::FAILED;
                s.error_ = message; });
            return;
        }
        Tone3000HttpResponse pollResponse = deps.postForm(origin + "/api/v1/oauth/token", pollForm);
        if (!IsCurrent(generation))
            return;
        auto result = poller.OnTokenResponse(pollResponse, deps.nowMs());
        switch (result.action)
        {
        case Tone3000DevicePoller::Action::Continue:
            break;
        case Tone3000DevicePoller::Action::Success:
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (closed || generation != flowGeneration)
                    return;
                SaveTokens(result.tokens);
                tokens = result.tokens;
                lastRefreshAtMs = 0; // a new session.
                status.signedIn_ = true;
                status.deviceState_ = Tone3000AuthStatus::SUCCEEDED;
                status.userCode_.clear();
                status.verificationUri_.clear();
                status.verificationUriComplete_.clear();
                status.error_.clear();
            }
            Publish();
            return;
        }
        case Tone3000DevicePoller::Action::Failed:
        {
            std::string message = DescribeDeviceError(result.error);
            PublishDevice(generation, [&message](Tone3000AuthStatus &s)
                          {
                s.deviceState_ = Tone3000AuthStatus::FAILED;
                s.userCode_.clear();
                s.verificationUri_.clear();
                s.verificationUriComplete_.clear();
                s.error_ = message; });
            return;
        }
        }
    }
}

bool Tone3000Auth::CancelDeviceFlow(uint64_t ownerId)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (status.deviceState_ != Tone3000AuthStatus::REQUESTING && status.deviceState_ != Tone3000AuthStatus::WAITING)
        {
            return false; // nothing in progress (keep e.g. a FAILED status and its error).
        }
        if (ownerId != 0 && flowOwner != 0 && ownerId != flowOwner)
        {
            return false; // someone else's sign-in (an orphaned one, owner 0, may be cancelled by anyone).
        }
        ++flowGeneration;
        flowOwner = 0;
        status.deviceState_ = Tone3000AuthStatus::IDLE;
        status.userCode_.clear();
        status.verificationUri_.clear();
        status.verificationUriComplete_.clear();
        status.expiresAtMs_ = 0;
        status.error_.clear();
    }
    cv.notify_all();
    Publish();
    return true;
}

void Tone3000Auth::SignOut()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++flowGeneration;
        flowOwner = 0;
        DeleteTokens();
        tokens.reset();
        lastRefreshAtMs = 0;
        status = Tone3000AuthStatus(); // signed out, device state idle, code and URIs cleared.
    }
    cv.notify_all();
    Publish();
}

void Tone3000Auth::ReleaseDeviceFlowOwner(uint64_t ownerId)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (ownerId != 0 && flowOwner == ownerId)
    {
        flowOwner = 0;
    }
}

bool Tone3000Auth::Close()
{
    std::unique_lock<std::mutex> lock(mutex);
    closed = true;
    ++flowGeneration;
    listener = nullptr;
    cv.notify_all();
    if (deps.abort)
    {
        // Kill the requests in flight (they fail at once) and fail any later ones, so that the
        // threads waiting on them finish well within CLOSE_TIMEOUT.
        deps.abort();
    }
    // A poll thread sleeping on cv exits at once. One still stuck (injected I/O without an
    // abort) is left detached after the timeout; it holds its own reference, and publishes
    // nothing (the listener is gone, and SetStatusListener is ignored from now on). A tracked
    // operation still running then (a websocket reply thread) only holds the socket handler,
    // which the model has already closed (its FinalCleanup has run), so it no longer reaches
    // the model either.
    bool idle = cv.wait_for(lock, CLOSE_TIMEOUT, [this]()
                            { return activeFlowThreads == 0 && activeOperations == 0 && activeListenerCalls == 0; });
    if (!idle)
    {
        Lv2Log::warning(SS("TONE3000: closing with " << activeFlowThreads << " sign-in thread(s), "
                                                     << activeOperations << " request(s) and "
                                                     << activeListenerCalls << " status update(s) still running."));
    }
    return idle;
}

Tone3000Auth::OperationToken Tone3000Auth::BeginOperation()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed)
        {
            throw std::runtime_error("TONE3000 sign-in is not available.");
        }
        ++activeOperations;
    }
    auto self = shared_from_this();
    return OperationToken(nullptr, [self](void *)
                          {
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            --self->activeOperations;
        }
        self->cv.notify_all(); });
}

bool Tone3000Auth::WaitForOperations(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, timeout, [this]()
                       { return activeOperations == 0; });
}

bool Tone3000Auth::WaitForDeviceFlowThreads(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, timeout, [this]()
                       { return activeFlowThreads == 0; });
}

std::string Tone3000Auth::GetAccessToken(bool forceRefresh, const std::string &rejectedAccessToken)
{
    return GetTokens(forceRefresh, rejectedAccessToken).accessToken;
}

Tone3000Tokens Tone3000Auth::GetTokens(bool forceRefresh, const std::string &rejectedAccessToken)
{
    std::lock_guard<std::mutex> refreshLock(refreshMutex);
    std::optional<Tone3000Tokens> current;
    {
        std::lock_guard<std::mutex> lock(mutex);
        current = tokens;
    }
    if (!current)
    {
        throw std::runtime_error("Not signed in to TONE3000.");
    }
    int64_t nowMs = deps.nowMs();
    if (forceRefresh)
    {
        // Coalesce: forced refreshes queued behind the one that just ran (refreshMutex) get its tokens,
        // unless the caller's 401 was for the current token itself.
        std::lock_guard<std::mutex> lock(mutex);
        if (lastRefreshAtMs != 0 && nowMs - lastRefreshAtMs < FORCED_REFRESH_COALESCE_MS &&
            (rejectedAccessToken.empty() || current->accessToken != rejectedAccessToken))
        {
            forceRefresh = false;
        }
    }
    if (!forceRefresh && nowMs < current->expiresAtMs - REFRESH_MARGIN_MS)
    {
        return *current;
    }
    return Refresh(*current);
}

Tone3000Tokens Tone3000Auth::Refresh(const Tone3000Tokens &current)
{
    // Sign out only if the session we tried to refresh is still the stored one: a sign-out
    // or a new sign-in while the request was in flight wins.
    auto signOut = [this, &current]()
    {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (tokens && tokens->refreshToken == current.refreshToken)
            {
                DeleteTokens();
                tokens.reset();
                lastRefreshAtMs = 0;
                status.signedIn_ = false;
                if (status.deviceState_ != Tone3000AuthStatus::REQUESTING && status.deviceState_ != Tone3000AuthStatus::WAITING)
                {
                    // Drop the stale result of the sign-in that created this session; leave a sign-in in progress alone.
                    status.deviceState_ = Tone3000AuthStatus::IDLE;
                    status.userCode_.clear();
                    status.verificationUri_.clear();
                    status.verificationUriComplete_.clear();
                    status.expiresAtMs_ = 0;
                    status.error_.clear();
                }
                changed = true;
            }
        }
        if (changed)
        {
            Publish();
        }
        throw std::runtime_error("Your TONE3000 session has expired. Please sign in again.");
    };
    if (current.refreshToken.empty())
    {
        signOut();
    }
    std::string form = HtmlFormBuilder({{"grant_type", "refresh_token"},
                                        {"refresh_token", current.refreshToken},
                                        {"client_id", clientId}})
                           .build();
    Tone3000HttpResponse response = deps.postForm(origin + "/api/v1/oauth/token", form);
    if (response.status == 0)
    {
        throw std::runtime_error(SS("Unable to reach the TONE3000 server. " << response.transportError));
    }
    if (IsSuccess(response.status))
    {
        auto refreshed = ParseTone3000TokenResponse(response.body, deps.nowMs());
        if (!refreshed)
        {
            throw std::runtime_error("TONE3000 token refresh failed: invalid response.");
        }
        if (refreshed->refreshToken.empty())
        {
            refreshed->refreshToken = current.refreshToken; // not rotated.
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!tokens) // signed out while refreshing.
            {
                throw std::runtime_error("Not signed in to TONE3000.");
            }
            if (tokens->refreshToken != current.refreshToken)
            {
                // A newer session (sign-in while we were refreshing): keep it, discard ours.
                return *tokens;
            }
            SaveTokens(*refreshed);
            tokens = *refreshed;
            lastRefreshAtMs = deps.nowMs();
        }
        return *refreshed;
    }
    if (response.status == 401 || ParseTone3000OAuthError(response.body) == "invalid_grant")
    {
        signOut(); // refresh token rejected.
    }
    throw std::runtime_error(SS("TONE3000 token refresh failed (HTTP " << response.status << ")."));
}

namespace
{
    std::string ToLower(std::string s)
    {
        for (char &c : s)
        {
            c = (char)std::tolower((unsigned char)c);
        }
        return s;
    }
}

std::optional<Tone3000UrlOrigin> pipedal::ParseTone3000UrlOrigin(const std::string &url)
{
    // Deliberately strict: anything a URL parser (curl's, or a browser's) might read differently
    // is refused rather than interpreted.
    for (char c : url)
    {
        unsigned char uc = (unsigned char)c;
        if (uc <= 0x20 || uc >= 0x7F || c == '\\')
            return std::nullopt;
    }
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos)
        return std::nullopt;
    Tone3000UrlOrigin result;
    result.scheme = ToLower(url.substr(0, schemeEnd));
    int defaultPort;
    if (result.scheme == "https")
        defaultPort = 443;
    else if (result.scheme == "http")
        defaultPort = 80;
    else
        return std::nullopt;

    size_t authorityStart = schemeEnd + 3;
    size_t authorityEnd = url.find_first_of("/?#", authorityStart);
    std::string authority = url.substr(authorityStart, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - authorityStart);
    if (authority.find('@') != std::string::npos)
        return std::nullopt; // user info: "https://www.tone3000.com@evil.example/".

    std::string host = authority;
    std::string port;
    if (size_t colon = authority.rfind(':'); colon != std::string::npos)
    {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    if (host.empty())
        return std::nullopt;
    for (char c : host)
    {
        // Host names and dotted IPv4 addresses only: no IPv6 literals, no percent-encoding.
        if (!std::isalnum((unsigned char)c) && c != '-' && c != '.')
            return std::nullopt;
    }
    if (host.front() == '.' || host.back() == '.' || host.find("..") != std::string::npos)
        return std::nullopt;
    result.host = ToLower(host);
    result.port = defaultPort;
    if (authority.find(':') != std::string::npos)
    {
        if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos)
            return std::nullopt;
        result.port = std::stoi(port);
        if (result.port < 1 || result.port > 65535)
            return std::nullopt;
    }
    return result;
}

bool pipedal::IsTone3000SameOrigin(const std::string &url, const std::string &origin)
{
    auto a = ParseTone3000UrlOrigin(url);
    auto b = ParseTone3000UrlOrigin(origin);
    return a && b && a->scheme == b->scheme && a->host == b->host && a->port == b->port;
}

bool Tone3000Auth::IsOriginUrl(const std::string &url) const
{
    // Only the TONE3000 API origin ever sees the bearer token.
    return IsTone3000SameOrigin(url, origin);
}

std::string Tone3000Auth::CheckedOriginUrl(const std::string &url) const
{
    std::string fullUrl = url.starts_with("/") ? origin + url : url;
    if (!IsOriginUrl(fullUrl))
    {
        throw std::runtime_error(SS("Refusing to send TONE3000 credentials to " << fullUrl));
    }
    return fullUrl;
}

Tone3000HttpResponse Tone3000Auth::Send(const std::string &method, const std::string &url, const std::vector<std::string> &headers)
{
    if (method == "GET")
    {
        return deps.get(url, headers);
    }
    if (method != "PUT" && method != "DELETE")
    {
        throw std::invalid_argument(SS("Unsupported HTTP method: " << method));
    }
    if (!deps.request)
    {
        throw std::runtime_error(SS("HTTP " << method << " is not available."));
    }
    std::vector<std::string> allHeaders = headers;
    // An empty body: some front ends want a length on PUT.
    allHeaders.push_back("Content-Length: 0");
    return deps.request(method, url, allHeaders);
}

Tone3000HttpResponse Tone3000Auth::AuthorizedGet(const std::string &url)
{
    return AuthorizedRequest("GET", url);
}

Tone3000HttpResponse Tone3000Auth::AuthorizedRequest(const std::string &method, const std::string &url)
{
    if (method != "GET" && method != "PUT" && method != "DELETE")
    {
        throw std::invalid_argument(SS("Unsupported HTTP method: " << method));
    }
    std::string fullUrl = CheckedOriginUrl(url);
    std::string token = GetAccessToken();
    Tone3000HttpResponse response = Send(method, fullUrl, {SS("Authorization: Bearer " << token)});
    if (response.status == 401)
    {
        token = GetAccessToken(true, token);
        response = Send(method, fullUrl, {SS("Authorization: Bearer " << token)});
    }
    return response;
}

Tone3000HttpResponse Tone3000Auth::AnonymousGet(const std::string &url)
{
    return deps.get(CheckedOriginUrl(url), {});
}

//////////////////////////////////////////////////////////////////////////////
// Token file. Called with `mutex` held.

std::optional<Tone3000Tokens> Tone3000Auth::LoadTokens()
{
    std::error_code ec;
    if (!fs::exists(tokenFile, ec))
    {
        return std::nullopt;
    }
    try
    {
        // Installers may have widened the permissions; the file holds a refresh token.
        struct stat st;
        if (stat(tokenFile.c_str(), &st) == 0 && (st.st_mode & 077) != 0)
        {
            chmod(tokenFile.c_str(), 0600);
        }
        std::ifstream f(tokenFile);
        if (!f)
        {
            return std::nullopt;
        }
        json_reader reader(f);
        StoredTokens stored;
        reader.read(&stored);
        if (stored.access_token_.empty())
        {
            return std::nullopt;
        }
        return Tone3000Tokens{stored.access_token_, stored.refresh_token_, stored.expires_at_};
    }
    catch (const std::exception &e)
    {
        Lv2Log::warning(SS("Ignoring unreadable TONE3000 token file " << tokenFile << ": " << e.what()));
        return std::nullopt;
    }
}

void Tone3000Auth::SaveTokens(const Tone3000Tokens &t)
{
    StoredTokens stored;
    stored.access_token_ = t.accessToken;
    stored.refresh_token_ = t.refreshToken;
    stored.expires_at_ = t.expiresAtMs;
    std::stringstream ss;
    {
        json_writer writer(ss);
        writer.write(&stored);
    }
    std::string text = ss.str();

    std::error_code ec;
    fs::create_directories(tokenFile.parent_path(), ec);
    fs::path tempPath = tokenFile;
    tempPath += ".tmp";

    int fd = open(tempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
    {
        throw std::runtime_error(SS("Unable to save TONE3000 sign-in: can't write " << tempPath));
    }
    fchmod(fd, 0600); // in case the file already existed with other permissions.
    const char *p = text.data();
    size_t remaining = text.size();
    bool ok = true;
    while (remaining > 0)
    {
        ssize_t n = write(fd, p, remaining);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        p += n;
        remaining -= (size_t)n;
    }
    if (ok && fsync(fd) != 0)
        ok = false;
    close(fd);
    if (!ok || rename(tempPath.c_str(), tokenFile.c_str()) != 0)
    {
        fs::remove(tempPath, ec);
        throw std::runtime_error(SS("Unable to save TONE3000 sign-in to " << tokenFile));
    }
}

void Tone3000Auth::DeleteTokens()
{
    std::error_code ec;
    fs::remove(tokenFile, ec);
}
