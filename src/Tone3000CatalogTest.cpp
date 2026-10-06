// Copyright (c) 2026 Robin Davies
// MIT license; see other source files in this project.
//
// TONE3000 catalog proxy: query-string building, parameter whitelisting, response parsing
// (fixtures shaped like the API's PaginatedResponse<Tone> / Model) and the proxy calls,
// all against a scripted fake server. No network access.

#include "pch.h"
#include "catch.hpp"
#include "Tone3000Catalog.hpp"
#include "Tone3000Auth.hpp"
#include "json.hpp"
#include "ss.hpp"
#include <deque>
#include <sstream>
#include <unistd.h>

using namespace pipedal;
namespace fs = std::filesystem;

namespace
{
    const char *ORIGIN = "https://t3k.test";

    const char *TONES_PAGE = R"JSON({
        "data": [
            {
                "id": 101,
                "user_id": "u-1",
                "user": {"id": "u-1", "username": "capturer", "display_name": "  Pro Capturer ", "is_verified": true,
                         "avatar_url": null, "url": "https://www.tone3000.com/capturer"},
                "created_at": "2025-01-01T00:00:00Z",
                "title": "Plexi \"Lead\" & Crunch",
                "description": null,
                "gear": "amp",
                "images": ["https://cdn.t3k.test/101.jpg", "https://cdn.t3k.test/101b.jpg"],
                "is_public": true,
                "links": null,
                "format": "nam",
                "license": "t3k",
                "sizes": ["standard", "lite"],
                "makes": [{"id": 3, "name": "Marshall"}, {"name": "  "}],
                "tags": [{"name": "crunch"}, "lead"],
                "models_count": 12,
                "a1_models_count": 6,
                "a2_models_count": 6,
                "downloads_count": 3456,
                "favorites_count": 78,
                "is_favorite": true,
                "url": "https://www.tone3000.com/tones/plexi-101"
            },
            {
                "id": 202,
                "user": {"id": "u-2", "username": "irmaker", "display_name": null},
                "title": "4x12 V30",
                "gear": "ir",
                "images": ["javascript:alert(1)"],
                "platform": "ir",
                "makes": [],
                "tags": null,
                "models_count": 3,
                "downloads_count": 9,
                "favorites_count": 0,
                "url": "http://insecure.example/202"
            },
            {"title": "no id: dropped"},
            "not an object",
            {"id": -4, "title": "bad id: dropped"}
        ],
        "page": 2,
        "page_size": 25,
        "total": 51,
        "total_pages": 3
    })JSON";

    const char *MODELS_PAGE = R"({
        "data": [
            {"id": 9001, "created_at": "x", "updated_at": "y", "user_id": "u-1",
             "model_url": "https://www.tone3000.com/api/v1/models/9001/download", "name": "Gain 5", "size": "standard", "tone_id": 101},
            {"id": 9002, "model_url": "https://x", "name": "Gain 7", "size": null, "tone_id": 101},
            {"name": "no id"}
        ],
        "page": 1, "page_size": 50, "total": 2, "total_pages": 1
    })";

    struct Request
    {
        std::string method;
        std::string url;
        std::vector<std::string> headers;
    };

    Tone3000HttpResponse Http(int status, const std::string &body)
    {
        Tone3000HttpResponse r;
        r.status = status;
        r.body = body;
        return r;
    }

    struct FakeServer
    {
        int64_t now = 1'000'000'000'000;
        std::deque<Tone3000HttpResponse> posts;
        std::deque<Tone3000HttpResponse> responses; // GET, PUT and DELETE.
        std::vector<Request> requests;

        Tone3000Auth::Dependencies Deps()
        {
            Tone3000Auth::Dependencies d;
            d.postForm = [this](const std::string &url, const std::string &body)
            {
                requests.push_back({"POST", url, {}});
                REQUIRE(!posts.empty());
                auto r = posts.front();
                posts.pop_front();
                return r;
            };
            d.get = [this](const std::string &url, const std::vector<std::string> &headers)
            {
                return Next("GET", url, headers);
            };
            d.request = [this](const std::string &method, const std::string &url, const std::vector<std::string> &headers)
            {
                return Next(method, url, headers);
            };
            d.nowMs = [this]()
            { return now; };
            d.wait = [this](std::chrono::milliseconds ms)
            {
                now += ms.count();
                return true;
            };
            return d;
        }
        Tone3000HttpResponse Next(const std::string &method, const std::string &url, const std::vector<std::string> &headers)
        {
            requests.push_back({method, url, headers});
            REQUIRE(!responses.empty());
            auto r = responses.front();
            responses.pop_front();
            return r;
        }
    };

    struct TempDir
    {
        fs::path path;
        TempDir()
        {
            char tmpl[] = "/tmp/t3kcatalogtestXXXXXX";
            path = mkdtemp(tmpl);
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    std::shared_ptr<Tone3000Auth> SignedIn(FakeServer &server, const fs::path &dir)
    {
        server.posts = {
            Http(200, R"({"device_code":"d","user_code":"U","verification_uri":"https://t3k.test/device","interval":1})"),
            Http(200, R"({"access_token":"access-1","refresh_token":"refresh-1","expires_in":3600})")};
        auto auth = Tone3000Auth::Create(dir / "t.json", server.Deps(), ORIGIN, "c");
        auth->RunDeviceFlow();
        REQUIRE(auth->IsSignedIn());
        server.requests.clear();
        return auth;
    }

    Tone3000CatalogQuery Query(const std::string &text = "")
    {
        Tone3000CatalogQuery q;
        q.query_ = text;
        return q;
    }

    template <typename T>
    std::string ToJson(const T &value)
    {
        std::stringstream s;
        json_writer writer(s, true);
        writer.write(value);
        return s.str();
    }
}

TEST_CASE("TONE3000 catalog percent-encoding", "[t3k_catalog]")
{
    using t3k_catalog::PercentEncode;
    REQUIRE(PercentEncode("") == "");
    REQUIRE(PercentEncode("AZaz09-._~") == "AZaz09-._~");
    REQUIRE(PercentEncode("a b&c=d?e/f#g+h%") == "a%20b%26c%3Dd%3Fe%2Ff%23g%2Bh%25");
    REQUIRE(PercentEncode("Ünï,") == "%C3%9Cn%C3%AF%2C");
    REQUIRE(PercentEncode("\"'<>") == "%22%27%3C%3E");
}

TEST_CASE("TONE3000 catalog search query string", "[t3k_catalog]")
{
    // Defaults: page 1, 25 a page; no text so the API's default sort (trending) is left implicit.
    REQUIRE(BuildTone3000TonesPath(Query()) == "/api/v1/tones/search?page=1&page_size=25");

    // Text is trimmed and encoded.
    REQUIRE(BuildTone3000TonesPath(Query("  plexi & co ")) == "/api/v1/tones/search?page=1&page_size=25&query=plexi%20%26%20co");

    SECTION("default sort rule")
    {
        auto q = Query("plexi");
        q.sort_ = "bestMatch"; // the default with text: not sent.
        REQUIRE(BuildTone3000TonesPath(q) == "/api/v1/tones/search?page=1&page_size=25&query=plexi");
        q.sort_ = "trending";
        REQUIRE(BuildTone3000TonesPath(q) == "/api/v1/tones/search?page=1&page_size=25&query=plexi&sort=trending");
        q.sort_ = "popular";
        REQUIRE(BuildTone3000TonesPath(q) == "/api/v1/tones/search?page=1&page_size=25&query=plexi&sort=downloads-all-time");

        q = Query();
        q.sort_ = "trending"; // the default without text: not sent.
        REQUIRE(BuildTone3000TonesPath(q) == "/api/v1/tones/search?page=1&page_size=25");
        q.sort_ = "bestMatch"; // meaningless without text: the default.
        REQUIRE(BuildTone3000TonesPath(q) == "/api/v1/tones/search?page=1&page_size=25");
        q.sort_ = "newest";
        REQUIRE(BuildTone3000TonesPath(q) == "/api/v1/tones/search?page=1&page_size=25&sort=newest");
        q.sort_ = "oldest";
        REQUIRE(BuildTone3000TonesPath(q) == "/api/v1/tones/search?page=1&page_size=25&sort=oldest");
    }
    SECTION("filters and joins")
    {
        Tone3000CatalogQuery q = Query("x");
        q.page_ = 3;
        q.pageSize_ = 40;
        q.gear_ = "full-rig";
        q.format_ = "nam";
        q.tags_ = {"high gain", "metal"};
        q.makes_ = {"Mesa/Boogie", "EVH"};
        q.creators_ = {"some_user", "other user"};
        q.calibrated_ = true;
        q.verified_ = true;
        q.architecture_ = 2;
        REQUIRE(BuildTone3000TonesPath(q) ==
                "/api/v1/tones/search?page=3&page_size=40&query=x&gears=amp-cab&format=nam"
                "&tags=high%20gain_metal&makes=Mesa%2FBoogie_EVH&creators=some_user,other%20user"
                "&calibrated=true&verified=true&architecture=2");
    }
    SECTION("calibrated does not apply to impulse responses")
    {
        auto q = Query();
        q.calibrated_ = true;
        REQUIRE(BuildTone3000TonesPath(q) == "/api/v1/tones/search?page=1&page_size=25&calibrated=true");
        // "ir" is a format, not a gear (the API deprecated gears=ir).
        q.gear_ = "ir";
        REQUIRE(BuildTone3000TonesPath(q) == "/api/v1/tones/search?page=1&page_size=25&format=ir");
        q.format_ = "ir";
        REQUIRE(BuildTone3000TonesPath(q) == "/api/v1/tones/search?page=1&page_size=25&format=ir");
        q.format_ = "";
        // Cab and space are IR-only gear.
        q.gear_ = "cab";
        REQUIRE(BuildTone3000TonesPath(q) == "/api/v1/tones/search?page=1&page_size=25&gears=cab");
        q.gear_ = "space";
        REQUIRE(BuildTone3000TonesPath(q) == "/api/v1/tones/search?page=1&page_size=25&gears=space");
        q.gear_ = "amp-cab";
        REQUIRE(BuildTone3000TonesPath(q) == "/api/v1/tones/search?page=1&page_size=25&gears=amp-cab&calibrated=true");
        q.gear_ = "";
        q.format_ = "ir";
        REQUIRE(BuildTone3000TonesPath(q) == "/api/v1/tones/search?page=1&page_size=25&format=ir");
    }
    SECTION("the user's own lists take title and gear only")
    {
        auto q = Query("amp");
        q.kind_ = "favorited";
        q.gear_ = "pedal";
        q.sort_ = "newest";
        q.tags_ = {"t"};
        q.verified_ = true;
        REQUIRE(BuildTone3000TonesPath(q) == "/api/v1/tones/favorited?page=1&page_size=25&query=amp&gear=pedal");
        q.kind_ = "downloaded";
        q.query_ = "";
        q.page_ = 2;
        REQUIRE(BuildTone3000TonesPath(q) == "/api/v1/tones/downloaded?page=2&page_size=25&gear=pedal");
        q.gear_ = "full-rig"; // deprecated alias.
        REQUIRE(BuildTone3000TonesPath(q) == "/api/v1/tones/downloaded?page=2&page_size=25&gear=amp-cab");
        q.gear_ = "ir"; // the lists accept it.
        REQUIRE(BuildTone3000TonesPath(q) == "/api/v1/tones/downloaded?page=2&page_size=25&gear=ir");
    }
}

TEST_CASE("TONE3000 catalog parameter validation", "[t3k_catalog]")
{
    auto bad = [](const std::function<void(Tone3000CatalogQuery &)> &edit)
    {
        Tone3000CatalogQuery q = Query("x");
        edit(q);
        REQUIRE_THROWS_AS(BuildTone3000TonesPath(q), std::invalid_argument);
    };
    bad([](auto &q) { q.kind_ = "created"; });
    bad([](auto &q) { q.kind_ = "search/../../user"; });
    bad([](auto &q) { q.sort_ = "downloads-all-time"; }); // UI names only.
    bad([](auto &q) { q.sort_ = "trending&x=1"; });
    bad([](auto &q) { q.gear_ = "bogus"; });
    bad([](auto &q) { q.gear_ = "Amp"; });
    bad([](auto &q) { q.gear_ = "ir"; q.format_ = "nam"; }); // contradictory.
    bad([](auto &q) { q.gear_ = "amp&page=9"; });
    bad([](auto &q) { q.format_ = "proteus"; });
    bad([](auto &q) { q.page_ = 0; });
    bad([](auto &q) { q.page_ = t3k_catalog::MAX_PAGE + 1; });
    bad([](auto &q) { q.pageSize_ = 0; });
    bad([](auto &q) { q.pageSize_ = t3k_catalog::MAX_PAGE_SIZE + 1; });
    bad([](auto &q) { q.architecture_ = 3; });
    bad([](auto &q) { q.query_ = std::string(t3k_catalog::MAX_QUERY_LENGTH + 1, 'a'); });
    bad([](auto &q) { q.query_ = "a\nb"; });
    bad([](auto &q) { q.query_ = std::string("a\0b", 3); });
    bad([](auto &q) { q.tags_ = {"a_b"}; });   // would split.
    bad([](auto &q) { q.makes_ = {"x_y"}; });
    bad([](auto &q) { q.creators_ = {"a,b"}; });
    bad([](auto &q) { q.tags_ = {"  "}; });
    bad([](auto &q) { q.tags_ = std::vector<std::string>(t3k_catalog::MAX_FILTER_VALUES + 1, "t"); });
    bad([](auto &q) { q.tags_ = {std::string(t3k_catalog::MAX_FILTER_VALUE_LENGTH + 1, 't')}; });

    // At the bounds.
    Tone3000CatalogQuery q = Query(std::string(t3k_catalog::MAX_QUERY_LENGTH, 'a'));
    q.page_ = t3k_catalog::MAX_PAGE;
    q.pageSize_ = t3k_catalog::MAX_PAGE_SIZE;
    q.tags_ = std::vector<std::string>(t3k_catalog::MAX_FILTER_VALUES, "t");
    REQUIRE_NOTHROW(BuildTone3000TonesPath(q));

    // Trending.
    REQUIRE(BuildTone3000TrendingPath("") == "/api/v1/tones/trending");
    REQUIRE(BuildTone3000TrendingPath("outboard") == "/api/v1/tones/trending?gear=outboard");
    REQUIRE(BuildTone3000TrendingPath("full-rig") == "/api/v1/tones/trending?gear=amp-cab");
    REQUIRE(BuildTone3000TrendingPath("ir") == "/api/v1/tones/trending?gear=ir"); // accepted there.
    for (const char *gear : {"amp", "amp-cab", "pedal", "outboard", "cab", "space", "experimental"})
    {
        REQUIRE(BuildTone3000TrendingPath(gear) == std::string("/api/v1/tones/trending?gear=") + gear);
    }
    REQUIRE_THROWS_AS(BuildTone3000TrendingPath("../user"), std::invalid_argument);

    // Favourites.
    REQUIRE(BuildTone3000FavoritePath(42) == "/api/v1/tones/42/favorite");
    REQUIRE_THROWS_AS(BuildTone3000FavoritePath(0), std::invalid_argument);
    REQUIRE_THROWS_AS(BuildTone3000FavoritePath(-1), std::invalid_argument);
    REQUIRE_THROWS_AS(BuildTone3000FavoritePath(t3k_catalog::MAX_TONE_ID + 1), std::invalid_argument);

    // Models.
    Tone3000CatalogModelsQuery m;
    m.toneId_ = 101;
    REQUIRE(BuildTone3000ModelsPath(m) == "/api/v1/models?tone_id=101&page=1&page_size=50");
    m.architecture_ = 2;
    m.page_ = 2;
    m.pageSize_ = 100;
    REQUIRE(BuildTone3000ModelsPath(m) == "/api/v1/models?tone_id=101&page=2&page_size=100&architecture=2");
    m.toneId_ = 0;
    REQUIRE_THROWS_AS(BuildTone3000ModelsPath(m), std::invalid_argument);
    m.toneId_ = 1;
    m.architecture_ = -1;
    REQUIRE_THROWS_AS(BuildTone3000ModelsPath(m), std::invalid_argument);
    m.architecture_ = 0;
    m.pageSize_ = 1000;
    REQUIRE_THROWS_AS(BuildTone3000ModelsPath(m), std::invalid_argument);

    // Tags and makes.
    Tone3000CatalogTaxonomyQuery t;
    t.query_ = "high gain";
    REQUIRE(BuildTone3000TaxonomyPath("tags", t) == "/api/v1/tags?page=1&page_size=50&query=high%20gain");
    t.query_ = "";
    t.pageSize_ = 20;
    REQUIRE(BuildTone3000TaxonomyPath("makes", t) == "/api/v1/makes?page=1&page_size=20");
    REQUIRE_THROWS_AS(BuildTone3000TaxonomyPath("users", t), std::invalid_argument);
    t.pageSize_ = 0;
    REQUIRE_THROWS_AS(BuildTone3000TaxonomyPath("tags", t), std::invalid_argument);
}

TEST_CASE("TONE3000 catalog response parsing", "[t3k_catalog]")
{
    auto page = ParseTone3000TonesPage(TONES_PAGE);
    REQUIRE(page.ok_);
    REQUIRE(page.page_ == 2);
    REQUIRE(page.pageSize_ == 25);
    REQUIRE(page.total_ == 51);
    REQUIRE(page.totalPages_ == 3);
    REQUIRE(page.tones_.size() == 2); // rows without a valid id are dropped.

    const auto &plexi = page.tones_[0];
    REQUIRE(plexi.id_ == 101);
    REQUIRE(plexi.title_ == "Plexi \"Lead\" & Crunch");
    REQUIRE(plexi.description_ == "");
    REQUIRE(plexi.gear_ == "amp");
    REQUIRE(plexi.format_ == "nam");
    REQUIRE(plexi.thumbnail_ == "https://cdn.t3k.test/101.jpg");
    REQUIRE(plexi.userName_ == "Pro Capturer");
    REQUIRE(plexi.userVerified_);
    REQUIRE(plexi.modelsCount_ == 12);
    REQUIRE(plexi.a2ModelsCount_ == 6);
    REQUIRE(plexi.favoritesCount_ == 78);
    REQUIRE(plexi.downloadsCount_ == 3456);
    REQUIRE(plexi.hasFavoriteState_);
    REQUIRE(plexi.isFavorite_);
    REQUIRE(plexi.makes_ == std::vector<std::string>{"Marshall"});
    REQUIRE(plexi.tags_ == std::vector<std::string>{"crunch", "lead"});
    REQUIRE(plexi.sizes_ == std::vector<std::string>{"standard", "lite"});
    REQUIRE(plexi.license_ == "t3k");
    REQUIRE(plexi.url_ == "https://www.tone3000.com/tones/plexi-101");

    const auto &ir = page.tones_[1];
    REQUIRE(ir.id_ == 202);
    REQUIRE(ir.format_ == "ir"); // from the deprecated platform.
    REQUIRE(ir.userName_ == "irmaker");
    REQUIRE(!ir.userVerified_);
    REQUIRE(ir.thumbnail_ == "");  // https only.
    REQUIRE(ir.url_ == "");        // https only.
    REQUIRE(!ir.hasFavoriteState_); // anonymous responses have no is_favorite.
    REQUIRE(ir.tags_.empty());

    // The reply serialises with the names the web client reads.
    std::string json = ToJson(page);
    REQUIRE(json.find("\"totalPages\"") != std::string::npos);
    REQUIRE(json.find("\"hasFavoriteState\"") != std::string::npos);
    REQUIRE(json.find("\"a2ModelsCount\"") != std::string::npos);

    // Trending answers with data only.
    auto trending = ParseTone3000TonesPage(R"({"data":[{"id":7,"title":"T"}]})");
    REQUIRE(trending.ok_);
    REQUIRE(trending.page_ == 1);
    REQUIRE(trending.totalPages_ == 1);
    REQUIRE(trending.total_ == 1);

    // Long descriptions are cut on a character boundary.
    std::string longText;
    for (int i = 0; i < 1500; ++i)
        longText += "\xC3\xA9"; // é, 2 bytes.
    auto cut = ParseTone3000TonesPage(SS(R"({"data":[{"id":1,"description":")" << longText << R"("}]})"));
    REQUIRE(cut.ok_);
    REQUIRE(cut.tones_[0].description_.size() == 2000);

    // Garbage.
    REQUIRE(!ParseTone3000TonesPage("").ok_);
    REQUIRE(!ParseTone3000TonesPage("<html>").ok_);
    REQUIRE(!ParseTone3000TonesPage("[]").ok_);
    REQUIRE(!ParseTone3000TonesPage(R"({"data":{}})").ok_);
    REQUIRE(!ParseTone3000TonesPage(R"({"error":"x"})").error_.empty());

    auto models = ParseTone3000ModelsPage(MODELS_PAGE);
    REQUIRE(models.ok_);
    REQUIRE(models.models_.size() == 2);
    REQUIRE(models.models_[0].id_ == 9001);
    REQUIRE(models.models_[0].name_ == "Gain 5");
    REQUIRE(models.models_[0].size_ == "standard");
    REQUIRE(models.models_[1].size_ == "");
    REQUIRE(models.total_ == 2);
    REQUIRE(models.totalPages_ == 1);
    REQUIRE(ToJson(models).find("model_url") == std::string::npos); // download URLs stay server-side.
    REQUIRE(!ParseTone3000ModelsPage("null").ok_);

    auto names = ParseTone3000Names(R"({"data":[{"id":1,"name":"Fender"},{"name":"Vox"},{"id":3}],"page":1,"total_pages":1})");
    REQUIRE(names.ok_);
    REQUIRE(names.names_ == std::vector<std::string>{"Fender", "Vox"});
    REQUIRE(!ParseTone3000Names("{}").ok_);
}

TEST_CASE("TONE3000 catalog proxy", "[t3k_catalog]")
{
    TempDir dir;
    FakeServer server;

    SECTION("signed out: trending is anonymous, everything else asks for a sign-in")
    {
        auto auth = Tone3000Auth::Create(dir.path / "t.json", server.Deps(), ORIGIN, "c");
        Tone3000Catalog catalog(auth);

        server.responses = {Http(200, TONES_PAGE)};
        auto trending = catalog.Trending("amp");
        REQUIRE(trending.ok_);
        REQUIRE(!trending.signedIn_);
        REQUIRE(trending.tones_.size() == 2);
        REQUIRE(server.requests.size() == 1);
        REQUIRE(server.requests[0].url == "https://t3k.test/api/v1/tones/trending?gear=amp");
        REQUIRE(server.requests[0].headers.empty()); // no credentials.

        server.requests.clear();
        auto search = catalog.Search(Query("x"));
        REQUIRE(!search.ok_);
        REQUIRE(search.needsSignIn_);
        auto fav = catalog.SetFavorite({});
        REQUIRE(!fav.ok_);
        Tone3000CatalogFavoriteRequest fr;
        fr.toneId_ = 5;
        fr.favorite_ = true;
        fav = catalog.SetFavorite(fr);
        REQUIRE(fav.needsSignIn_);
        Tone3000CatalogModelsQuery mq;
        mq.toneId_ = 5;
        REQUIRE(catalog.Models(mq).needsSignIn_);
        REQUIRE(catalog.Taxonomy("tags", {}).needsSignIn_);
        REQUIRE(server.requests.empty());
    }
    SECTION("signed in")
    {
        auto auth = SignedIn(server, dir.path);
        Tone3000Catalog catalog(auth);

        server.responses = {Http(200, TONES_PAGE)};
        auto q = Query("plexi");
        q.gear_ = "amp";
        auto search = catalog.Search(q);
        REQUIRE(search.ok_);
        REQUIRE(search.signedIn_);
        REQUIRE(search.tones_.size() == 2);
        REQUIRE(server.requests.back().method == "GET");
        REQUIRE(server.requests.back().url == "https://t3k.test/api/v1/tones/search?page=1&page_size=25&query=plexi&gears=amp");
        REQUIRE(server.requests.back().headers == std::vector<std::string>{"Authorization: Bearer access-1"});

        // Trending with the session: the signed-in list (favourite state).
        server.responses = {Http(200, TONES_PAGE)};
        auto trending = catalog.Trending("");
        REQUIRE(trending.ok_);
        REQUIRE(trending.signedIn_);
        REQUIRE(server.requests.back().headers.size() == 1);

        // Favourite: PUT, unfavourite: DELETE.
        Tone3000CatalogFavoriteRequest fr;
        fr.toneId_ = 101;
        fr.favorite_ = true;
        server.responses = {Http(204, "")};
        auto fav = catalog.SetFavorite(fr);
        REQUIRE(fav.ok_);
        REQUIRE(fav.toneId_ == 101);
        REQUIRE(server.requests.back().method == "PUT");
        REQUIRE(server.requests.back().url == "https://t3k.test/api/v1/tones/101/favorite");
        REQUIRE(server.requests.back().headers[0] == "Authorization: Bearer access-1");
        fr.favorite_ = false;
        server.responses = {Http(200, "{}")};
        REQUIRE(catalog.SetFavorite(fr).ok_);
        REQUIRE(server.requests.back().method == "DELETE");

        // A failed favourite reports the failure (the UI rolls back).
        server.responses = {Http(500, "")};
        fav = catalog.SetFavorite(fr);
        REQUIRE(!fav.ok_);
        REQUIRE(!fav.needsSignIn_);
        REQUIRE(fav.error_.find("500") != std::string::npos);

        // Models.
        Tone3000CatalogModelsQuery mq;
        mq.toneId_ = 101;
        mq.architecture_ = 2;
        server.responses = {Http(200, MODELS_PAGE)};
        auto models = catalog.Models(mq);
        REQUIRE(models.ok_);
        REQUIRE(models.models_.size() == 2);
        REQUIRE(server.requests.back().url == "https://t3k.test/api/v1/models?tone_id=101&page=1&page_size=50&architecture=2");

        // Tags.
        Tone3000CatalogTaxonomyQuery tq;
        tq.query_ = "cr";
        server.responses = {Http(200, R"({"data":[{"name":"crunch"}]})")};
        auto tags = catalog.Taxonomy("tags", tq);
        REQUIRE(tags.ok_);
        REQUIRE(tags.names_ == std::vector<std::string>{"crunch"});
        REQUIRE(server.requests.back().url == "https://t3k.test/api/v1/tags?page=1&page_size=50&query=cr");

        // Network trouble and server errors.
        server.responses = {Http(0, "")};
        auto failed = catalog.Search(Query());
        REQUIRE(!failed.ok_);
        REQUIRE(!failed.needsSignIn_);
        server.responses = {Http(503, "")};
        failed = catalog.Search(Query());
        REQUIRE(failed.error_.find("503") != std::string::npos);
        server.responses = {Http(200, "<html>")};
        REQUIRE(!catalog.Search(Query()).ok_);

        // Invalid parameters never reach the server.
        server.requests.clear();
        auto badQuery = Query();
        badQuery.gear_ = "evil";
        auto rejected = catalog.Search(badQuery);
        REQUIRE(!rejected.ok_);
        REQUIRE(!rejected.error_.empty());
        REQUIRE(server.requests.empty());
    }
    SECTION("a rejected session asks for a new sign-in; trending falls back to anonymous")
    {
        auto auth = SignedIn(server, dir.path);
        Tone3000Catalog catalog(auth);

        // 401, refresh rejected (signs out).
        server.responses = {Http(401, "")};
        server.posts = {Http(400, R"({"error":"invalid_grant"})")};
        auto search = catalog.Search(Query("x"));
        REQUIRE(!search.ok_);
        REQUIRE(search.needsSignIn_);
        REQUIRE(!auth->IsSignedIn());

        auth = SignedIn(server, dir.path);
        Tone3000Catalog catalog2(auth);
        // 401 twice (refresh accepted, token still refused): trending retries without credentials.
        server.responses = {Http(401, ""), Http(401, ""), Http(200, TONES_PAGE)};
        server.posts = {Http(200, R"({"access_token":"access-2","refresh_token":"refresh-2","expires_in":3600})")};
        auto trending = catalog2.Trending("");
        REQUIRE(trending.ok_);
        REQUIRE(!trending.signedIn_);
        REQUIRE(server.requests.back().headers.empty());
    }
}

TEST_CASE("TONE3000 catalog request slots are bounded", "[t3k_catalog]")
{
    REQUIRE(Tone3000CatalogRequestSlot::InFlight() == 0);
    {
        std::vector<std::unique_ptr<Tone3000CatalogRequestSlot>> slots;
        for (int i = 0; i < t3k_catalog::MAX_REQUESTS_IN_FLIGHT; ++i)
        {
            slots.push_back(std::make_unique<Tone3000CatalogRequestSlot>());
            REQUIRE(slots.back()->Acquired());
        }
        Tone3000CatalogRequestSlot extra;
        REQUIRE(!extra.Acquired());
        REQUIRE(Tone3000CatalogRequestSlot::InFlight() == t3k_catalog::MAX_REQUESTS_IN_FLIGHT);
    }
    REQUIRE(Tone3000CatalogRequestSlot::InFlight() == 0);
    Tone3000CatalogRequestSlot again;
    REQUIRE(again.Acquired());
}

TEST_CASE("TONE3000 authorized requests: methods and origin", "[t3k_catalog]")
{
    TempDir dir;
    FakeServer server;
    auto auth = SignedIn(server, dir.path);

    REQUIRE_THROWS_AS(auth->AuthorizedRequest("POST", "/api/v1/x"), std::invalid_argument);
    REQUIRE_THROWS_AS(auth->AuthorizedRequest("PUT\r\nX: y", "/api/v1/x"), std::invalid_argument);
    REQUIRE_THROWS(auth->AuthorizedRequest("PUT", "https://evil.example/api/v1/x"));
    REQUIRE_THROWS(auth->AnonymousGet("https://evil.example/api/v1/x"));
    REQUIRE(server.requests.empty());

    // PUT retried once after a 401, with a refreshed token; an empty body is declared.
    server.responses = {Http(401, ""), Http(200, "")};
    server.posts = {Http(200, R"({"access_token":"access-2","refresh_token":"refresh-2","expires_in":3600})")};
    auto r = auth->AuthorizedRequest("PUT", "/api/v1/tones/1/favorite");
    REQUIRE(r.status == 200);
    REQUIRE(server.requests.size() == 3);
    REQUIRE(server.requests[2].method == "PUT");
    REQUIRE(server.requests[2].headers == std::vector<std::string>{"Authorization: Bearer access-2", "Content-Length: 0"});

    // Without a request function only GET works.
    TempDir dir2;
    FakeServer server2;
    auto deps = server2.Deps();
    deps.request = nullptr;
    server2.posts = {
        Http(200, R"({"device_code":"d","user_code":"U","verification_uri":"https://t3k.test/device","interval":1})"),
        Http(200, R"({"access_token":"a","refresh_token":"r","expires_in":3600})")};
    auto auth2 = Tone3000Auth::Create(dir2.path / "t.json", deps, ORIGIN, "c");
    auth2->RunDeviceFlow();
    REQUIRE_THROWS(auth2->AuthorizedRequest("DELETE", "/api/v1/tones/1/favorite"));
}
