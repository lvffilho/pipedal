// Copyright (c) 2026 Robin Davies
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#pragma once

// TONE3000 catalog browsing, proxied through the daemon so the access token never
// leaves it (websocket messages t3kCatalog*).
//
// Every client-supplied parameter is checked against a whitelist or a bound before it
// becomes part of a URL (Build* throw std::invalid_argument otherwise), and responses
// are reduced to the fields the web UI shows (Parse* never throw on bad upstream JSON).
//
// Endpoints (origin + /api/v1):
//   GET /tones/search                 page, page_size, query, sort, gears, format, tags, makes,
//                                     creators, calibrated, verified, architecture
//   GET /tones/{favorited|downloaded} page, page_size, query, gear
//   GET /tones/trending[?gear=]       anonymous allowed
//   PUT/DELETE /tones/{id}/favorite
//   GET /models?tone_id&page&page_size[&architecture]
//   GET /tags, GET /makes             page, page_size, query
//
// The network calls block: run them off the websocket thread.

#include "json.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pipedal
{
    class Tone3000Auth;

    namespace t3k_catalog
    {
        static constexpr int64_t MAX_PAGE = 1000;
        static constexpr int64_t MAX_PAGE_SIZE = 100;
        static constexpr size_t MAX_QUERY_LENGTH = 200;    // bytes, after trimming.
        static constexpr size_t MAX_FILTER_VALUES = 10;     // tags, makes or creators.
        static constexpr size_t MAX_FILTER_VALUE_LENGTH = 100;
        static constexpr int64_t MAX_TONE_ID = 9007199254740991LL; // 2^53-1: exact in JS.
        static constexpr int MAX_REQUESTS_IN_FLIGHT = 8;

        // RFC 3986 percent-encoding: everything but unreserved characters (A-Z a-z 0-9 - . _ ~).
        std::string PercentEncode(const std::string &value);
    }

    // t3kCatalogSearch request.
    class Tone3000CatalogQuery
    {
    public:
        std::string kind_ = "search"; // search | favorited | downloaded
        std::string query_;
        std::string sort_;   // "" (API default) | bestMatch | trending | popular | newest | oldest
        std::string gear_;   // "" | amp | amp-cab | pedal | outboard | cab | space | experimental
                             // (deprecated, still accepted: full-rig = amp-cab, ir = format ir)
        std::string format_; // "" | nam | ir
        std::vector<std::string> tags_;
        std::vector<std::string> makes_;
        std::vector<std::string> creators_;
        bool calibrated_ = false;
        bool verified_ = false;
        int64_t architecture_ = 0; // 0 = omit; 1 or 2.
        int64_t page_ = 1;
        int64_t pageSize_ = 25;

        DECLARE_JSON_MAP(Tone3000CatalogQuery);
    };

    // t3kCatalogModels request.
    class Tone3000CatalogModelsQuery
    {
    public:
        int64_t toneId_ = 0;
        int64_t architecture_ = 0; // 0 = omit; 1 or 2.
        int64_t page_ = 1;
        int64_t pageSize_ = 50;
        DECLARE_JSON_MAP(Tone3000CatalogModelsQuery);
    };

    // t3kCatalogTags / t3kCatalogMakes request.
    class Tone3000CatalogTaxonomyQuery
    {
    public:
        std::string query_;
        int64_t pageSize_ = 50;
        DECLARE_JSON_MAP(Tone3000CatalogTaxonomyQuery);
    };

    // t3kCatalogSetFavorite request.
    class Tone3000CatalogFavoriteRequest
    {
    public:
        int64_t toneId_ = 0;
        bool favorite_ = false;
        DECLARE_JSON_MAP(Tone3000CatalogFavoriteRequest);
    };

    // A tone as the browser shows it.
    class Tone3000CatalogTone
    {
    public:
        int64_t id_ = 0;
        std::string title_;
        std::string description_;
        std::string gear_;
        std::string format_; // format, else the deprecated platform.
        std::string thumbnail_; // first image (https only), or "".
        std::string userName_;  // display_name, else username.
        bool userVerified_ = false;
        int64_t modelsCount_ = 0;
        int64_t a2ModelsCount_ = 0;
        int64_t favoritesCount_ = 0;
        int64_t downloadsCount_ = 0;
        bool hasFavoriteState_ = false; // is_favorite was present (signed-in responses).
        bool isFavorite_ = false;
        std::vector<std::string> makes_;
        std::vector<std::string> tags_;
        std::vector<std::string> sizes_;
        std::string license_;
        std::string url_; // https only, or "".
        DECLARE_JSON_MAP(Tone3000CatalogTone);
    };

    class Tone3000CatalogModel
    {
    public:
        int64_t id_ = 0;
        std::string name_;
        std::string size_;
        DECLARE_JSON_MAP(Tone3000CatalogModel);
    };

    // Common reply fields: ok, or an error. needsSignIn: the request needs a TONE3000 session.
    class Tone3000CatalogTonesReply
    {
    public:
        bool ok_ = false;
        bool needsSignIn_ = false;
        bool signedIn_ = false;
        std::string error_;
        int64_t page_ = 1;
        int64_t pageSize_ = 0;
        int64_t total_ = 0;
        int64_t totalPages_ = 1;
        std::vector<Tone3000CatalogTone> tones_;
        DECLARE_JSON_MAP(Tone3000CatalogTonesReply);
    };

    class Tone3000CatalogModelsReply
    {
    public:
        bool ok_ = false;
        bool needsSignIn_ = false;
        std::string error_;
        int64_t page_ = 1;
        int64_t total_ = 0;
        int64_t totalPages_ = 1;
        std::vector<Tone3000CatalogModel> models_;
        DECLARE_JSON_MAP(Tone3000CatalogModelsReply);
    };

    class Tone3000CatalogNamesReply
    {
    public:
        bool ok_ = false;
        bool needsSignIn_ = false;
        std::string error_;
        std::vector<std::string> names_;
        DECLARE_JSON_MAP(Tone3000CatalogNamesReply);
    };

    class Tone3000CatalogFavoriteReply
    {
    public:
        bool ok_ = false;
        bool needsSignIn_ = false;
        std::string error_;
        int64_t toneId_ = 0;
        bool favorite_ = false;
        DECLARE_JSON_MAP(Tone3000CatalogFavoriteReply);
    };

    // URL builders: API paths ("/api/v1/...") with validated, percent-encoded query strings.
    // Throw std::invalid_argument for anything outside the whitelist / bounds.
    std::string BuildTone3000TonesPath(const Tone3000CatalogQuery &query);
    std::string BuildTone3000TrendingPath(const std::string &gear);
    std::string BuildTone3000FavoritePath(int64_t toneId);
    std::string BuildTone3000ModelsPath(const Tone3000CatalogModelsQuery &query);
    std::string BuildTone3000TaxonomyPath(const std::string &kind /* tags | makes */, const Tone3000CatalogTaxonomyQuery &query);
    // The API sort id for a UI sort name ("popular" -> "downloads-all-time"); throws if unknown.
    std::string Tone3000SortId(const std::string &sort);

    // Response parsers. Malformed input yields ok_ = false with an error.
    Tone3000CatalogTonesReply ParseTone3000TonesPage(const std::string &body);
    Tone3000CatalogModelsReply ParseTone3000ModelsPage(const std::string &body);
    Tone3000CatalogNamesReply ParseTone3000Names(const std::string &body);

    // The proxy. Each call blocks on the network (bounded by curl --max-time in Tone3000Auth).
    class Tone3000Catalog
    {
    public:
        Tone3000Catalog(std::shared_ptr<Tone3000Auth> auth);

        Tone3000CatalogTonesReply Search(const Tone3000CatalogQuery &query);
        // Signed in: with is_favorite; signed out (or session gone): anonymous.
        Tone3000CatalogTonesReply Trending(const std::string &gear);
        Tone3000CatalogFavoriteReply SetFavorite(const Tone3000CatalogFavoriteRequest &request);
        Tone3000CatalogModelsReply Models(const Tone3000CatalogModelsQuery &query);
        Tone3000CatalogNamesReply Taxonomy(const std::string &kind, const Tone3000CatalogTaxonomyQuery &query);

    private:
        std::shared_ptr<Tone3000Auth> auth;
    };

    // Bounds the number of proxy requests running at once (one detached thread each).
    class Tone3000CatalogRequestSlot
    {
    public:
        Tone3000CatalogRequestSlot();
        ~Tone3000CatalogRequestSlot();
        Tone3000CatalogRequestSlot(const Tone3000CatalogRequestSlot &) = delete;
        Tone3000CatalogRequestSlot &operator=(const Tone3000CatalogRequestSlot &) = delete;
        bool Acquired() const { return acquired; }

        static int InFlight();

    private:
        bool acquired = false;
    };
}
