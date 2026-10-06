// Copyright (c) 2026 Robin Davies
// MIT license; see other source files in this project.

// TONE3000 API conformance.
//
// [t3k_api]   Recorded responses (sanitised: same members, types, nulls and enum values as the
//             live API in October 2026; every name, id, URL and text replaced) through the
//             parsers PiPedal uses; cancellable curl; origin checks.
// [t3k_live]  Hidden. Talks to www.tone3000.com with the credentials in ~/.env.tone3000
//             (SECRET_KEY: a server-side bearer credential; PUBLISHABLE_KEY: an OAuth client
//             id), read at run time and never printed. Skips when the file is absent.
//             The secret key goes into a 0600 token file in a 0700 temp directory
//             (pipedal-t3k-api-XXXXXX under $TMPDIR), removed when the test ends; a crash
//             can leave it behind.
//             Run with: pipedaltest "[t3k_live]"

#include "pch.h"
#include "catch.hpp"
#include "Curl.hpp"
#include "IrClassifier.hpp"
#include "Tone3000Auth.hpp"
#include "Tone3000Catalog.hpp"
#include "Tone3000Download.hpp"
#include "Tone3000Downloader.hpp" // PIPEDAL_T3K_PUBLISHABLE_KEY
#include "Tone3000Tone.hpp"
#include "HtmlHelper.hpp"
#include "json_variant.hpp"
#include "ss.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <mutex>
#include <fstream>
#include <map>
#include <netinet/in.h>
#include <set>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace pipedal;
namespace fs = std::filesystem;

namespace
{
    // GET /api/v1/tones/{id} for an IR tone (gear space, format ir, sizes null, display_name null).
    const char *TONE_INFO_IR = R"FIXTURE({"id":88860,"user_id":"00000000-0000-4000-8000-000000000001","title":"Example tone 2","description":"Example description 3.","created_at":"2026-01-02T03:04:05.000Z","updated_at":"2026-01-02T03:04:05.000Z","published_at":"2026-01-02T03:04:05.000Z","images":["https://example.invalid/image-7.jpg"],"is_public":true,"links":[],"models_count":2,"favorites_count":668,"downloads_count":33177,"license":"t3k","sizes":null,"a1_models_count":0,"a2_models_count":0,"irs_count":2,"custom_models_count":0,"is_favorite":false,"user":{"id":"00000000-0000-4000-8000-000000000009","username":"example-user-10","avatar_url":"https://www.tone3000.com/example/11","display_name":null,"is_verified":false,"url":"https://www.tone3000.com/example/12"},"makes":[{"id":77958,"name":"Example name 13"}],"tags":[{"id":271282,"name":"Example name 14"},{"id":271345,"name":"Example name 15"},{"id":271284,"name":"Example name 16"},{"id":15,"name":"Example name 17"},{"id":271015,"name":"Example name 18"},{"id":14,"name":"Example name 19"},{"id":94022,"name":"Example name 20"},{"id":70155,"name":"Example name 21"},{"id":271014,"name":"Example name 22"}],"url":"https://www.tone3000.com/example/23","format":"ir","gear":"space"})FIXTURE";

    // GET /api/v1/tones/search: a tone with images null and avatar_url null (gear experimental, format ir).
    const char *SEARCH_NULL_IMAGES = R"FIXTURE({"data":[{"id":36381,"title":"Example tone 1","description":"Example description 2.","tags":[{"name":"Example name 3"}],"makes":[],"models_count":4,"a1_models_count":0,"a2_models_count":0,"irs_count":4,"custom_models_count":0,"favorites_count":80,"downloads_count":5495,"is_favorite":false,"created_at":"2026-01-02T03:04:05.000Z","updated_at":"2026-01-02T03:04:05.000Z","published_at":"2026-01-02T03:04:05.000Z","license":"t3k","images":null,"user_id":"00000000-0000-4000-8000-000000000008","user":{"id":"00000000-0000-4000-8000-000000000009","username":"example-user-10","avatar_url":null,"display_name":null,"is_verified":false,"url":"https://www.tone3000.com/example/11"},"url":"https://www.tone3000.com/example/12","format":"ir","gear":"experimental"}]})FIXTURE";

    // GET /api/v1/models?tone_id=: IR models (size and architecture_version null).
    const char *MODELS_IR = R"FIXTURE({"data":[{"id":749548,"tone_id":88860,"user_id":"00000000-0000-4000-8000-000000000001","created_at":"2026-01-02T03:04:05.000Z","updated_at":"2026-01-02T03:04:05.000Z","name":"Example name 4","model_url":"https://www.tone3000.com/api/v1/models/5/download","size":null,"architecture_version":null},{"id":748925,"tone_id":88860,"user_id":"00000000-0000-4000-8000-000000000006","created_at":"2026-01-02T03:04:05.000Z","updated_at":"2026-01-02T03:04:05.000Z","name":"Example name 9","model_url":"https://www.tone3000.com/api/v1/models/10/download","size":null,"architecture_version":null}],"page":1,"page_size":50,"total":2,"total_pages":1})FIXTURE";

    // GET /api/v1/models?tone_id=: NAM models.
    const char *MODELS_NAM = R"FIXTURE({"data":[{"id":683631,"tone_id":76884,"user_id":"00000000-0000-4000-8000-000000000001","created_at":"2026-01-02T03:04:05.000Z","updated_at":"2026-01-02T03:04:05.000Z","name":"Example name 4","model_url":"https://www.tone3000.com/api/v1/models/5/download","size":"standard","architecture_version":"1"},{"id":683629,"tone_id":76884,"user_id":"00000000-0000-4000-8000-000000000008","created_at":"2026-01-02T03:04:05.000Z","updated_at":"2026-01-02T03:04:05.000Z","name":"Example name 11","model_url":"https://www.tone3000.com/api/v1/models/12/download","size":"standard","architecture_version":"1"}],"page":1,"page_size":50,"total":11,"total_pages":1})FIXTURE";

    // GET /api/v1/tones/search?gears=space.
    const char *SEARCH_SPACE = R"FIXTURE({"data":[{"id":88860,"title":"Example tone 1","description":"Example description 2.","tags":[{"name":"Example name 3"},{"name":"Example name 4"},{"name":"Example name 5"},{"name":"Example name 6"},{"name":"Example name 7"},{"name":"Example name 8"},{"name":"Example name 9"},{"name":"Example name 10"},{"name":"Example name 11"}],"makes":[{"name":"Example name 12"}],"models_count":2,"a1_models_count":0,"a2_models_count":0,"irs_count":2,"custom_models_count":0,"favorites_count":668,"downloads_count":33177,"is_favorite":false,"created_at":"2026-01-02T03:04:05.000Z","updated_at":"2026-01-02T03:04:05.000Z","published_at":"2026-01-02T03:04:05.000Z","license":"t3k","images":["https://example.invalid/image-17.jpg"],"user_id":"00000000-0000-4000-8000-000000000018","user":{"id":"00000000-0000-4000-8000-000000000019","username":"example-user-20","avatar_url":"https://www.tone3000.com/example/21","display_name":null,"is_verified":false,"url":"https://www.tone3000.com/example/22"},"url":"https://www.tone3000.com/example/23","format":"ir","gear":"space"},{"id":87851,"title":"Example tone 26","description":"Example description 27.","tags":[{"name":"Example name 28"},{"name":"Example name 29"},{"name":"Example name 30"},{"name":"Example name 31"}],"makes":[{"name":"Example name 32"}],"models_count":14,"a1_models_count":0,"a2_models_count":0,"irs_count":14,"custom_models_count":0,"favorites_count":588,"downloads_count":19115,"is_favorite":false,"created_at":"2026-01-02T03:04:05.000Z","updated_at":"2026-01-02T03:04:05.000Z","published_at":"2026-01-02T03:04:05.000Z","license":"t3k","images":["https://example.invalid/image-37.jpg"],"user_id":"00000000-0000-4000-8000-000000000038","user":{"id":"00000000-0000-4000-8000-000000000039","username":"example-user-40","avatar_url":"https://www.tone3000.com/example/41","display_name":null,"is_verified":false,"url":"https://www.tone3000.com/example/42"},"url":"https://www.tone3000.com/example/43","format":"ir","gear":"space"}],"page":1,"page_size":25,"total":42,"total_pages":2})FIXTURE";

    // GET /api/v1/tones/trending?gear=cab (anonymous): no paging members.
    const char *TRENDING_CAB = R"FIXTURE({"data":[{"id":90730,"user_id":"00000000-0000-4000-8000-000000000001","title":"Example tone 2","description":"Example description 3.","created_at":"2026-01-02T03:04:05.000Z","updated_at":"2026-01-02T03:04:05.000Z","images":["https://example.invalid/image-6.jpg"],"is_public":true,"links":["https://example.invalid/link-7","https://example.invalid/link-8"],"license":"t3k","models_count":4,"favorites_count":535,"downloads_count":23887,"sizes":null,"published_at":"2026-01-02T03:04:05.000Z","a1_models_count":0,"a2_models_count":0,"custom_models_count":0,"irs_count":4,"makes":[{"id":10247,"name":"Example name 11"},{"id":66187,"name":"Example name 12"}],"tags":[{"id":79427,"name":"Example name 13"},{"id":136245,"name":"Example name 14"},{"id":139,"name":"Example name 15"},{"id":136242,"name":"Example name 16"},{"id":66216,"name":"Example name 17"},{"id":97190,"name":"Example name 18"},{"id":109118,"name":"Example name 19"},{"id":224116,"name":"Example name 20"},{"id":112402,"name":"Example name 21"},{"id":56770,"name":"Example name 22"},{"id":70538,"name":"Example name 23"},{"id":282,"name":"Example name 24"},{"id":140,"name":"Example name 25"},{"id":101020,"name":"Example name 26"},{"id":58863,"name":"Example name 27"},{"id":39,"name":"Example name 28"},{"id":136932,"name":"Example name 29"},{"id":170665,"name":"Example name 30"},{"id":57143,"name":"Example name 31"},{"id":279196,"name":"Example name 32"},{"id":73908,"name":"Example name 33"},{"id":58742,"name":"Example name 34"},{"id":279199,"name":"Example name 35"},{"id":118,"name":"Example name 36"},{"id":90742,"name":"Example name 37"},{"id":71437,"name":"Example name 38"},{"id":279203,"name":"Example name 39"},{"id":160,"name":"Example name 40"},{"id":59059,"name":"Example name 41"},{"id":84442,"name":"Example name 42"},{"id":87750,"name":"Example name 43"},{"id":15,"name":"Example name 44"}],"user":{"id":"00000000-0000-4000-8000-000000000045","username":"example-user-46","avatar_url":"https://www.tone3000.com/example/47","display_name":"Example User 48","is_verified":true,"url":"https://www.tone3000.com/example/49"},"is_favorite":false,"url":"https://www.tone3000.com/example/50","format":"ir","gear":"cab"},{"id":84863,"user_id":"00000000-0000-4000-8000-000000000053","title":"Example tone 54","description":"Example description 55.","created_at":"2026-01-02T03:04:05.000Z","updated_at":"2026-01-02T03:04:05.000Z","images":["https://example.invalid/image-58.jpg"],"is_public":true,"links":["https://example.invalid/link-59"],"license":"t3k","models_count":5,"favorites_count":852,"downloads_count":38193,"sizes":null,"published_at":"2026-01-02T03:04:05.000Z","a1_models_count":0,"a2_models_count":0,"custom_models_count":0,"irs_count":5,"makes":[{"id":385,"name":"Example name 62"},{"id":10717,"name":"Example name 63"},{"id":14419,"name":"Example name 64"},{"id":66889,"name":"Example name 65"}],"tags":[{"id":39,"name":"Example name 66"},{"id":56890,"name":"Example name 67"},{"id":147117,"name":"Example name 68"},{"id":252563,"name":"Example name 69"},{"id":109182,"name":"Example name 70"},{"id":182688,"name":"Example name 71"},{"id":15,"name":"Example name 72"}],"user":{"id":"00000000-0000-4000-8000-000000000073","username":"example-user-74","avatar_url":"https://www.tone3000.com/example/75","display_name":null,"is_verified":false,"url":"https://www.tone3000.com/example/76"},"is_favorite":false,"url":"https://www.tone3000.com/example/77","format":"ir","gear":"cab"}]})FIXTURE";

    // GET /api/v1/tags.
    const char *TAGS = R"FIXTURE({"data":[{"id":103,"name":"Example name 1","tones_count":9616,"url":"https://www.tone3000.com/example/2"},{"id":39,"name":"Example name 3","tones_count":3050,"url":"https://www.tone3000.com/example/4"}],"page":1,"page_size":5,"total":6361,"total_pages":1273})FIXTURE";

    // TONE3000's gear values. The API's own list (from its 400 answer to an unknown gear) is
    // compared against this in the live test.
    const std::set<std::string> API_GEARS = {"amp", "amp-cab", "pedal", "outboard", "cab", "space", "experimental"};

    struct TempDir
    {
        fs::path path;
        TempDir()
        {
            std::string pattern = (fs::temp_directory_path() / "pipedal-t3k-api-XXXXXX").string();
            std::vector<char> buffer(pattern.begin(), pattern.end());
            buffer.push_back('\0');
            REQUIRE(mkdtemp(buffer.data()) != nullptr); // 0700.
            path = buffer.data();
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    // Curl's temp files go to a test directory (the daemon's is not writable here).
    struct CurlTempDirectoryScope
    {
        fs::path previous;
        CurlTempDirectoryScope(const fs::path &directory)
            : previous(GetCurlTempDirectory())
        {
            SetCurlTempDirectory(directory);
        }
        ~CurlTempDirectoryScope() { SetCurlTempDirectory(previous); }
    };

    // A TCP listener that never answers: curl connects (the kernel completes the handshake),
    // sends its request and waits.
    struct SilentServer
    {
        int fd = -1;
        int port = 0;
        SilentServer()
        {
            fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            REQUIRE(fd >= 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            REQUIRE(bind(fd, (sockaddr *)&addr, sizeof(addr)) == 0);
            REQUIRE(listen(fd, 16) == 0);
            socklen_t len = sizeof(addr);
            REQUIRE(getsockname(fd, (sockaddr *)&addr, &len) == 0);
            port = ntohs(addr.sin_port);
        }
        ~SilentServer() { close(fd); }
        std::string Origin() const { return SS("http://127.0.0.1:" << port); }
    };

    // Answers each connection with 200 "ok" and records the requests (headers and body).
    class AnsweringServer
    {
    public:
        AnsweringServer()
        {
            fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            REQUIRE(fd >= 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            REQUIRE(bind(fd, (sockaddr *)&addr, sizeof(addr)) == 0);
            REQUIRE(listen(fd, 16) == 0);
            socklen_t len = sizeof(addr);
            REQUIRE(getsockname(fd, (sockaddr *)&addr, &len) == 0);
            port = ntohs(addr.sin_port);
            thread = std::thread([this]()
                                 { Serve(); });
        }
        ~AnsweringServer()
        {
            shutdown(fd, SHUT_RDWR); // unblocks accept().
            close(fd);
            thread.join();
        }
        std::string Origin() const { return SS("http://127.0.0.1:" << port); }
        std::vector<std::string> Requests()
        {
            std::lock_guard<std::mutex> lock(mutex);
            return requests;
        }

    private:
        void Serve()
        {
            while (true)
            {
                int client = accept(fd, nullptr, nullptr);
                if (client < 0)
                    return;
                std::string request;
                char buffer[4096];
                size_t headerEnd = std::string::npos;
                size_t contentLength = 0;
                while (true)
                {
                    if (headerEnd == std::string::npos && (headerEnd = request.find("\r\n\r\n")) != std::string::npos)
                    {
                        std::string lower = request.substr(0, headerEnd);
                        for (char &c : lower)
                            c = (char)std::tolower((unsigned char)c);
                        if (size_t p = lower.find("content-length:"); p != std::string::npos)
                            contentLength = std::stoul(lower.substr(p + 15));
                    }
                    if (headerEnd != std::string::npos && request.size() >= headerEnd + 4 + contentLength)
                        break;
                    ssize_t n = read(client, buffer, sizeof(buffer));
                    if (n <= 0)
                        break;
                    request.append(buffer, (size_t)n);
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    requests.push_back(request);
                }
                const char *response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
                [[maybe_unused]] ssize_t written = write(client, response, strlen(response));
                close(client);
            }
        }
        int fd = -1;
        int port = 0;
        std::thread thread;
        std::mutex mutex;
        std::vector<std::string> requests;
    };

    template <typename T>
    T ParseJsonAs(const std::string &json)
    {
        T result;
        std::istringstream ss(json);
        json_reader reader(ss);
        reader.read(&result);
        return result;
    }

    std::string ReadFile(const fs::path &path)
    {
        std::ifstream f(path);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // Writes a token file for Tone3000Auth (access token only; never refreshed in these tests).
    void WriteTokenFile(const fs::path &path, const std::string &accessToken)
    {
        std::ofstream f(path);
        json_writer writer(f);
        json_variant v = json_variant::make_object();
        v["access_token"] = accessToken;
        v["refresh_token"] = std::string();
        v["expires_at"] = 4102444800000.0; // 2100-01-01.
        writer.write(v);
        f.close();
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write);
    }
}

TEST_CASE("TONE3000 recorded responses: catalog parsers", "[t3k_api]")
{
    SECTION("search page, with a null images member")
    {
        auto reply = ParseTone3000TonesPage(SEARCH_NULL_IMAGES);
        REQUIRE(reply.ok_);
        REQUIRE(reply.tones_.size() == 1);
        const auto &t = reply.tones_[0];
        REQUIRE(t.id_ == 36381);
        REQUIRE(t.gear_ == "experimental");
        REQUIRE(t.format_ == "ir");
        REQUIRE(t.thumbnail_.empty());
        REQUIRE(t.userName_ == "example-user-10"); // display_name null: username.
        REQUIRE(t.hasFavoriteState_);
        REQUIRE(t.tags_ == std::vector<std::string>{"Example name 3"});
    }
    SECTION("search by gear")
    {
        auto reply = ParseTone3000TonesPage(SEARCH_SPACE);
        REQUIRE(reply.ok_);
        REQUIRE(reply.tones_.size() == 2);
        REQUIRE(reply.page_ == 1);
        REQUIRE(reply.pageSize_ == 25); // the search caps page_size at 25, whatever was asked.
        REQUIRE(reply.total_ == 42);
        for (const auto &t : reply.tones_)
        {
            REQUIRE(t.gear_ == "space");
            REQUIRE(t.format_ == "ir");
            REQUIRE(API_GEARS.contains(t.gear_));
        }
    }
    SECTION("trending has no paging members")
    {
        auto reply = ParseTone3000TonesPage(TRENDING_CAB);
        REQUIRE(reply.ok_);
        REQUIRE(reply.tones_.size() == 2);
        REQUIRE(reply.page_ == 1);
        REQUIRE(reply.totalPages_ == 1);
        REQUIRE(reply.total_ == 2);
        REQUIRE(reply.tones_[0].gear_ == "cab");
    }
    SECTION("models: IR models have no size or architecture")
    {
        auto ir = ParseTone3000ModelsPage(MODELS_IR);
        REQUIRE(ir.ok_);
        REQUIRE(ir.models_.size() == 2);
        REQUIRE(ir.models_[0].id_ == 749548);
        REQUIRE(ir.models_[0].size_.empty());
        auto nam = ParseTone3000ModelsPage(MODELS_NAM);
        REQUIRE(nam.ok_);
        REQUIRE(nam.models_.size() == 2);
        REQUIRE(nam.models_[0].size_ == "standard");
    }
    SECTION("tags")
    {
        auto reply = ParseTone3000Names(TAGS);
        REQUIRE(reply.ok_);
        REQUIRE(reply.names_ == std::vector<std::string>{"Example name 1", "Example name 3"});
    }
}

TEST_CASE("TONE3000 recorded responses: tone info", "[t3k_api]")
{
    // GET /api/v1/tones/{id}: "format" replaced the deprecated "platform"; sizes may be null.
    auto download = ParseJsonAs<Tone3000Download>(TONE_INFO_IR);
    REQUIRE(download.id() == 88860);
    REQUIRE(download.gear() == "space");
    REQUIRE(download.format() == "ir");
    REQUIRE(download.platform().empty());
    REQUIRE(!download.sizes().has_value());

    auto tone = ParseJsonAs<tone3000::Tone>(TONE_INFO_IR);
    REQUIRE(tone.format() == "ir");
    REQUIRE(tone.irs_count() == 2);

    // The web UI hands search results to writeTone3000Readme as they come: images may be null.
    std::string searchTone;
    {
        json_variant page = json_variant::parse(SEARCH_NULL_IMAGES);
        std::ostringstream ss;
        json_writer writer(ss);
        writer.write(page["data"][size_t(0)]);
        searchTone = ss.str();
    }
    auto nullImages = ParseJsonAs<tone3000::Tone>(searchTone);
    REQUIRE(nullImages.images().empty());
    REQUIRE(nullImages.gear() == "experimental");
    REQUIRE(ParseJsonAs<Tone3000Download>(searchTone).images().empty());

    // The README shows the format (it used to read the missing "platform" member).
    TempDir dir;
    tone3000::WriteTone3000Readme(dir.path / "README.md", tone, "");
    std::string readme = ReadFile(dir.path / "README.md");
    REQUIRE(readme.find("Space, IR") != std::string::npos);

    tone3000::Tone legacy;
    legacy.gear("full-rig");
    legacy.platform("nam");
    tone3000::WriteTone3000Readme(dir.path / "LEGACY.md", legacy, "");
    REQUIRE(ReadFile(dir.path / "LEGACY.md").find("Amp + cab, NAM") != std::string::npos);

    tone3000::Tone amp;
    amp.gear("amp-cab");
    amp.format("nam");
    tone3000::WriteTone3000Readme(dir.path / "AMP.md", amp, "");
    REQUIRE(ReadFile(dir.path / "AMP.md").find("Amp + cab, NAM") != std::string::npos);
}

TEST_CASE("TONE3000 IR classification uses the API's gear values", "[t3k_api]")
{
    // Every gear TONE3000 files impulse responses under (format=ir, October 2026: cab, space,
    // pedal, outboard, experimental). Only cab and space are unambiguous.
    REQUIRE(ClassifyIr("cab", 3.0) == IrKind::Cab);
    REQUIRE(ClassifyIr("space", 0.2) == IrKind::Reverb);
    for (const char *gear : {"pedal", "outboard", "experimental", "amp", "amp-cab", "ir", "full-rig", ""})
    {
        CAPTURE(gear);
        REQUIRE(ClassifyIr(gear, 0.3) == IrKind::Cab);
        REQUIRE(ClassifyIr(gear, 2.5) == IrKind::Reverb);
    }
}

TEST_CASE("TONE3000 origin check compares scheme, host and port", "[t3k_api]")
{
    const std::string origin = "https://www.tone3000.com";
    for (const char *url : {
             "https://www.tone3000.com",
             "https://www.tone3000.com/",
             "https://www.tone3000.com/api/v1/user",
             "https://www.tone3000.com?x=1",
             "https://www.tone3000.com#f",
             "https://www.tone3000.com:443/api/v1/user",
             "HTTPS://WWW.TONE3000.COM/api/v1/user",
         })
    {
        CAPTURE(url);
        REQUIRE(IsTone3000SameOrigin(url, origin));
    }
    for (const char *url : {
             "http://www.tone3000.com/api/v1/user",    // scheme.
             "https://www.tone3000.com:8443/api",      // port.
             "https://tone3000.com/api",               // host.
             "https://www.tone3000.com.evil.example/", // suffix.
             "https://www.tone3000.comevil.example/",
             "https://www.tone3000.com@evil.example/", // user info.
             "https://user@www.tone3000.com/",
             "https://evil.example\\@www.tone3000.com/",
             "https://evil.example/https://www.tone3000.com",
             "https://www.tone3000.com%2eevil.example/",
             "https://www.tone3000.com./api",
             "https://www.tone3000.com:/api",
             "https://www.tone3000.com:0/api",
             "https://www.tone3000.com:99999/api",
             "https://www.tone3000.com:44a3/api",
             "https://www.tone3000.com /api",
             "https://www.tone3000.com\t/api",
             "//www.tone3000.com/api",
             "/api/v1/user",
             "ftp://www.tone3000.com/",
             "https:///www.tone3000.com/",
             "",
         })
    {
        CAPTURE(url);
        REQUIRE(!IsTone3000SameOrigin(url, origin));
    }
    REQUIRE(IsTone3000SameOrigin("http://127.0.0.1:8080/x", "http://127.0.0.1:8080"));
    REQUIRE(!IsTone3000SameOrigin("http://127.0.0.1:8081/x", "http://127.0.0.1:8080"));
    REQUIRE(IsTone3000SameOrigin("http://t3k.test:80/x", "http://t3k.test"));
}

TEST_CASE("Curl requests can be cancelled", "[t3k_api]")
{
    if (!fs::exists("/usr/bin/curl"))
    {
        WARN("Skipped: /usr/bin/curl is not installed.");
        return;
    }
    TempDir dir;
    CurlTempDirectoryScope tempScope(dir.path);
    SilentServer server;

    CurlCancellation cancellation;
    auto start = std::chrono::steady_clock::now();
    std::thread canceller([&cancellation]()
                          {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancellation.Cancel(); });
    REQUIRE_THROWS(CurlGet(server.Origin() + "/x", dir.path / "out", nullptr, nullptr, 30, &cancellation));
    canceller.join();
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    REQUIRE(cancellation.IsCancelled());

    // Later requests fail at once.
    start = std::chrono::steady_clock::now();
    std::string body;
    REQUIRE_THROWS(CurlPostStrings(server.Origin() + "/x", "a=b", body, nullptr, nullptr, 30, &cancellation));
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));

    // Not cancelled: requests complete normally, with and without a cancellation object.
    AnsweringServer answering;
    CurlCancellation unused;
    std::vector<std::string> headers{"X-Test: one two"};
    REQUIRE(CurlGet(answering.Origin() + "/get?a=1&b=2", dir.path / "out", nullptr, &headers, 5, &unused) == 200);
    REQUIRE(ReadFile(dir.path / "out") == "ok");
    REQUIRE(CurlRequest("PUT", answering.Origin() + "/put", dir.path / "out", nullptr, &headers, 5, &unused) == 200);
    body.clear();
    REQUIRE(CurlPostStrings(answering.Origin() + "/post", "a=b c'd", body, nullptr, &headers, 5, &unused) == 200);
    REQUIRE(body == "ok");
    REQUIRE(CurlGet(answering.Origin() + "/plain", dir.path / "out", nullptr, &headers, 5, nullptr) == 200);
    REQUIRE(!unused.IsCancelled());
    auto requests = answering.Requests();
    REQUIRE(requests.size() == 4);
    REQUIRE(requests[0].starts_with("GET /get?a=1&b=2 HTTP/1.1\r\n"));
    REQUIRE(requests[1].starts_with("PUT /put HTTP/1.1\r\n"));
    REQUIRE(requests[2].starts_with("POST /post HTTP/1.1\r\n"));
    REQUIRE(requests[2].ends_with("\r\n\r\na=b c'd")); // --data-binary @file, as one argument.
    REQUIRE(requests[3].starts_with("GET /plain HTTP/1.1\r\n"));
    for (const auto &request : requests)
    {
        REQUIRE(request.find("\r\nX-Test: one two\r\n") != std::string::npos); // -H @file.
    }

    // A refused connection is an error, not a cancellation.
    REQUIRE_THROWS(CurlGet(SS("http://127.0.0.1:1/x"), dir.path / "out", nullptr, nullptr, 5, &unused));
    REQUIRE(!unused.IsCancelled());
}

namespace
{
    std::string FdLink(const fs::path &fdPath)
    {
        std::error_code ec;
        auto target = fs::read_symlink(fdPath, ec);
        return ec ? std::string() : target.string();
    }

    // Our child processes named curl.
    std::vector<pid_t> CurlChildren()
    {
        std::vector<pid_t> result;
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator("/proc", ec))
        {
            std::string name = entry.path().filename().string();
            if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
                continue;
            std::ifstream f(entry.path() / "stat");
            std::string stat;
            std::getline(f, stat);
            // pid (comm) state ppid ...
            auto close = stat.rfind(')');
            if (close == std::string::npos || stat.find("(curl)") == std::string::npos)
                continue;
            std::istringstream rest(stat.substr(close + 1));
            std::string state;
            pid_t ppid = 0;
            rest >> state >> ppid;
            if (ppid == ::getpid())
                result.push_back((pid_t)std::stol(name));
        }
        return result;
    }
}

TEST_CASE("Curl does not inherit descriptors opened without O_CLOEXEC", "[t3k_api]")
{
    if (!fs::exists("/usr/bin/curl"))
    {
        WARN("Skipped: /usr/bin/curl is not installed.");
        return;
    }
    bool haveClosefrom = false; // as in Curl.cpp
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
    haveClosefrom = true;
#endif
#endif
    if (!haveClosefrom)
    {
        WARN("Skipped: needs posix_spawn_file_actions_addclosefrom_np (glibc 2.34).");
        return;
    }
    TempDir dir;
    CurlTempDirectoryScope tempScope(dir.path);
    SilentServer server;

    int leaky = ::socket(AF_INET, SOCK_STREAM, 0); // no SOCK_CLOEXEC, like a listening socket.
    REQUIRE(leaky >= 0);
    std::string leakyLink = FdLink(SS("/proc/self/fd/" << leaky));
    REQUIRE(!leakyLink.empty());

    // While curl waits on the silent server, record its descriptors, then cancel it.
    CurlCancellation cancellation;
    std::vector<std::string> childFds;
    std::string childStdout, childStderr;
    bool found = false;
    std::thread inspector([&]()
                          {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!found && std::chrono::steady_clock::now() < deadline)
        {
            auto children = CurlChildren();
            if (children.size() == 1)
            {
                fs::path fdDir = SS("/proc/" << children[0] << "/fd");
                std::error_code ec;
                for (const auto &entry : fs::directory_iterator(fdDir, ec))
                {
                    childFds.push_back(FdLink(entry.path()));
                }
                childStdout = FdLink(fdDir / "1");
                childStderr = FdLink(fdDir / "2");
                found = !childFds.empty();
            }
            if (!found)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        cancellation.Cancel(); });
    REQUIRE_THROWS(CurlGet(server.Origin() + "/x", dir.path / "out", nullptr, nullptr, 30, &cancellation));
    inspector.join();
    ::close(leaky);

    REQUIRE(found);
    CAPTURE(childFds);
    REQUIRE(std::find(childFds.begin(), childFds.end(), leakyLink) == childFds.end());
    // stdout and stderr still go to the output pipe.
    REQUIRE(childStdout.starts_with("pipe:"));
    REQUIRE(childStdout == childStderr);
}

TEST_CASE("TONE3000 Close aborts requests in flight", "[t3k_api]")
{
    if (!fs::exists("/usr/bin/curl"))
    {
        WARN("Skipped: /usr/bin/curl is not installed.");
        return;
    }
    TempDir dir;
    CurlTempDirectoryScope tempScope(dir.path);
    SilentServer server;
    WriteTokenFile(dir.path / "tokens.json", "test-access-token");
    auto auth = Tone3000Auth::Create(dir.path / "tokens.json", Tone3000Auth::DefaultDependencies(), server.Origin(), "client");
    REQUIRE(auth->IsSignedIn());

    // Shared with the detached thread, which may outlive this scope if Close() fails.
    struct Outcome
    {
        std::atomic<bool> finished{false};
        std::atomic<int> status{-1};
    };
    auto outcome = std::make_shared<Outcome>();
    {
        auto operation = auth->BeginOperation();
        std::thread([auth, operation, outcome]() mutable
                    {
            try {
                outcome->status = auth->AuthorizedGet("/api/v1/tones/1").status;
            } catch (const std::exception &) {
                outcome->status = -2;
            }
            outcome->finished = true;
            operation.reset(); })
            .detach();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // curl is waiting on the server.
    REQUIRE(!outcome->finished);

    auto start = std::chrono::steady_clock::now();
    REQUIRE(auth->Close()); // didn't time out.
    REQUIRE(std::chrono::steady_clock::now() - start < Tone3000Auth::CLOSE_TIMEOUT);
    REQUIRE(outcome->finished);
    REQUIRE(outcome->status == 0); // no answer: the transport failed.
}

namespace
{
    std::optional<std::map<std::string, std::string>> ReadLiveCredentials()
    {
        const char *home = getenv("HOME");
        if (!home)
            return std::nullopt;
        std::ifstream f(fs::path(home) / ".env.tone3000");
        if (!f)
            return std::nullopt;
        std::map<std::string, std::string> result;
        std::string line;
        while (std::getline(f, line))
        {
            auto trim = [](std::string s)
            {
                size_t a = s.find_first_not_of(" \t\r\n");
                size_t b = s.find_last_not_of(" \t\r\n");
                return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
            };
            line = trim(line);
            if (line.empty() || line[0] == '#')
                continue;
            if (line.starts_with("export "))
                line = trim(line.substr(7));
            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string value = trim(line.substr(eq + 1));
            if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
                value = value.substr(1, value.size() - 2);
            result[trim(line.substr(0, eq))] = value;
        }
        return result;
    }

    // A signed-in session whose access token is the account's secret key (a server-side bearer
    // credential that the API accepts in place of an OAuth access token).
    struct LiveSession
    {
        TempDir dir;
        CurlTempDirectoryScope tempScope{dir.path};
        std::shared_ptr<Tone3000Auth> auth;
        std::map<std::string, std::string> credentials;

        bool Open()
        {
            auto c = ReadLiveCredentials();
            if (!c || (*c)["SECRET_KEY"].empty())
            {
                WARN("Skipped: no SECRET_KEY in ~/.env.tone3000.");
                return false;
            }
            credentials = *c;
            WriteTokenFile(dir.path / "tokens.json", credentials["SECRET_KEY"]);
            auth = Tone3000Auth::Create(dir.path / "tokens.json");
            return auth->IsSignedIn();
        }
        ~LiveSession()
        {
            if (auth)
                auth->Close();
        }
    };

    Tone3000CatalogQuery LiveQuery(const std::string &gear = "", const std::string &format = "")
    {
        Tone3000CatalogQuery q;
        q.gear_ = gear;
        q.format_ = format;
        q.pageSize_ = 10;
        return q;
    }

    void RequireTones(const Tone3000CatalogTonesReply &reply)
    {
        CAPTURE(reply.error_);
        REQUIRE(reply.ok_);
        REQUIRE(!reply.tones_.empty());
        for (const auto &t : reply.tones_)
        {
            CAPTURE(t.id_, t.gear_, t.format_);
            REQUIRE(t.id_ > 0);
            REQUIRE(!t.title_.empty());
            REQUIRE(API_GEARS.contains(t.gear_));
            REQUIRE((t.format_ == "nam" || t.format_ == "ir" || t.format_ == "aida-x" || t.format_ == "aa-snapshot" || t.format_ == "proteus"));
            REQUIRE(t.url_.starts_with("https://"));
        }
    }
}

TEST_CASE("TONE3000 live: catalog endpoints and response shapes", "[.][t3k_live]")
{
    LiveSession live;
    if (!live.Open())
        return;
    Tone3000Catalog catalog(live.auth);

    auto all = catalog.Search(LiveQuery());
    RequireTones(all);
    REQUIRE(all.signedIn_);
    REQUIRE(all.total_ > 0);
    REQUIRE(all.totalPages_ >= 1);
    REQUIRE(all.tones_[0].hasFavoriteState_);

    // Each gear filter returns that gear; the deprecated aliases still work.
    for (const char *gear : {"amp", "amp-cab", "pedal", "cab", "space"})
    {
        CAPTURE(gear);
        auto reply = catalog.Search(LiveQuery(gear));
        RequireTones(reply);
        for (const auto &t : reply.tones_)
            REQUIRE(t.gear_ == gear);
    }
    auto fullRig = catalog.Search(LiveQuery("full-rig"));
    RequireTones(fullRig);
    for (const auto &t : fullRig.tones_)
        REQUIRE(t.gear_ == "amp-cab");
    auto irs = catalog.Search(LiveQuery("ir"));
    RequireTones(irs);
    for (const auto &t : irs.tones_)
        REQUIRE(t.format_ == "ir");

    // Trending: signed in, and anonymous.
    RequireTones(catalog.Trending(""));
    auto anonymous = live.auth->AnonymousGet(BuildTone3000TrendingPath("cab"));
    REQUIRE(anonymous.status == 200);
    auto trending = ParseTone3000TonesPage(anonymous.body);
    RequireTones(trending);
    for (const auto &t : trending.tones_)
        REQUIRE(t.gear_ == "cab");

    // The user's own lists, unfiltered and with the gear chips the UI offers there (including
    // "ir", which these endpoints accept as a gear).
    for (const char *kind : {"favorited", "downloaded"})
    {
        for (const char *gear : {"", "ir", "cab", "amp-cab", "full-rig"})
        {
            auto q = LiveQuery(gear);
            q.kind_ = kind;
            CAPTURE(kind, gear, BuildTone3000TonesPath(q));
            auto response = live.auth->AuthorizedGet(BuildTone3000TonesPath(q));
            REQUIRE(response.status == 200);
            auto reply = ParseTone3000TonesPage(response.body);
            CAPTURE(reply.error_);
            REQUIRE(reply.ok_);
            for (const auto &t : reply.tones_)
            {
                if (std::string(gear) == "ir")
                    REQUIRE(t.format_ == "ir");
                else if (*gear)
                    REQUIRE(t.gear_ == (std::string(gear) == "full-rig" ? std::string("amp-cab") : std::string(gear)));
            }
        }
    }

    // Models of a NAM tone and of an IR tone.
    for (const auto &tone : {all.tones_[0], irs.tones_[0]})
    {
        Tone3000CatalogModelsQuery mq;
        mq.toneId_ = tone.id_;
        auto models = catalog.Models(mq);
        CAPTURE(tone.id_, models.error_);
        REQUIRE(models.ok_);
        REQUIRE(!models.models_.empty());
        for (const auto &m : models.models_)
        {
            REQUIRE(m.id_ > 0);
            REQUIRE(!m.name_.empty());
        }
    }

    // Tone info, as the downloader and the README writer read it.
    auto info = live.auth->AuthorizedGet(SS("/api/v1/tones/" << irs.tones_[0].id_));
    REQUIRE(info.status == 200);
    auto download = ParseJsonAs<Tone3000Download>(info.body);
    REQUIRE(download.id() == irs.tones_[0].id_);
    REQUIRE(download.format() == "ir");
    REQUIRE(API_GEARS.contains(download.gear()));
    auto tone = ParseJsonAs<tone3000::Tone>(info.body);
    REQUIRE(tone.format() == "ir");

    // Tags and makes.
    Tone3000CatalogTaxonomyQuery tq;
    tq.pageSize_ = 5;
    for (const char *kind : {"tags", "makes"})
    {
        auto names = catalog.Taxonomy(kind, tq);
        CAPTURE(kind, names.error_);
        REQUIRE(names.ok_);
        REQUIRE(!names.names_.empty());
    }
}

TEST_CASE("TONE3000 live: gear values", "[.][t3k_live]")
{
    LiveSession live;
    if (!live.Open())
        return;
    // The API lists its gear values when it rejects one.
    auto response = live.auth->AuthorizedGet("/api/v1/tones/downloaded?page=1&page_size=1&gear=not-a-gear");
    REQUIRE(response.status == 400);
    json_variant body = json_variant::parse(response.body);
    std::string error = body["error"].as_string();
    CAPTURE(error);
    size_t colon = error.find(':');
    REQUIRE(colon != std::string::npos);
    std::set<std::string> gears;
    std::stringstream list(error.substr(colon + 1));
    std::string gear;
    while (std::getline(list, gear, ','))
    {
        size_t a = gear.find_first_not_of(' ');
        gears.insert(gear.substr(a));
    }
    REQUIRE(gears == API_GEARS);

    // IR classification premise: impulse responses are filed under cab and space, but also under
    // the general-purpose gears, so only cab and space decide the kind on their own.
    auto irs = Tone3000Catalog(live.auth).Search(LiveQuery("", "ir"));
    RequireTones(irs);
    for (const auto &t : irs.tones_)
        REQUIRE((t.gear_ == "cab" || t.gear_ == "space" || t.gear_ == "pedal" || t.gear_ == "outboard" || t.gear_ == "experimental"));
}

TEST_CASE("TONE3000 live: device authorization with the publishable key", "[.][t3k_live]")
{
    auto credentials = ReadLiveCredentials();
    if (!credentials)
    {
        WARN("Skipped: no ~/.env.tone3000.");
        return;
    }
    TempDir dir;
    CurlTempDirectoryScope tempScope(dir.path);
    auto deps = Tone3000Auth::DefaultDependencies();
    std::vector<std::string> clientIds{PIPEDAL_T3K_PUBLISHABLE_KEY};
    if (!(*credentials)["PUBLISHABLE_KEY"].empty())
        clientIds.push_back((*credentials)["PUBLISHABLE_KEY"]);
    for (size_t i = 0; i < clientIds.size(); ++i)
    {
        CAPTURE(i); // 0: the compiled-in key; 1: the one in ~/.env.tone3000.
        auto response = deps.postForm(std::string(Tone3000Auth::DEFAULT_ORIGIN) + "/api/v1/oauth/device_authorization",
                                      HtmlFormBuilder({{"client_id", clientIds[i]}}).build());
        REQUIRE(response.status == 200);
        auto authorization = ParseTone3000DeviceAuthorization(response.body);
        REQUIRE(authorization.has_value());
        REQUIRE(authorization->verificationUri.starts_with("https://www.tone3000.com/"));
        REQUIRE(authorization->verificationUriComplete.starts_with(authorization->verificationUri));
        REQUIRE(authorization->intervalS >= 1);
        // Polling once must answer authorization_pending (nobody approves it): the device-code
        // grant is enabled for this client id.
        auto poll = deps.postForm(std::string(Tone3000Auth::DEFAULT_ORIGIN) + "/api/v1/oauth/token",
                                  HtmlFormBuilder({{"grant_type", "urn:ietf:params:oauth:grant-type:device_code"},
                                                   {"device_code", authorization->deviceCode},
                                                   {"client_id", clientIds[i]}})
                                      .build());
        REQUIRE(ParseTone3000OAuthError(poll.body) == "authorization_pending");
    }

    // The secret key is a bearer credential, not a client id: never needed for sign-in.
    // An unknown client id is refused (so the UI falls back as designed).
    auto refused = deps.postForm(std::string(Tone3000Auth::DEFAULT_ORIGIN) + "/api/v1/oauth/device_authorization",
                                 HtmlFormBuilder({{"client_id", std::string("t3k_pub_not-a-real-client-id")}}).build());
    CAPTURE(refused.status);
    REQUIRE(refused.status >= 400);
}
