// Copyright (c) 2026 Robin Davies
// MIT license; see other source files in this project.
#include "pch.h"
#include "catch.hpp"
#include "WebServer.hpp"
#include "UploadPolicy.hpp"
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace pipedal;
namespace fs = std::filesystem;
using clock_type = std::chrono::steady_clock;

// Slow-loris: UploadRatePolicy, enforced in request_with_file_upload::consume() and by the
// per-connection watchdog, cuts off slow or silent clients ~10 s in (header deadline / body
// grace), well before the open-handshake cap (30..3600 s), and frees their upload slots.
// Hidden ([.]) because it takes ~11 s; run it explicitly with "[slow_loris]".

// WebServer ignores its address argument and always listens on tcp::v6() (dual-stack where
// the host allows it), so ::1 is the native loopback and is tried first; 127.0.0.1 is the
// fallback (IPv6 disabled, or a v6-only socket).
static int ConnectLocal(int port)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd >= 0)
    {
        sockaddr_in6 addr6{};
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons((uint16_t)port);
        addr6.sin6_addr = in6addr_loopback;
        if (connect(fd, (sockaddr *)&addr6, sizeof(addr6)) == 0)
            return fd;
        close(fd);
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (sockaddr *)&addr, sizeof(addr)) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static void SendAll(int fd, const std::string &s)
{
    size_t sent = 0;
    while (sent < s.size())
    {
        ssize_t n = send(fd, s.data() + sent, s.size() - sent, MSG_NOSIGNAL);
        if (n <= 0)
            return;
        sent += (size_t)n;
    }
}

// Every 250 ms, send `trickle` (if non-empty) until the server closes the connection.
// Returns seconds until close, or a negative value if still open at the deadline.
static double WaitForClose(int fd, double deadlineSeconds, const std::string &trickle, std::string *response)
{
    auto start = clock_type::now();
    while (true)
    {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 250) > 0)
        {
            char buf[512];
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
                return std::chrono::duration<double>(clock_type::now() - start).count();
            if (response)
                response->append(buf, (size_t)n);
            continue;
        }
        double elapsed = std::chrono::duration<double>(clock_type::now() - start).count();
        if (elapsed > deadlineSeconds)
            return -1;
        if (!trickle.empty())
            SendAll(fd, trickle);
    }
}

static std::string ReadResponse(int fd, double timeoutSeconds)
{
    std::string result;
    auto start = clock_type::now();
    while (std::chrono::duration<double>(clock_type::now() - start).count() < timeoutSeconds)
    {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 250) <= 0)
            continue;
        char buf[512];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        result.append(buf, (size_t)n);
    }
    return result;
}

TEST_CASE("slow-loris requests are cut off by the upload rate policy", "[.][slow_loris]")
{
    fs::path tempDir = fs::temp_directory_path() / "pipedal_slow_loris_test";
    fs::remove_all(tempDir);
    fs::create_directories(tempDir);

    // RAII: restore the global temp directory and shut the server down even if a REQUIRE
    // below fails. Declared in this order so the server stops before the directory is reset.
    struct TempDirRestore
    {
        fs::path previous = WebServer::GetUploadTempDirectory();
        ~TempDirRestore() { WebServer::SetUploadTempDirectory(previous); }
    } tempDirRestore;
    WebServer::SetUploadTempDirectory(tempDir);

    // 64 MiB max upload => 2058 s open-handshake cap; everything below must finish far sooner.
    REQUIRE(UploadRatePolicy::RequestTimeCap(64 * 1024 * 1024) == std::chrono::seconds(2058));
    struct ServerGuard
    {
        std::shared_ptr<WebServer> server;
        void Stop()
        {
            if (server)
            {
                server->ShutDown(1000);
                server->Join();
                server.reset();
            }
        }
        ~ServerGuard() { Stop(); }
    } guard{WebServer::create(
        boost::asio::ip::make_address("::1"), 0, // address is ignored; see ConnectLocal
         ".", 4, 64 * 1024 * 1024)};
    auto &server = guard.server;
    server->RunInBackground();
    int port = 0;
    for (int i = 0; i < 100 && port == 0; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        port = server->GetListeningPort();
    }
    REQUIRE(port != 0);

    const std::string uploadHead =
        "POST /x HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10000000\r\n\r\nabc";

    struct Case
    {
        const char *name;
        std::string initial;
        std::string trickle;
        double seconds = -1;
        std::string response;
    };
    std::vector<Case> cases{
        // Headers trickle in forever: header deadline (consume path) => 400.
        {"headers trickle", "GET / HTTP/1.1\r\nHost: localhost\r\nX-Slow: ", "x"},
        // Connects, sends nothing: watchdog (connection-time header deadline).
        {"silent connection", "", ""},
        // Two uploads take both slots (UploadLimiter{2}) and trickle the body: rate rule.
        {"upload body trickle 1", uploadHead, "x"},
        {"upload body trickle 2", uploadHead, "x"},
    };
    std::vector<std::thread> threads;
    for (auto &c : cases)
    {
        threads.emplace_back([&c, port]
                             {
            int fd = ConnectLocal(port);
            if (fd < 0)
                return;
            if (!c.initial.empty())
                SendAll(fd, c.initial);
            c.seconds = WaitForClose(fd, 30.0, c.trickle, &c.response);
            close(fd); });
    }
    for (auto &t : threads)
        t.join();
    for (auto &c : cases)
    {
        INFO(c.name << ": " << c.seconds << " s, response: " << c.response.substr(0, 40));
        CHECK(c.seconds > 9.0);
        CHECK(c.seconds < 13.0);
    }
    // Whichever of consume() (400 response) or the watchdog (plain close) fires first.
    CHECK((cases[0].response.empty() || cases[0].response.starts_with("HTTP/1.1 400")));

    // Upload slots were freed: a normal upload is processed (no 503), quickly.
    {
        int fd = ConnectLocal(port);
        REQUIRE(fd >= 0);
        std::string body(1024 * 1024, 'z');
        auto start = clock_type::now();
        SendAll(fd, "POST /x HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Length: " +
                        std::to_string(body.size()) + "\r\n\r\n" + body);
        std::string response = ReadResponse(fd, 10.0);
        close(fd);
        INFO(response.substr(0, 60));
        CHECK(response.starts_with("HTTP/1.1 "));
        CHECK(response.find(" 503 ") == std::string::npos);
        CHECK(clock_type::now() - start < std::chrono::seconds(5));
    }

    guard.Stop();
    CHECK(fs::is_empty(tempDir)); // spooled bodies were cleaned up
    fs::remove_all(tempDir);
}
