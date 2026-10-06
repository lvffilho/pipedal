// Copyright (c) 2026 Robin Davies
// MIT license; see other source files in this project.
//
// TONE3000 device-code sign-in: parsing, the RFC 8628 polling state machine and token
// refresh, all driven by a mocked HTTP function and a fake clock. No network access.

#include "pch.h"
#include "catch.hpp"
#include "Tone3000Auth.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <set>
#include "TemporaryFile.hpp"
#include <sys/stat.h>
#include <unistd.h>

using namespace pipedal;
namespace fs = std::filesystem;

namespace
{
    const char *ORIGIN = "https://t3k.test";

    struct Request
    {
        std::string method;
        std::string url;
        std::string body; // form body, or the Authorization header for GETs.
        int64_t atMs;
    };

    // Scripted HTTP + fake clock.
    struct FakeServer
    {
        int64_t now = 1'000'000'000'000;
        std::deque<Tone3000HttpResponse> posts;
        std::deque<Tone3000HttpResponse> gets;
        std::vector<Request> requests;
        std::vector<int64_t> waits;
        bool cancelOnWait = false;
        std::function<void()> beforeNextPost; // runs (once) inside the next POST.

        Tone3000Auth::Dependencies Deps()
        {
            Tone3000Auth::Dependencies d;
            d.postForm = [this](const std::string &url, const std::string &body)
            {
                requests.push_back({"POST", url, body, now});
                if (beforeNextPost)
                {
                    auto hook = std::move(beforeNextPost);
                    beforeNextPost = nullptr;
                    hook();
                }
                REQUIRE(!posts.empty());
                auto r = posts.front();
                posts.pop_front();
                return r;
            };
            d.get = [this](const std::string &url, const std::vector<std::string> &headers)
            {
                requests.push_back({"GET", url, headers.empty() ? "" : headers[0], now});
                REQUIRE(!gets.empty());
                auto r = gets.front();
                gets.pop_front();
                return r;
            };
            d.nowMs = [this]()
            { return now; };
            d.wait = [this](std::chrono::milliseconds ms)
            {
                waits.push_back(ms.count());
                now += ms.count();
                return !cancelOnWait;
            };
            return d;
        }
    };

    Tone3000HttpResponse Http(int status, const std::string &body)
    {
        Tone3000HttpResponse r;
        r.status = status;
        r.body = body;
        return r;
    }
    Tone3000HttpResponse DeviceCode(int interval = 5, int expiresIn = 600)
    {
        return Http(200, SS(R"({"device_code":"dev-123","user_code":"ABCD-EFGH","verification_uri":"https://t3k.test/device",)"
                            << R"("verification_uri_complete":"https://t3k.test/device?user_code=ABCD-EFGH","expires_in":)"
                            << expiresIn << R"(,"interval":)" << interval << "}"));
    }
    Tone3000HttpResponse OAuthError(const std::string &error, int status = 400)
    {
        return Http(status, SS(R"({"error":")" << error << R"("})"));
    }
    Tone3000HttpResponse TokenResponse(const std::string &access, const std::string &refresh, int expiresIn = 3600)
    {
        return Http(200, SS(R"({"access_token":")" << access << R"(","refresh_token":")" << refresh
                            << R"(","token_type":"Bearer","expires_in":)" << expiresIn << "}"));
    }

    struct TempDir
    {
        fs::path path;
        TempDir()
        {
            char tmpl[] = "/tmp/t3kauthtestXXXXXX";
            path = mkdtemp(tmpl);
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };
}

TEST_CASE("TONE3000 device authorization response parsing", "[t3k_device_auth]")
{
    auto d = ParseTone3000DeviceAuthorization(DeviceCode(7, 300).body);
    REQUIRE(d);
    REQUIRE(d->deviceCode == "dev-123");
    REQUIRE(d->userCode == "ABCD-EFGH");
    REQUIRE(d->verificationUri == "https://t3k.test/device");
    REQUIRE(d->verificationUriComplete == "https://t3k.test/device?user_code=ABCD-EFGH");
    REQUIRE(d->intervalS == 7);
    REQUIRE(d->expiresInS == 300);

    // verification_uri_complete is optional; interval and expires_in default.
    d = ParseTone3000DeviceAuthorization(R"({"device_code":"d","user_code":"u","verification_uri":"https://x/device"})");
    REQUIRE(d);
    REQUIRE(d->verificationUriComplete == "https://x/device");
    REQUIRE(d->intervalS == 5);
    REQUIRE(d->expiresInS == 900);

    REQUIRE(!ParseTone3000DeviceAuthorization(R"({"user_code":"u","verification_uri":"v"})"));
    REQUIRE(!ParseTone3000DeviceAuthorization("not json"));
    REQUIRE(!ParseTone3000DeviceAuthorization("[1,2]"));
}

TEST_CASE("TONE3000 token response parsing", "[t3k_device_auth]")
{
    auto t = ParseTone3000TokenResponse(TokenResponse("acc", "ref", 3600).body, 1000);
    REQUIRE(t);
    REQUIRE(t->accessToken == "acc");
    REQUIRE(t->refreshToken == "ref");
    REQUIRE(t->expiresAtMs == 1000 + 3600 * 1000);

    REQUIRE(!ParseTone3000TokenResponse(R"({"refresh_token":"r","expires_in":10})", 0));
    // expires_in is optional: default one hour.
    t = ParseTone3000TokenResponse(R"({"access_token":"a"})", 0);
    REQUIRE(t);
    REQUIRE(t->expiresAtMs == 3600 * 1000);
    REQUIRE(!ParseTone3000TokenResponse("", 0));

    REQUIRE(ParseTone3000OAuthError(R"({"error":"slow_down","error_description":"x"})") == "slow_down");
    REQUIRE(ParseTone3000OAuthError("<html>") == "");
}

TEST_CASE("TONE3000 device poller state machine", "[t3k_device_auth]")
{
    Tone3000DeviceAuthorization a;
    a.deviceCode = "d";
    a.intervalS = 5;
    a.expiresInS = 60;
    Tone3000DevicePoller poller(a, 0);
    REQUIRE(poller.IntervalMs() == 5000);

    using Action = Tone3000DevicePoller::Action;
    REQUIRE(poller.OnTokenResponse(OAuthError("authorization_pending"), 5000).action == Action::Continue);
    REQUIRE(poller.IntervalMs() == 5000);
    REQUIRE(poller.OnTokenResponse(OAuthError("slow_down"), 10000).action == Action::Continue);
    REQUIRE(poller.IntervalMs() == 10000);
    REQUIRE(poller.OnTokenResponse(OAuthError("slow_down"), 20000).action == Action::Continue);
    REQUIRE(poller.IntervalMs() == 15000);
    REQUIRE(poller.OnTokenResponse(Tone3000HttpResponse{}, 20000).action == Action::Continue); // no answer

    auto r = poller.OnTokenResponse(OAuthError("expired_token"), 30000);
    REQUIRE(r.action == Action::Failed);
    REQUIRE(r.error == "expired_token");
    r = poller.OnTokenResponse(OAuthError("access_denied"), 30000);
    REQUIRE(r.action == Action::Failed);
    REQUIRE(r.error == "access_denied");

    r = poller.OnTokenResponse(TokenResponse("a", "r", 100), 40000);
    REQUIRE(r.action == Action::Success);
    REQUIRE(r.tokens.expiresAtMs == 140000);

    REQUIRE(!poller.Expired(59999));
    REQUIRE(poller.Expired(60000));
}

TEST_CASE("TONE3000 device flow signs in and stores tokens 0600", "[t3k_device_auth]")
{
    TempDir dir;
    fs::path tokenFile = dir.path / "config" / "tone3000_auth.json";
    FakeServer server;
    server.posts = {DeviceCode(5), OAuthError("authorization_pending"), OAuthError("slow_down"),
                    OAuthError("authorization_pending"), TokenResponse("access-1", "refresh-1", 3600)};

    auto auth = Tone3000Auth::Create(tokenFile, server.Deps(), ORIGIN, "client-x");
    std::vector<Tone3000AuthStatus> statuses;
    auth->SetStatusListener([&](const Tone3000AuthStatus &s)
                            { statuses.push_back(s); });

    REQUIRE(!auth->IsSignedIn());
    auth->RunDeviceFlow();

    REQUIRE(server.requests.size() == 5);
    REQUIRE(server.requests[0].url == "https://t3k.test/api/v1/oauth/device_authorization");
    REQUIRE(server.requests[0].body == "client_id=client-x");
    REQUIRE(server.requests[1].url == "https://t3k.test/api/v1/oauth/token");
    REQUIRE(server.requests[1].body ==
            "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code&device_code=dev-123&client_id=client-x");
    // 5 s, 5 s, then slow_down adds 5 s.
    REQUIRE(server.waits == std::vector<int64_t>{5000, 5000, 10000, 10000});

    REQUIRE(auth->IsSignedIn());
    auto status = auth->GetStatus();
    REQUIRE(status.signedIn_);
    REQUIRE(status.deviceState_ == Tone3000AuthStatus::SUCCEEDED);

    // Status sequence: requesting, waiting (with codes), succeeded.
    REQUIRE(statuses.size() == 3);
    REQUIRE(statuses[0].deviceState_ == Tone3000AuthStatus::REQUESTING);
    REQUIRE(statuses[1].deviceState_ == Tone3000AuthStatus::WAITING);
    REQUIRE(statuses[1].userCode_ == "ABCD-EFGH");
    REQUIRE(statuses[1].verificationUriComplete_ == "https://t3k.test/device?user_code=ABCD-EFGH");
    REQUIRE(statuses[2].deviceState_ == Tone3000AuthStatus::SUCCEEDED);

    struct stat st;
    REQUIRE(stat(tokenFile.c_str(), &st) == 0);
    REQUIRE((st.st_mode & 0777) == 0600);

    // A new instance (daemon restart) picks up the stored session.
    FakeServer server2;
    server2.now = server.now;
    auto auth2 = Tone3000Auth::Create(tokenFile, server2.Deps(), ORIGIN, "client-x");
    REQUIRE(auth2->IsSignedIn());
    REQUIRE(auth2->GetAccessToken() == "access-1");
    REQUIRE(server2.requests.empty());

    auth2->SignOut();
    REQUIRE(!fs::exists(tokenFile));
    REQUIRE(!auth2->IsSignedIn());
}

TEST_CASE("TONE3000 device flow failures", "[t3k_device_auth]")
{
    TempDir dir;
    fs::path tokenFile = dir.path / "tone3000_auth.json";

    SECTION("expired_token")
    {
        FakeServer server;
        server.posts = {DeviceCode(), OAuthError("authorization_pending"), OAuthError("expired_token")};
        auto auth = Tone3000Auth::Create(tokenFile, server.Deps(), ORIGIN, "c");
        auth->RunDeviceFlow();
        auto s = auth->GetStatus();
        REQUIRE(s.deviceState_ == Tone3000AuthStatus::FAILED);
        REQUIRE(s.error_.find("expired") != std::string::npos);
        REQUIRE(!auth->IsSignedIn());
    }
    SECTION("access_denied")
    {
        FakeServer server;
        server.posts = {DeviceCode(), OAuthError("access_denied")};
        auto auth = Tone3000Auth::Create(tokenFile, server.Deps(), ORIGIN, "c");
        auth->RunDeviceFlow();
        auto s = auth->GetStatus();
        REQUIRE(s.deviceState_ == Tone3000AuthStatus::FAILED);
        REQUIRE(s.error_.find("declined") != std::string::npos);
        REQUIRE(!fs::exists(tokenFile));
    }
    SECTION("code deadline passes without an answer")
    {
        FakeServer server;
        server.posts = {DeviceCode(5, 12), OAuthError("authorization_pending"), OAuthError("authorization_pending")};
        auto auth = Tone3000Auth::Create(tokenFile, server.Deps(), ORIGIN, "c");
        auth->RunDeviceFlow();
        REQUIRE(server.waits.size() == 3); // polls at 5 s and 10 s; at 15 s the 12 s code is dead.
        REQUIRE(server.requests.size() == 3);
        REQUIRE(auth->GetStatus().deviceState_ == Tone3000AuthStatus::FAILED);
    }
    SECTION("device endpoint refuses: popup fallback")
    {
        FakeServer server;
        server.posts = {OAuthError("unauthorized_client", 400)};
        auto auth = Tone3000Auth::Create(tokenFile, server.Deps(), ORIGIN, "c");
        auth->RunDeviceFlow();
        auto s = auth->GetStatus();
        REQUIRE(s.deviceState_ == Tone3000AuthStatus::UNAVAILABLE);
        REQUIRE(s.error_.find("unauthorized_client") != std::string::npos);
    }
    SECTION("device endpoint unreachable")
    {
        FakeServer server;
        Tone3000HttpResponse noAnswer;
        noAnswer.transportError = "Couldn't resolve host.";
        server.posts = {noAnswer};
        auto auth = Tone3000Auth::Create(tokenFile, server.Deps(), ORIGIN, "c");
        auth->RunDeviceFlow();
        REQUIRE(auth->GetStatus().deviceState_ == Tone3000AuthStatus::FAILED);
    }
    SECTION("cancelled while waiting")
    {
        FakeServer server;
        server.cancelOnWait = true;
        server.posts = {DeviceCode()};
        auto auth = Tone3000Auth::Create(tokenFile, server.Deps(), ORIGIN, "c");
        auth->RunDeviceFlow();
        REQUIRE(server.requests.size() == 1);
        REQUIRE(!auth->IsSignedIn());
    }
}

TEST_CASE("TONE3000 device flow thread is cancellable", "[t3k_device_auth]")
{
    TempDir dir;
    FakeServer server;
    server.posts = {DeviceCode(5)};
    auto deps = server.Deps();
    deps.wait = nullptr; // the real (condition variable) wait: 5 s unless cancelled.
    auto auth = Tone3000Auth::Create(dir.path / "t.json", deps, ORIGIN, "c");

    std::mutex m;
    std::condition_variable cv;
    bool waiting = false;
    auth->SetStatusListener([&](const Tone3000AuthStatus &s)
                            {
        std::lock_guard<std::mutex> lock(m);
        if (s.deviceState_ == Tone3000AuthStatus::WAITING) waiting = true;
        cv.notify_all(); });

    auto start = std::chrono::steady_clock::now();
    auth->StartDeviceFlow();
    {
        std::unique_lock<std::mutex> lock(m);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&]()
                            { return waiting; }));
    }
    auth->CancelDeviceFlow();
    REQUIRE(auth->WaitForDeviceFlowThreads(std::chrono::seconds(2)));
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(4));
    REQUIRE(server.requests.size() == 1); // never polled.
    REQUIRE(auth->GetStatus().deviceState_ == Tone3000AuthStatus::IDLE);

    // Close() likewise stops a running flow promptly.
    server.posts = {DeviceCode(5)};
    waiting = false;
    auth->StartDeviceFlow();
    {
        std::unique_lock<std::mutex> lock(m);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&]()
                            { return waiting; }));
    }
    auth->Close();
    REQUIRE(auth->WaitForDeviceFlowThreads(std::chrono::milliseconds(10)));
}

TEST_CASE("TONE3000 token refresh scheduling", "[t3k_device_auth]")
{
    TempDir dir;
    fs::path tokenFile = dir.path / "tone3000_auth.json";
    FakeServer server;
    server.posts = {DeviceCode(1), TokenResponse("access-1", "refresh-1", 3600)};
    auto auth = Tone3000Auth::Create(tokenFile, server.Deps(), ORIGIN, "c");
    auth->RunDeviceFlow();
    REQUIRE(auth->IsSignedIn());
    int64_t expiresAt = auth->GetTokens().expiresAtMs;
    server.requests.clear();

    // More than 60 s left: the stored token, no request.
    server.now = expiresAt - 61 * 1000;
    REQUIRE(auth->GetAccessToken() == "access-1");
    REQUIRE(server.requests.empty());

    // Inside the 60 s margin: refresh.
    server.now = expiresAt - 59 * 1000;
    server.posts = {TokenResponse("access-2", "refresh-2", 3600)};
    REQUIRE(auth->GetAccessToken() == "access-2");
    REQUIRE(server.requests.size() == 1);
    REQUIRE(server.requests[0].url == "https://t3k.test/api/v1/oauth/token");
    REQUIRE(server.requests[0].body == "grant_type=refresh_token&refresh_token=refresh-1&client_id=c");

    // The refreshed pair is persisted.
    {
        FakeServer other;
        other.now = server.now;
        auto reloaded = Tone3000Auth::Create(tokenFile, other.Deps(), ORIGIN, "c");
        REQUIRE(reloaded->GetAccessToken() == "access-2");
    }

    // A refresh response without a refresh_token keeps the old one.
    server.now += Tone3000Auth::FORCED_REFRESH_COALESCE_MS; // (a forced refresh right after one is coalesced.)
    server.posts = {Http(200, R"({"access_token":"access-3","expires_in":3600})")};
    REQUIRE(auth->GetAccessToken(true) == "access-3");
    REQUIRE(auth->GetTokens().refreshToken == "refresh-2");

    // Network failure: keep the session, report the error.
    server.now += 3600 * 1000;
    server.posts = {Tone3000HttpResponse{}};
    REQUIRE_THROWS(auth->GetAccessToken());
    REQUIRE(auth->IsSignedIn());

    // Refresh token rejected: signed out.
    // A 400 that isn't invalid_grant (e.g. a malformed request) keeps the session.
    server.posts = {OAuthError("invalid_request", 400)};
    REQUIRE_THROWS(auth->GetAccessToken());
    REQUIRE(auth->IsSignedIn());

    server.posts = {OAuthError("invalid_grant", 400)};
    REQUIRE_THROWS(auth->GetAccessToken());
    REQUIRE(!auth->IsSignedIn());
    REQUIRE(!auth->GetStatus().signedIn_);
    REQUIRE(!fs::exists(tokenFile));
    REQUIRE_THROWS(auth->GetAccessToken());
}

TEST_CASE("TONE3000 authorized GET refreshes once after 401", "[t3k_device_auth]")
{
    TempDir dir;
    FakeServer server;
    server.posts = {DeviceCode(1), TokenResponse("access-1", "refresh-1", 3600)};
    auto auth = Tone3000Auth::Create(dir.path / "t.json", server.Deps(), ORIGIN, "c");
    auth->RunDeviceFlow();
    server.requests.clear();

    // 200 first time: one GET, no refresh.
    server.gets = {Http(200, "{\"id\":1}")};
    auto r = auth->AuthorizedGet("/api/v1/user");
    REQUIRE(r.status == 200);
    REQUIRE(server.requests.size() == 1);
    REQUIRE(server.requests[0].url == "https://t3k.test/api/v1/user");
    REQUIRE(server.requests[0].body == "Authorization: Bearer access-1");

    // 401: refresh, retry once with the new token.
    server.requests.clear();
    server.gets = {Http(401, ""), Http(200, "ok")};
    server.posts = {TokenResponse("access-2", "refresh-2", 3600)};
    r = auth->AuthorizedGet("/api/v1/tones/7");
    REQUIRE(r.status == 200);
    REQUIRE(server.requests.size() == 3);
    REQUIRE(server.requests[0].method == "GET");
    REQUIRE(server.requests[1].method == "POST");
    REQUIRE(server.requests[2].body == "Authorization: Bearer access-2");

    // 401 twice: only one retry; the caller sees the 401.
    server.now += Tone3000Auth::FORCED_REFRESH_COALESCE_MS; // (a forced refresh right after one is coalesced.)
    server.requests.clear();
    server.gets = {Http(401, ""), Http(401, "")};
    server.posts = {TokenResponse("access-3", "refresh-3", 3600)};
    r = auth->AuthorizedGet("/api/v1/tones/7");
    REQUIRE(r.status == 401);
    REQUIRE(server.requests.size() == 3);

    REQUIRE_THROWS(auth->AuthorizedGet("http://insecure.example/x"));
    // The bearer token only goes to the TONE3000 origin.
    server.requests.clear();
    REQUIRE_THROWS(auth->AuthorizedGet("https://evil.example/api/v1/user"));
    REQUIRE_THROWS(auth->AuthorizedGet("https://t3k.test.evil.example/api/v1/user"));
    REQUIRE_THROWS(auth->AuthorizedGet("https://t3k.test@evil.example/x"));
    REQUIRE(server.requests.empty());
}

TEST_CASE("TONE3000 refresh does not overwrite a newer session", "[t3k_device_auth]")
{
    TempDir dir;
    fs::path tokenFile = dir.path / "t.json";
    FakeServer server;
    server.posts = {DeviceCode(1), TokenResponse("old-access", "old-refresh", 3600)};
    auto auth = Tone3000Auth::Create(tokenFile, server.Deps(), ORIGIN, "c");
    auth->RunDeviceFlow();

    SECTION("new sign-in while refreshing: the new session wins")
    {
        // The refresh POST is in flight when the user completes a new sign-in.
        server.beforeNextPost = [&]()
        {
            // Served before the in-flight refresh's own response.
            server.posts.push_front(TokenResponse("new-access", "new-refresh", 3600));
            server.posts.push_front(DeviceCode(1));
            auth->RunDeviceFlow();
        };
        server.posts = {TokenResponse("stale-access", "stale-refresh", 3600)};
        REQUIRE(auth->GetAccessToken(true) == "new-access");
        REQUIRE(auth->GetTokens().refreshToken == "new-refresh");

        FakeServer other;
        other.now = server.now;
        REQUIRE(Tone3000Auth::Create(tokenFile, other.Deps(), ORIGIN, "c")->GetAccessToken() == "new-access");
    }
    SECTION("new sign-in while a rejected refresh is in flight: not signed out")
    {
        server.beforeNextPost = [&]()
        {
            // Served before the in-flight refresh's own response.
            server.posts.push_front(TokenResponse("new-access", "new-refresh", 3600));
            server.posts.push_front(DeviceCode(1));
            auth->RunDeviceFlow();
        };
        server.posts = {OAuthError("invalid_grant", 400)};
        REQUIRE_THROWS(auth->GetAccessToken(true));
        REQUIRE(auth->IsSignedIn());
        REQUIRE(auth->GetTokens().accessToken == "new-access");
    }
    SECTION("sign-out while refreshing stays signed out")
    {
        server.beforeNextPost = [&]()
        { auth->SignOut(); };
        server.posts = {TokenResponse("stale-access", "stale-refresh", 3600)};
        REQUIRE_THROWS(auth->GetAccessToken(true));
        REQUIRE(!auth->IsSignedIn());
        REQUIRE(!fs::exists(tokenFile));
    }
}

TEST_CASE("TemporaryFile is private, unique and fast", "[t3k_device_auth]")
{
    TempDir dir;
    auto start = std::chrono::steady_clock::now();
    std::vector<TemporaryFile> files;
    std::set<fs::path> names;
    for (int i = 0; i < 20; ++i)
    {
        files.emplace_back(dir.path);
        names.insert(files.back().Path());
        struct stat st;
        REQUIRE(stat(files.back().Path().c_str(), &st) == 0);
        REQUIRE((st.st_mode & 0777) == 0600);
        REQUIRE(files.back().Path().filename().string().starts_with("temp_"));
        REQUIRE(files.back().Path().extension() == ".tmp");
    }
    REQUIRE(names.size() == 20);
    // The old implementation re-seeded rand() from time() and spun until the clock ticked.
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));
    files.clear();
    REQUIRE(fs::is_empty(dir.path));
}

TEST_CASE("TONE3000 forced refreshes are coalesced", "[t3k_device_auth]")
{
    TempDir dir;
    FakeServer server;
    server.posts = {DeviceCode(1), TokenResponse("access-1", "refresh-1", 3600)};
    auto auth = Tone3000Auth::Create(dir.path / "t.json", server.Deps(), ORIGIN, "c");
    auth->RunDeviceFlow();
    server.requests.clear();

    // the first forced refresh (e.g. after a 401) goes to the server.
    server.posts = {TokenResponse("access-2", "refresh-2", 3600)};
    REQUIRE(auth->GetAccessToken(true) == "access-2");
    REQUIRE(server.requests.size() == 1);

    // other clients retrying after the same 401: the token that was just refreshed, no request.
    server.now += Tone3000Auth::FORCED_REFRESH_COALESCE_MS - 1;
    REQUIRE(auth->GetAccessToken(true) == "access-2");
    REQUIRE(auth->GetAccessToken(true) == "access-2");
    REQUIRE(server.requests.size() == 1);

    // but a retry after a 401 for the current token itself really refreshes.
    server.posts = {TokenResponse("access-2b", "refresh-2b", 3600)};
    REQUIRE(auth->GetAccessToken(true, "access-2") == "access-2b");
    REQUIRE(server.requests.size() == 2);
    // (a 401 for an older token is coalesced.)
    REQUIRE(auth->GetAccessToken(true, "access-1") == "access-2b");
    REQUIRE(server.requests.size() == 2);
    server.requests.erase(server.requests.begin() + 1);

    // later: refreshed again.
    server.now += Tone3000Auth::FORCED_REFRESH_COALESCE_MS;
    server.posts = {TokenResponse("access-3", "refresh-3", 3600)};
    REQUIRE(auth->GetAccessToken(true) == "access-3");
    REQUIRE(server.requests.size() == 2);

    // AuthorizedGet's retry after a 401 names the token it used: refreshed even right after a refresh.
    server.requests.clear();
    server.gets = {Http(401, ""), Http(200, "ok")};
    server.posts = {TokenResponse("access-3b", "refresh-3b", 3600)};
    REQUIRE(auth->AuthorizedGet("/api/v1/user").status == 200);
    REQUIRE(server.requests.size() == 3);
    REQUIRE(server.requests[1].method == "POST");
    REQUIRE(server.requests[2].body == "Authorization: Bearer access-3b");

    // a new session isn't coalesced with the previous session's refresh.
    auth->SignOut();
    server.posts = {DeviceCode(1), TokenResponse("access-4", "refresh-4", 3600)};
    auth->RunDeviceFlow();
    server.requests.clear();
    server.posts = {TokenResponse("access-5", "refresh-5", 3600)};
    REQUIRE(auth->GetAccessToken(true) == "access-5");
    REQUIRE(server.requests.size() == 1);
}

TEST_CASE("TONE3000 a client cancels only the sign-in it started", "[t3k_device_auth]")
{
    TempDir dir;
    FakeServer server;
    server.posts = {DeviceCode(5)};
    auto deps = server.Deps();
    deps.wait = nullptr; // the real (condition variable) wait: 5 s unless cancelled.
    auto auth = Tone3000Auth::Create(dir.path / "t.json", deps, ORIGIN, "c");

    std::mutex m;
    std::condition_variable cv;
    bool waiting = false;
    auth->SetStatusListener([&](const Tone3000AuthStatus &s)
                            {
        std::lock_guard<std::mutex> lock(m);
        if (s.deviceState_ == Tone3000AuthStatus::WAITING) waiting = true;
        cv.notify_all(); });

    auth->StartDeviceFlow(1); // client 1 opens the sign-in dialog.
    {
        std::unique_lock<std::mutex> lock(m);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&]()
                            { return waiting; }));
    }
    // client 2 closes its (unrelated) dialog: client 1's sign-in carries on.
    REQUIRE_FALSE(auth->CancelDeviceFlow(2));
    REQUIRE(auth->GetStatus().deviceState_ == Tone3000AuthStatus::WAITING);
    REQUIRE(auth->GetStatus().userCode_ == "ABCD-EFGH");

    SECTION("the owner closes its dialog")
    {
        REQUIRE(auth->CancelDeviceFlow(1));
    }
    SECTION("the owner disconnected: the orphaned flow keeps running, and any client can cancel it")
    {
        auth->ReleaseDeviceFlowOwner(2); // not the owner: no effect.
        REQUIRE_FALSE(auth->CancelDeviceFlow(2));
        auth->ReleaseDeviceFlowOwner(1); // client 1's socket closed.
        REQUIRE(auth->GetStatus().deviceState_ == Tone3000AuthStatus::WAITING);
        REQUIRE(auth->CancelDeviceFlow(3)); // e.g. client 1 reconnected with a new id.
    }
    REQUIRE(auth->GetStatus().deviceState_ == Tone3000AuthStatus::IDLE);
    REQUIRE(auth->WaitForDeviceFlowThreads(std::chrono::seconds(2)));
    auth->Close();
}

TEST_CASE("TONE3000 cancelling with no sign-in in progress changes nothing", "[t3k_device_auth]")
{
    TempDir dir;
    FakeServer server;
    server.posts = {DeviceCode(1), OAuthError("access_denied")};
    auto auth = Tone3000Auth::Create(dir.path / "t.json", server.Deps(), ORIGIN, "c");
    int published = 0;
    auth->SetStatusListener([&](const Tone3000AuthStatus &)
                            { ++published; });
    // idle: nothing to cancel, by anyone.
    REQUIRE_FALSE(auth->CancelDeviceFlow());
    REQUIRE_FALSE(auth->CancelDeviceFlow(5));
    REQUIRE(published == 0);

    // a finished (failed) flow keeps its status and error.
    auth->RunDeviceFlow();
    auto failed = auth->GetStatus();
    REQUIRE(failed.deviceState_ == Tone3000AuthStatus::FAILED);
    REQUIRE(!failed.error_.empty());
    published = 0;
    REQUIRE_FALSE(auth->CancelDeviceFlow());
    REQUIRE_FALSE(auth->CancelDeviceFlow(5));
    REQUIRE(published == 0);
    REQUIRE(auth->GetStatus().deviceState_ == Tone3000AuthStatus::FAILED);
    REQUIRE(auth->GetStatus().error_ == failed.error_);
}

TEST_CASE("TONE3000 access token request body", "[t3k_device_auth]")
{
    auto read = [](const std::string &json)
    {
        std::stringstream s(json);
        json_reader reader(s);
        return ReadTone3000AccessTokenRequest(reader);
    };
    // older clients: a bare bool.
    REQUIRE(read("true").forceRefresh);
    REQUIRE_FALSE(read("false").forceRefresh);
    REQUIRE(read("true").rejectedAccessToken.empty());

    auto request = read(R"({"forceRefresh":true,"rejectedAccessToken":"abc"})");
    REQUIRE(request.forceRefresh);
    REQUIRE(request.rejectedAccessToken == "abc");
    request = read(R"({"forceRefresh":false,"rejectedAccessToken":""})");
    REQUIRE_FALSE(request.forceRefresh);
    REQUIRE(request.rejectedAccessToken.empty());

    REQUIRE_THROWS(read("1"));
    REQUIRE_THROWS(read(R"({"forceRefresh":"yes"})"));
    REQUIRE_THROWS(read(R"({"forceRefresh":true,"rejectedAccessToken":5})"));
}

TEST_CASE("TONE3000 sign-out clears the device sign-in status", "[t3k_device_auth]")
{
    TempDir dir;
    FakeServer server;
    server.posts = {DeviceCode(1), TokenResponse("access-1", "refresh-1", 3600)};
    auto auth = Tone3000Auth::Create(dir.path / "t.json", server.Deps(), ORIGIN, "c");
    auth->RunDeviceFlow();
    REQUIRE(auth->GetStatus().deviceState_ == Tone3000AuthStatus::SUCCEEDED);

    SECTION("explicit sign-out")
    {
        auth->SignOut();
    }
    SECTION("refresh token rejected")
    {
        server.posts = {OAuthError("invalid_grant", 400)};
        REQUIRE_THROWS(auth->GetAccessToken(true));
    }
    auto status = auth->GetStatus();
    REQUIRE(!status.signedIn_);
    REQUIRE(status.deviceState_ == Tone3000AuthStatus::IDLE);
    REQUIRE(status.userCode_.empty());
    REQUIRE(status.verificationUri_.empty());
    REQUIRE(status.verificationUriComplete_.empty());
    REQUIRE(status.expiresAtMs_ == 0);
}

TEST_CASE("TONE3000 Close waits for background operations and listener calls", "[t3k_device_auth]")
{
    TempDir dir;
    FakeServer server;
    auto auth = Tone3000Auth::Create(dir.path / "t.json", server.Deps(), ORIGIN, "c");

    SECTION("tracked operations")
    {
        std::atomic<bool> released{false};
        auto operation = auth->BeginOperation();
        std::thread worker([&released, operation]() mutable
                           {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            released = true;
            operation.reset(); });
        operation.reset(); // the worker holds the only token.
        REQUIRE(auth->Close());
        REQUIRE(released); // Close() returned only after the worker released its token.
        worker.join();
        // no new operations once closed.
        REQUIRE_THROWS(auth->BeginOperation());
    }
    SECTION("a listener call in progress")
    {
        std::atomic<bool> inListener{false};
        std::atomic<bool> listenerDone{false};
        auth->SetStatusListener([&](const Tone3000AuthStatus &)
                                {
            inListener = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            listenerDone = true; });
        std::thread worker([auth]()
                           { auth->SignOut(); }); // publishes.
        while (!inListener)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(auth->Close());
        REQUIRE(listenerDone); // the listener's owner may go away now.
        worker.join();
        // and is never called again, nor is a listener set after Close().
        listenerDone = false;
        bool lateListenerCalled = false;
        auth->SetStatusListener([&](const Tone3000AuthStatus &)
                                { lateListenerCalled = true; });
        auth->SignOut();
        REQUIRE(!listenerDone);
        REQUIRE(!lateListenerCalled);
    }
}

TEST_CASE("TONE3000 Close aborts the requests in flight", "[t3k_device_auth]")
{
    TempDir dir;
    // A GET that blocks until the dependency's abort() is called.
    struct Blocker
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool aborted = false;
        bool entered = false;
    };
    auto blocker = std::make_shared<Blocker>();
    Tone3000Auth::Dependencies deps;
    deps.nowMs = []()
    { return (int64_t)1'000'000'000'000; };
    deps.postForm = [](const std::string &, const std::string &)
    { return Tone3000HttpResponse{}; };
    deps.get = [blocker](const std::string &, const std::vector<std::string> &)
    {
        std::unique_lock<std::mutex> lock(blocker->mutex);
        blocker->entered = true;
        blocker->cv.notify_all();
        blocker->cv.wait_for(lock, std::chrono::seconds(30), [&]()
                             { return blocker->aborted; });
        Tone3000HttpResponse r;
        r.status = 0;
        r.transportError = "aborted";
        return r;
    };
    deps.abort = [blocker]()
    {
        std::lock_guard<std::mutex> lock(blocker->mutex);
        blocker->aborted = true;
        blocker->cv.notify_all();
    };
    {
        std::ofstream f(dir.path / "t.json");
        f << R"({"access_token":"a","refresh_token":"r","expires_at":4102444800000})";
    }
    auto auth = Tone3000Auth::Create(dir.path / "t.json", deps, "https://t3k.test", "c");
    REQUIRE(auth->IsSignedIn());
    auto operation = auth->BeginOperation();
    std::thread worker([auth, operation]() mutable
                       {
        auth->AuthorizedGet("/api/v1/user");
        operation.reset(); });
    {
        std::unique_lock<std::mutex> lock(blocker->mutex);
        REQUIRE(blocker->cv.wait_for(lock, std::chrono::seconds(5), [&]()
                                     { return blocker->entered; }));
    }
    operation.reset();
    auto start = std::chrono::steady_clock::now();
    REQUIRE(auth->Close());
    REQUIRE(std::chrono::steady_clock::now() - start < Tone3000Auth::CLOSE_TIMEOUT);
    REQUIRE(blocker->aborted);
    worker.join();
}
