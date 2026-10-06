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

#include "Tone3000Catalog.hpp"
#include "Tone3000Auth.hpp"
#include "json_variant.hpp"
#include "ss.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

using namespace pipedal;

JSON_MAP_BEGIN(Tone3000CatalogQuery)
JSON_MAP_REFERENCE(Tone3000CatalogQuery, kind)
JSON_MAP_REFERENCE(Tone3000CatalogQuery, query)
JSON_MAP_REFERENCE(Tone3000CatalogQuery, sort)
JSON_MAP_REFERENCE(Tone3000CatalogQuery, gear)
JSON_MAP_REFERENCE(Tone3000CatalogQuery, format)
JSON_MAP_REFERENCE(Tone3000CatalogQuery, tags)
JSON_MAP_REFERENCE(Tone3000CatalogQuery, makes)
JSON_MAP_REFERENCE(Tone3000CatalogQuery, creators)
JSON_MAP_REFERENCE(Tone3000CatalogQuery, calibrated)
JSON_MAP_REFERENCE(Tone3000CatalogQuery, verified)
JSON_MAP_REFERENCE(Tone3000CatalogQuery, architecture)
JSON_MAP_REFERENCE(Tone3000CatalogQuery, page)
JSON_MAP_REFERENCE(Tone3000CatalogQuery, pageSize)
JSON_MAP_END()

JSON_MAP_BEGIN(Tone3000CatalogModelsQuery)
JSON_MAP_REFERENCE(Tone3000CatalogModelsQuery, toneId)
JSON_MAP_REFERENCE(Tone3000CatalogModelsQuery, architecture)
JSON_MAP_REFERENCE(Tone3000CatalogModelsQuery, page)
JSON_MAP_REFERENCE(Tone3000CatalogModelsQuery, pageSize)
JSON_MAP_END()

JSON_MAP_BEGIN(Tone3000CatalogTaxonomyQuery)
JSON_MAP_REFERENCE(Tone3000CatalogTaxonomyQuery, query)
JSON_MAP_REFERENCE(Tone3000CatalogTaxonomyQuery, pageSize)
JSON_MAP_END()

JSON_MAP_BEGIN(Tone3000CatalogFavoriteRequest)
JSON_MAP_REFERENCE(Tone3000CatalogFavoriteRequest, toneId)
JSON_MAP_REFERENCE(Tone3000CatalogFavoriteRequest, favorite)
JSON_MAP_END()

JSON_MAP_BEGIN(Tone3000CatalogTone)
JSON_MAP_REFERENCE(Tone3000CatalogTone, id)
JSON_MAP_REFERENCE(Tone3000CatalogTone, title)
JSON_MAP_REFERENCE(Tone3000CatalogTone, description)
JSON_MAP_REFERENCE(Tone3000CatalogTone, gear)
JSON_MAP_REFERENCE(Tone3000CatalogTone, format)
JSON_MAP_REFERENCE(Tone3000CatalogTone, thumbnail)
JSON_MAP_REFERENCE(Tone3000CatalogTone, userName)
JSON_MAP_REFERENCE(Tone3000CatalogTone, userVerified)
JSON_MAP_REFERENCE(Tone3000CatalogTone, modelsCount)
JSON_MAP_REFERENCE(Tone3000CatalogTone, a2ModelsCount)
JSON_MAP_REFERENCE(Tone3000CatalogTone, favoritesCount)
JSON_MAP_REFERENCE(Tone3000CatalogTone, downloadsCount)
JSON_MAP_REFERENCE(Tone3000CatalogTone, hasFavoriteState)
JSON_MAP_REFERENCE(Tone3000CatalogTone, isFavorite)
JSON_MAP_REFERENCE(Tone3000CatalogTone, makes)
JSON_MAP_REFERENCE(Tone3000CatalogTone, tags)
JSON_MAP_REFERENCE(Tone3000CatalogTone, sizes)
JSON_MAP_REFERENCE(Tone3000CatalogTone, license)
JSON_MAP_REFERENCE(Tone3000CatalogTone, url)
JSON_MAP_END()

JSON_MAP_BEGIN(Tone3000CatalogModel)
JSON_MAP_REFERENCE(Tone3000CatalogModel, id)
JSON_MAP_REFERENCE(Tone3000CatalogModel, name)
JSON_MAP_REFERENCE(Tone3000CatalogModel, size)
JSON_MAP_END()

JSON_MAP_BEGIN(Tone3000CatalogTonesReply)
JSON_MAP_REFERENCE(Tone3000CatalogTonesReply, ok)
JSON_MAP_REFERENCE(Tone3000CatalogTonesReply, needsSignIn)
JSON_MAP_REFERENCE(Tone3000CatalogTonesReply, signedIn)
JSON_MAP_REFERENCE(Tone3000CatalogTonesReply, error)
JSON_MAP_REFERENCE(Tone3000CatalogTonesReply, page)
JSON_MAP_REFERENCE(Tone3000CatalogTonesReply, pageSize)
JSON_MAP_REFERENCE(Tone3000CatalogTonesReply, total)
JSON_MAP_REFERENCE(Tone3000CatalogTonesReply, totalPages)
JSON_MAP_REFERENCE(Tone3000CatalogTonesReply, tones)
JSON_MAP_END()

JSON_MAP_BEGIN(Tone3000CatalogModelsReply)
JSON_MAP_REFERENCE(Tone3000CatalogModelsReply, ok)
JSON_MAP_REFERENCE(Tone3000CatalogModelsReply, needsSignIn)
JSON_MAP_REFERENCE(Tone3000CatalogModelsReply, error)
JSON_MAP_REFERENCE(Tone3000CatalogModelsReply, page)
JSON_MAP_REFERENCE(Tone3000CatalogModelsReply, total)
JSON_MAP_REFERENCE(Tone3000CatalogModelsReply, totalPages)
JSON_MAP_REFERENCE(Tone3000CatalogModelsReply, models)
JSON_MAP_END()

JSON_MAP_BEGIN(Tone3000CatalogNamesReply)
JSON_MAP_REFERENCE(Tone3000CatalogNamesReply, ok)
JSON_MAP_REFERENCE(Tone3000CatalogNamesReply, needsSignIn)
JSON_MAP_REFERENCE(Tone3000CatalogNamesReply, error)
JSON_MAP_REFERENCE(Tone3000CatalogNamesReply, names)
JSON_MAP_END()

JSON_MAP_BEGIN(Tone3000CatalogFavoriteReply)
JSON_MAP_REFERENCE(Tone3000CatalogFavoriteReply, ok)
JSON_MAP_REFERENCE(Tone3000CatalogFavoriteReply, needsSignIn)
JSON_MAP_REFERENCE(Tone3000CatalogFavoriteReply, error)
JSON_MAP_REFERENCE(Tone3000CatalogFavoriteReply, toneId)
JSON_MAP_REFERENCE(Tone3000CatalogFavoriteReply, favorite)
JSON_MAP_END()

namespace
{
    using namespace pipedal::t3k_catalog;

    static constexpr size_t MAX_DESCRIPTION_LENGTH = 2000;
    static constexpr size_t MAX_TEXT_LENGTH = 500;
    static constexpr size_t MAX_LIST_ENTRIES = 50;

    const char *NOT_SIGNED_IN = "Sign in to TONE3000 to use this.";

    // What the gear and format chips may select: TONE3000's gear values (checked live, October
    // 2026: the API answers 400 "gear must be one of: amp, amp-cab, pedal, outboard, cab, space,
    // experimental"), plus the deprecated aliases older clients send: "full-rig" (now "amp-cab")
    // and "ir" (impulse responses are a format, not a gear: sent as format=ir to the search).
    const char *const GEARS[] = {"amp", "amp-cab", "pedal", "outboard", "cab", "space", "experimental",
                                 "full-rig", "ir"};
    const char *const FORMATS[] = {"nam", "ir"};

    template <size_t N>
    bool OneOf(const std::string &value, const char *const (&allowed)[N])
    {
        for (const char *a : allowed)
        {
            if (value == a)
                return true;
        }
        return false;
    }

    std::string Trim(const std::string &s)
    {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    bool HasControlCharacters(const std::string &s)
    {
        for (char c : s)
        {
            unsigned char uc = (unsigned char)c;
            if (uc < 0x20 || uc == 0x7F)
                return true;
        }
        return false;
    }

    std::string CheckedText(const std::string &value, size_t maxLength, const char *what)
    {
        std::string trimmed = Trim(value);
        if (trimmed.length() > maxLength)
        {
            throw std::invalid_argument(SS("TONE3000 " << what << " is too long."));
        }
        if (HasControlCharacters(trimmed))
        {
            throw std::invalid_argument(SS("TONE3000 " << what << " contains invalid characters."));
        }
        return trimmed;
    }

    // The gear value to send: checked, with the "full-rig" alias replaced.
    std::string ApiGear(const std::string &gear)
    {
        if (!gear.empty() && !OneOf(gear, GEARS))
            throw std::invalid_argument(SS("Invalid TONE3000 gear: " << gear));
        return gear == "full-rig" ? "amp-cab" : gear;
    }

    void CheckRange(int64_t value, int64_t min, int64_t max, const char *what)
    {
        if (value < min || value > max)
        {
            throw std::invalid_argument(SS("Invalid TONE3000 " << what << ": " << value));
        }
    }

    void CheckArchitecture(int64_t architecture)
    {
        if (architecture != 0 && architecture != 1 && architecture != 2)
        {
            throw std::invalid_argument(SS("Invalid TONE3000 architecture: " << architecture));
        }
    }

    // Each value encoded on its own, so that the separator stays a literal list separator. Values
    // may not contain the separator themselves (the API would split them).
    std::string JoinValues(const std::vector<std::string> &values, char separator, const char *what)
    {
        if (values.size() > MAX_FILTER_VALUES)
        {
            throw std::invalid_argument(SS("Too many TONE3000 " << what << "."));
        }
        std::string result;
        for (const std::string &raw : values)
        {
            std::string value = CheckedText(raw, MAX_FILTER_VALUE_LENGTH, what);
            if (value.empty() || value.find(separator) != std::string::npos)
            {
                throw std::invalid_argument(SS("Invalid TONE3000 " << what << ": " << value));
            }
            if (!result.empty())
                result += separator;
            result += PercentEncode(value);
        }
        return result;
    }

    class QueryString
    {
    public:
        // value must already be encoded.
        QueryString &Add(const char *key, const std::string &value)
        {
            if (!value.empty())
            {
                text += text.empty() ? '?' : '&';
                text += key;
                text += '=';
                text += value;
            }
            return *this;
        }
        QueryString &AddEncoded(const char *key, const std::string &value)
        {
            return Add(key, PercentEncode(value));
        }
        QueryString &Add(const char *key, int64_t value)
        {
            return Add(key, std::to_string(value));
        }
        QueryString &AddFlag(const char *key, bool value)
        {
            return Add(key, value ? std::string("true") : std::string());
        }
        const std::string &str() const { return text; }

    private:
        std::string text;
    };

    // Truncate at a UTF-8 character boundary.
    std::string Truncate(std::string s, size_t maxLength)
    {
        if (s.length() <= maxLength)
            return s;
        size_t n = maxLength;
        while (n > 0 && (((unsigned char)s[n]) & 0xC0) == 0x80)
            --n;
        s.resize(n);
        return s;
    }

    const json_variant *Member(const json_variant &v, const char *key)
    {
        if (!v.is_object() || !v.contains(key))
            return nullptr;
        return &v[key];
    }

    std::string String(const json_variant &v, const char *key, size_t maxLength = MAX_TEXT_LENGTH)
    {
        const json_variant *m = Member(v, key);
        if (!m || !m->is_string())
            return "";
        return Truncate(m->as_string(), maxLength);
    }

    int64_t Integer(const json_variant &v, const char *key, int64_t defaultValue = 0)
    {
        const json_variant *m = Member(v, key);
        if (!m || !m->is_number())
            return defaultValue;
        double d = m->as_number();
        if (!std::isfinite(d) || d < -9007199254740991.0 || d > 9007199254740991.0)
            return defaultValue;
        return (int64_t)d;
    }

    std::optional<bool> OptBool(const json_variant &v, const char *key)
    {
        const json_variant *m = Member(v, key);
        if (!m || !m->is_bool())
            return std::nullopt;
        return m->as_bool();
    }

    std::string HttpsOnly(const std::string &url)
    {
        return url.starts_with("https://") ? url : "";
    }

    // Arrays of strings, or of objects with a "name" (makes, tags).
    std::vector<std::string> Names(const json_variant &v, const char *key)
    {
        std::vector<std::string> result;
        const json_variant *m = Member(v, key);
        if (!m || !m->is_array())
            return result;
        for (const json_variant &item : *(m->as_array()))
        {
            std::string name;
            if (item.is_string())
                name = item.as_string();
            else if (item.is_object())
                name = String(item, "name");
            name = Trim(Truncate(name, MAX_TEXT_LENGTH));
            if (!name.empty())
                result.push_back(name);
            if (result.size() >= MAX_LIST_ENTRIES)
                break;
        }
        return result;
    }

    std::optional<Tone3000CatalogTone> ParseTone(const json_variant &v)
    {
        if (!v.is_object())
            return std::nullopt;
        Tone3000CatalogTone t;
        t.id_ = Integer(v, "id");
        if (t.id_ <= 0)
            return std::nullopt;
        t.title_ = String(v, "title");
        t.description_ = String(v, "description", MAX_DESCRIPTION_LENGTH);
        t.gear_ = String(v, "gear");
        t.format_ = String(v, "format");
        if (t.format_.empty())
            t.format_ = String(v, "platform");
        if (const json_variant *images = Member(v, "images"); images && images->is_array() && images->size() > 0)
        {
            const json_variant &first = (*images)[size_t(0)];
            if (first.is_string())
                t.thumbnail_ = HttpsOnly(Truncate(first.as_string(), 2048));
        }
        if (const json_variant *user = Member(v, "user"); user && user->is_object())
        {
            t.userName_ = Trim(String(*user, "display_name"));
            if (t.userName_.empty())
                t.userName_ = String(*user, "username");
            t.userVerified_ = OptBool(*user, "is_verified").value_or(false);
        }
        t.modelsCount_ = Integer(v, "models_count");
        t.a2ModelsCount_ = Integer(v, "a2_models_count");
        t.favoritesCount_ = Integer(v, "favorites_count");
        t.downloadsCount_ = Integer(v, "downloads_count");
        if (auto fav = OptBool(v, "is_favorite"))
        {
            t.hasFavoriteState_ = true;
            t.isFavorite_ = *fav;
        }
        t.makes_ = Names(v, "makes");
        t.tags_ = Names(v, "tags");
        t.sizes_ = Names(v, "sizes");
        t.license_ = String(v, "license");
        t.url_ = HttpsOnly(String(v, "url", 2048));
        return t;
    }

    std::optional<json_variant> ParseJson(const std::string &body)
    {
        try
        {
            return json_variant::parse(body);
        }
        catch (const std::exception &)
        {
            return std::nullopt;
        }
    }

    const char *INVALID_RESPONSE = "Unexpected response from the TONE3000 server.";

    // Result of one proxied request.
    struct Fetched
    {
        bool ok = false;
        bool needsSignIn = false;
        std::string error;
        std::string body;
    };

    Fetched Classify(const Tone3000HttpResponse &response)
    {
        Fetched f;
        if (response.status == 0)
        {
            f.error = "Unable to reach the TONE3000 server.";
        }
        else if (response.status == 401)
        {
            f.needsSignIn = true;
            f.error = "TONE3000 did not accept the sign-in. Please sign in again.";
        }
        else if (response.status < 200 || response.status >= 300)
        {
            f.error = SS("TONE3000 request failed (HTTP " << response.status << ").");
        }
        else
        {
            f.ok = true;
            f.body = response.body;
        }
        return f;
    }

    Fetched AuthorizedFetch(Tone3000Auth &auth, const std::string &method, const std::string &path)
    {
        if (!auth.IsSignedIn())
        {
            Fetched f;
            f.needsSignIn = true;
            f.error = NOT_SIGNED_IN;
            return f;
        }
        try
        {
            return Classify(auth.AuthorizedRequest(method, path));
        }
        catch (const std::exception &e)
        {
            Fetched f;
            f.error = e.what();
            f.needsSignIn = !auth.IsSignedIn(); // the refresh token was rejected.
            return f;
        }
    }

    template <typename REPLY>
    void CopyFailure(REPLY &reply, const Fetched &f)
    {
        reply.ok_ = false;
        reply.needsSignIn_ = f.needsSignIn;
        reply.error_ = f.error;
    }
}

std::string pipedal::t3k_catalog::PercentEncode(const std::string &value)
{
    static const char HEX[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() * 3);
    for (char c : value)
    {
        unsigned char uc = (unsigned char)c;
        if ((uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z') || (uc >= '0' && uc <= '9') ||
            uc == '-' || uc == '.' || uc == '_' || uc == '~')
        {
            result += c;
        }
        else
        {
            result += '%';
            result += HEX[uc >> 4];
            result += HEX[uc & 0x0F];
        }
    }
    return result;
}

std::string pipedal::Tone3000SortId(const std::string &sort)
{
    if (sort == "bestMatch")
        return "best-match";
    if (sort == "trending")
        return "trending";
    if (sort == "popular")
        return "downloads-all-time";
    if (sort == "newest")
        return "newest";
    if (sort == "oldest")
        return "oldest";
    throw std::invalid_argument(SS("Invalid TONE3000 sort: " << sort));
}

std::string pipedal::BuildTone3000TonesPath(const Tone3000CatalogQuery &q)
{
    CheckRange(q.page_, 1, MAX_PAGE, "page");
    CheckRange(q.pageSize_, 1, MAX_PAGE_SIZE, "page size");
    std::string text = CheckedText(q.query_, MAX_QUERY_LENGTH, "search text");
    std::string gear = ApiGear(q.gear_);
    if (!q.format_.empty() && !OneOf(q.format_, FORMATS))
        throw std::invalid_argument(SS("Invalid TONE3000 format: " << q.format_));
    CheckArchitecture(q.architecture_);
    std::string sortId = q.sort_.empty() ? "" : Tone3000SortId(q.sort_);
    std::string tags = JoinValues(q.tags_, '_', "tags");
    std::string makes = JoinValues(q.makes_, '_', "makes");
    std::string creators = JoinValues(q.creators_, ',', "creators");

    QueryString qs;
    qs.Add("page", q.page_).Add("page_size", q.pageSize_).AddEncoded("query", text);

    if (q.kind_ == "favorited" || q.kind_ == "downloaded")
    {
        // The user's own lists filter by title and gear only (they accept the "ir" alias).
        qs.AddEncoded("gear", gear);
        return "/api/v1/tones/" + q.kind_ + qs.str();
    }
    if (q.kind_ != "search")
        throw std::invalid_argument(SS("Invalid TONE3000 list: " << q.kind_));

    // The API sorts by best match when there is text, else by trending. Send a sort only when it
    // differs from that default (best match without text means the default too).
    std::string defaultSortId = text.empty() ? "trending" : "best-match";
    if (!sortId.empty() && sortId != defaultSortId && !(sortId == "best-match" && text.empty()))
        qs.Add("sort", sortId);
    std::string format = q.format_;
    if (gear == "ir")
    {
        // Deprecated as a gear (the API may strip it from gears=): ask for the format instead.
        if (format == "nam")
            throw std::invalid_argument("TONE3000 gear \"ir\" contradicts format \"nam\".");
        format = "ir";
        gear.clear();
    }
    qs.AddEncoded("gears", gear);
    qs.Add("format", format);
    qs.Add("tags", tags);
    qs.Add("makes", makes);
    qs.Add("creators", creators);
    // Calibration only applies to captures, not impulse responses (cab and space are IR-only gear).
    bool calibratedApplies = format != "ir" && gear != "cab" && gear != "space";
    qs.AddFlag("calibrated", q.calibrated_ && calibratedApplies);
    qs.AddFlag("verified", q.verified_);
    if (q.architecture_ != 0)
        qs.Add("architecture", q.architecture_);
    return "/api/v1/tones/search" + qs.str();
}

std::string pipedal::BuildTone3000TrendingPath(const std::string &gear)
{
    // The trending feed accepts gear=ir (impulse responses of any gear).
    return "/api/v1/tones/trending" + QueryString().AddEncoded("gear", ApiGear(gear)).str();
}

std::string pipedal::BuildTone3000FavoritePath(int64_t toneId)
{
    CheckRange(toneId, 1, MAX_TONE_ID, "tone id");
    return SS("/api/v1/tones/" << toneId << "/favorite");
}

std::string pipedal::BuildTone3000ModelsPath(const Tone3000CatalogModelsQuery &q)
{
    CheckRange(q.toneId_, 1, MAX_TONE_ID, "tone id");
    CheckRange(q.page_, 1, MAX_PAGE, "page");
    CheckRange(q.pageSize_, 1, MAX_PAGE_SIZE, "page size");
    CheckArchitecture(q.architecture_);
    QueryString qs;
    qs.Add("tone_id", q.toneId_).Add("page", q.page_).Add("page_size", q.pageSize_);
    if (q.architecture_ != 0)
        qs.Add("architecture", q.architecture_);
    return "/api/v1/models" + qs.str();
}

std::string pipedal::BuildTone3000TaxonomyPath(const std::string &kind, const Tone3000CatalogTaxonomyQuery &q)
{
    if (kind != "tags" && kind != "makes")
        throw std::invalid_argument(SS("Invalid TONE3000 taxonomy: " << kind));
    CheckRange(q.pageSize_, 1, MAX_PAGE_SIZE, "page size");
    std::string text = CheckedText(q.query_, MAX_FILTER_VALUE_LENGTH, "search text");
    QueryString qs;
    qs.Add("page", (int64_t)1).Add("page_size", q.pageSize_).AddEncoded("query", text);
    return "/api/v1/" + kind + qs.str();
}

Tone3000CatalogTonesReply pipedal::ParseTone3000TonesPage(const std::string &body)
{
    Tone3000CatalogTonesReply reply;
    auto v = ParseJson(body);
    const json_variant *data = v ? Member(*v, "data") : nullptr;
    if (!data || !data->is_array())
    {
        reply.error_ = INVALID_RESPONSE;
        return reply;
    }
    for (const json_variant &row : *(data->as_array()))
    {
        if (auto tone = ParseTone(row))
            reply.tones_.push_back(std::move(*tone));
    }
    reply.page_ = std::max((int64_t)1, Integer(*v, "page", 1));
    reply.pageSize_ = Integer(*v, "page_size", (int64_t)reply.tones_.size());
    reply.total_ = Integer(*v, "total", (int64_t)reply.tones_.size());
    reply.totalPages_ = std::max((int64_t)1, Integer(*v, "total_pages", 1));
    reply.ok_ = true;
    return reply;
}

Tone3000CatalogModelsReply pipedal::ParseTone3000ModelsPage(const std::string &body)
{
    Tone3000CatalogModelsReply reply;
    auto v = ParseJson(body);
    const json_variant *data = v ? Member(*v, "data") : nullptr;
    if (!data || !data->is_array())
    {
        reply.error_ = INVALID_RESPONSE;
        return reply;
    }
    for (const json_variant &row : *(data->as_array()))
    {
        if (!row.is_object())
            continue;
        Tone3000CatalogModel m;
        m.id_ = Integer(row, "id");
        if (m.id_ <= 0)
            continue;
        m.name_ = String(row, "name");
        m.size_ = String(row, "size");
        reply.models_.push_back(std::move(m));
    }
    reply.page_ = std::max((int64_t)1, Integer(*v, "page", 1));
    reply.total_ = Integer(*v, "total", (int64_t)reply.models_.size());
    reply.totalPages_ = std::max((int64_t)1, Integer(*v, "total_pages", 1));
    reply.ok_ = true;
    return reply;
}

Tone3000CatalogNamesReply pipedal::ParseTone3000Names(const std::string &body)
{
    Tone3000CatalogNamesReply reply;
    auto v = ParseJson(body);
    if (!v || !v->is_object() || !v->contains("data") || !(*v)["data"].is_array())
    {
        reply.error_ = INVALID_RESPONSE;
        return reply;
    }
    reply.names_ = Names(*v, "data");
    reply.ok_ = true;
    return reply;
}

//////////////////////////////////////////////////////////////////////////////
// Tone3000Catalog

Tone3000Catalog::Tone3000Catalog(std::shared_ptr<Tone3000Auth> auth)
    : auth(std::move(auth))
{
}

Tone3000CatalogTonesReply Tone3000Catalog::Search(const Tone3000CatalogQuery &query)
{
    Tone3000CatalogTonesReply reply;
    std::string path;
    try
    {
        path = BuildTone3000TonesPath(query);
    }
    catch (const std::exception &e)
    {
        reply.error_ = e.what();
        return reply;
    }
    Fetched f = AuthorizedFetch(*auth, "GET", path);
    if (!f.ok)
    {
        CopyFailure(reply, f);
        reply.signedIn_ = auth->IsSignedIn();
        return reply;
    }
    reply = ParseTone3000TonesPage(f.body);
    reply.signedIn_ = true;
    return reply;
}

Tone3000CatalogTonesReply Tone3000Catalog::Trending(const std::string &gear)
{
    Tone3000CatalogTonesReply reply;
    std::string path;
    try
    {
        path = BuildTone3000TrendingPath(gear);
    }
    catch (const std::exception &e)
    {
        reply.error_ = e.what();
        return reply;
    }
    bool signedIn = false;
    bool anonymous = true;
    Fetched f;
    if (auth->IsSignedIn())
    {
        f = AuthorizedFetch(*auth, "GET", path);
        signedIn = f.ok;
        anonymous = f.needsSignIn;
    }
    if (anonymous)
    {
        // Signed out, or the session is no good: the public list (no favourite state).
        try
        {
            f = Classify(auth->AnonymousGet(path));
        }
        catch (const std::exception &e)
        {
            f = Fetched();
            f.error = e.what();
        }
    }
    if (!f.ok)
    {
        CopyFailure(reply, f);
        reply.needsSignIn_ = false; // trending never needs a session.
        reply.signedIn_ = auth->IsSignedIn();
        return reply;
    }
    reply = ParseTone3000TonesPage(f.body);
    reply.signedIn_ = signedIn;
    return reply;
}

Tone3000CatalogFavoriteReply Tone3000Catalog::SetFavorite(const Tone3000CatalogFavoriteRequest &request)
{
    Tone3000CatalogFavoriteReply reply;
    reply.toneId_ = request.toneId_;
    reply.favorite_ = request.favorite_;
    std::string path;
    try
    {
        path = BuildTone3000FavoritePath(request.toneId_);
    }
    catch (const std::exception &e)
    {
        reply.error_ = e.what();
        return reply;
    }
    Fetched f = AuthorizedFetch(*auth, request.favorite_ ? "PUT" : "DELETE", path);
    if (!f.ok)
    {
        CopyFailure(reply, f);
        return reply;
    }
    reply.ok_ = true;
    return reply;
}

Tone3000CatalogModelsReply Tone3000Catalog::Models(const Tone3000CatalogModelsQuery &query)
{
    Tone3000CatalogModelsReply reply;
    std::string path;
    try
    {
        path = BuildTone3000ModelsPath(query);
    }
    catch (const std::exception &e)
    {
        reply.error_ = e.what();
        return reply;
    }
    Fetched f = AuthorizedFetch(*auth, "GET", path);
    if (!f.ok)
    {
        CopyFailure(reply, f);
        return reply;
    }
    return ParseTone3000ModelsPage(f.body);
}

Tone3000CatalogNamesReply Tone3000Catalog::Taxonomy(const std::string &kind, const Tone3000CatalogTaxonomyQuery &query)
{
    Tone3000CatalogNamesReply reply;
    std::string path;
    try
    {
        path = BuildTone3000TaxonomyPath(kind, query);
    }
    catch (const std::exception &e)
    {
        reply.error_ = e.what();
        return reply;
    }
    Fetched f = AuthorizedFetch(*auth, "GET", path);
    if (!f.ok)
    {
        CopyFailure(reply, f);
        return reply;
    }
    return ParseTone3000Names(f.body);
}

//////////////////////////////////////////////////////////////////////////////
// Tone3000CatalogRequestSlot

static std::atomic<int> catalogRequestsInFlight{0};

Tone3000CatalogRequestSlot::Tone3000CatalogRequestSlot()
{
    int current = catalogRequestsInFlight.load();
    while (current < MAX_REQUESTS_IN_FLIGHT)
    {
        if (catalogRequestsInFlight.compare_exchange_weak(current, current + 1))
        {
            acquired = true;
            return;
        }
    }
}

Tone3000CatalogRequestSlot::~Tone3000CatalogRequestSlot()
{
    if (acquired)
    {
        --catalogRequestsInFlight;
    }
}

int Tone3000CatalogRequestSlot::InFlight()
{
    return catalogRequestsInFlight.load();
}
