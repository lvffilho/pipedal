#include "pch.h"
#include "catch.hpp"
#include "HtmlHelper.hpp"
#include "Uri.hpp"
#include "WebServer.hpp"
#include "UploadPolicy.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>

using namespace pipedal;
namespace fs = std::filesystem;

static bool Resolve(const fs::path &root, const std::string &uriPath, fs::path *out)
{
    uri u(("http://localhost" + uriPath).c_str());
    std::vector<std::string> segs;
    for (size_t i = 0; i < u.segment_count(); ++i)
        segs.push_back(u.segment(i));
    return HtmlHelper::TryResolveUnderRoot(root, segs, out);
}

TEST_CASE("web path safety", "[web_path_safety]")
{
    fs::path root = fs::temp_directory_path() / "pipedal_path_safety_root";
    fs::remove_all(root);
    fs::create_directories(root / "a" / "b");
    fs::path out;

    CHECK_FALSE(Resolve(root, "/%2Fetc%2Fpasswd", &out));
    CHECK_FALSE(Resolve(root, "/..%2F..%2Fetc", &out));
    CHECK_FALSE(Resolve(root, "/%2E%2E/x", &out));
    CHECK_FALSE(Resolve(root, "/a/..", &out));
    CHECK_FALSE(Resolve(root, "/a/%5Cb", &out));
    CHECK_FALSE(Resolve(root, "/a/b%00c", &out));
    CHECK(Resolve(root, "/a/b/c.txt", &out));
    CHECK(out == root / "a" / "b" / "c.txt");

    CHECK_FALSE(HtmlHelper::IsSafePathSegment(""));
    CHECK_FALSE(HtmlHelper::IsSafePathSegment("."));
    CHECK_FALSE(HtmlHelper::IsSafePathSegment(".."));
    CHECK_FALSE(HtmlHelper::IsSafePathSegment("a/b"));
    CHECK_FALSE(HtmlHelper::IsSafePathSegment(std::string("a\0b", 3)));
    CHECK(HtmlHelper::IsSafePathSegment("file.js"));

    // symlink escaping the root is rejected.
    fs::create_directory_symlink("/etc", root / "link");
    CHECK_FALSE(Resolve(root, "/link/passwd", &out));
    fs::remove_all(root);
}

TEST_CASE("decode_url_segment malformed", "[web_path_safety]")
{
    CHECK_THROWS_AS(HtmlHelper::decode_url_segment("%"), std::invalid_argument);
    CHECK_THROWS_AS(HtmlHelper::decode_url_segment("%4"), std::invalid_argument);
    CHECK_THROWS_AS(HtmlHelper::decode_url_segment("%zz"), std::invalid_argument);
    CHECK(HtmlHelper::decode_url_segment("%41") == "A");
    CHECK(HtmlHelper::decode_url_segment("a%2Fb") == "a/b");
    // "x%4" placed at the very end of a page followed by a PROT_NONE guard page:
    // any read past pEnd faults deterministically.
    long pageSize = sysconf(_SC_PAGESIZE);
    // RAII, so a failed REQUIRE below doesn't leak the mapping.
    struct Mapping
    {
        void *mem;
        size_t size;
        ~Mapping()
        {
            if (mem != MAP_FAILED)
                munmap(mem, size);
        }
    } mapping{mmap(nullptr, 2 * pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), (size_t)(2 * pageSize)};
    REQUIRE(mapping.mem != MAP_FAILED);
    char *mem = (char *)mapping.mem;
    REQUIRE(mprotect(mem + pageSize, pageSize, PROT_NONE) == 0);
    char *start = mem + pageSize - 3;
    memcpy(start, "x%4", 3);
    CHECK_THROWS_AS(HtmlHelper::decode_url_segment(start, start + 3), std::invalid_argument);
}

TEST_CASE("http status for handler exceptions", "[web_path_safety]")
{
    // URL-decoding failures are the client's fault.
    CHECK(HttpStatusForException(std::invalid_argument("bad percent-encoding")) == 400);
    CHECK(HttpStatusForException(std::runtime_error("disk on fire")) == 500);
    CHECK(HttpStatusForException(std::logic_error("bug")) == 500);
    CHECK(HttpStatusForException(std::out_of_range("range")) == 500); // logic_error sibling, not invalid_argument
    // an exception reached through a base reference still maps by its dynamic type.
    try
    {
        HtmlHelper::decode_url_segment("%zz");
        FAIL("expected throw");
    }
    catch (const std::exception &e)
    {
        CHECK(HttpStatusForException(e) == 400);
    }
}

TEST_CASE("IsPathUnderRoot", "[web_path_safety]")
{
    fs::path base = fs::temp_directory_path() / "pipedal_under_root_test";
    fs::remove_all(base);
    fs::create_directories(base / "root");
    fs::create_directories(base / "outside");
    fs::path root = base / "root";

    CHECK(HtmlHelper::IsPathUnderRoot(root, root / "index.html"));
    CHECK(HtmlHelper::IsPathUnderRoot(root, root));
    CHECK_FALSE(HtmlHelper::IsPathUnderRoot(root, base / "outside" / "x"));
    // string-prefix sibling is not inside the root.
    CHECK_FALSE(HtmlHelper::IsPathUnderRoot(root, base / "root2" / "x"));

    // a .gz sibling that is a symlink out of the root is rejected.
    { std::ofstream(base / "outside" / "secret.gz") << "x"; }
    fs::create_symlink(base / "outside" / "secret.gz", root / "app.js.gz");
    CHECK_FALSE(HtmlHelper::IsPathUnderRoot(root, root / "app.js.gz"));

    // file symlink out of the root: the parent-directory check accepts it
    // (PiPedal links resources into the uploads dir); the full check rejects it.
    CHECK(HtmlHelper::IsParentDirectoryUnderRoot(root, root / "app.js.gz"));
    // symlinked directory escaping the root: rejected.
    fs::create_directory_symlink(base / "outside", root / "dirlink");
    CHECK_FALSE(HtmlHelper::IsParentDirectoryUnderRoot(root, root / "dirlink" / "secret.gz"));
    CHECK_FALSE(HtmlHelper::IsParentDirectoryUnderRoot(root, root / ".."));
    CHECK_FALSE(HtmlHelper::IsParentDirectoryUnderRoot(root, root / "sub" / ".." / ".." / "x"));
    CHECK(HtmlHelper::IsParentDirectoryUnderRoot(root, root / "sub" / "x"));

    // root that is itself a symlink is accepted; not-yet-existing paths under it too.
    fs::create_directory_symlink(root, base / "rootlink");
    CHECK(HtmlHelper::IsPathUnderRoot(base / "rootlink", base / "rootlink" / "new" / "file.txt"));
    CHECK(HtmlHelper::IsPathUnderRoot(root, root / "does" / "not" / "exist"));
    fs::path canonicalRoot = fs::weakly_canonical(base / "rootlink");
    CHECK(HtmlHelper::IsPathUnderCanonicalRoot(canonicalRoot, base / "rootlink" / "a.txt"));
    CHECK_FALSE(HtmlHelper::IsPathUnderCanonicalRoot(canonicalRoot, base / "outside" / "x"));
    fs::path resolved;
    CHECK(HtmlHelper::TryResolveUnderCanonicalRoot(canonicalRoot, {"a", "b.txt"}, &resolved));
    CHECK_FALSE(HtmlHelper::TryResolveUnderCanonicalRoot(canonicalRoot, {"dirlink", "x"}, &resolved));
    fs::remove_all(base);
}

TEST_CASE("IsFileUnderRoot", "[web_path_safety]")
{
    fs::path base = fs::temp_directory_path() / "pipedal_file_under_root_test";
    fs::remove_all(base);
    fs::create_directories(base / "root" / "sub");
    fs::create_directories(base / "outside");
    fs::path root = base / "root";

    CHECK(IsFileUnderRoot(root, root / "a.wav"));
    CHECK(IsFileUnderRoot(root, root / "sub" / "a.wav"));
    CHECK(IsFileUnderRoot(root / "", root / "a.wav"));            // trailing separator on the root
    CHECK_FALSE(IsFileUnderRoot(root, root));                     // the root itself is not a file in it
    CHECK_FALSE(IsFileUnderRoot(root, base / "root2" / "a.wav")); // string-prefix sibling
    CHECK_FALSE(IsFileUnderRoot(root, base / "outside" / "a.wav"));
    CHECK_FALSE(IsFileUnderRoot(root, root / ".." / "outside" / "a.wav"));
    CHECK_FALSE(IsFileUnderRoot(root, fs::path("root") / "a.wav")); // relative: not lexically under
    CHECK_FALSE(IsFileUnderRoot("", root / "a.wav"));

    // file symlink out of the root is allowed; directory symlink out of the root is not.
    { std::ofstream(base / "outside" / "x.wav") << "x"; }
    fs::create_symlink(base / "outside" / "x.wav", root / "linked.wav");
    CHECK(IsFileUnderRoot(root, root / "linked.wav"));
    fs::create_directory_symlink(base / "outside", root / "dirlink");
    CHECK_FALSE(IsFileUnderRoot(root, root / "dirlink" / "x.wav"));

    // IsPathInAudioUploads is IsFileUnderRoot(AUDIO_UPLOADS_ROOT) plus a safe file name.
    CHECK(IsPathInAudioUploads(AUDIO_UPLOADS_ROOT / "shared" / "a.wav") ==
          IsFileUnderRoot(AUDIO_UPLOADS_ROOT, AUDIO_UPLOADS_ROOT / "shared" / "a.wav"));
    CHECK_FALSE(IsPathInAudioUploads("/var/pipedal/audio_uploads2/a.wav"));
    CHECK_FALSE(IsPathInAudioUploads(AUDIO_UPLOADS_ROOT / ".." / "a.wav"));

    // IsSameDirectory: canonical comparison, so a symlinked alias matches.
    fs::create_directory_symlink(root, base / "rootlink");
    CHECK(IsSameDirectory(root, base / "rootlink"));
    CHECK(IsSameDirectory(root, root / "sub" / ".."));
    CHECK_FALSE(IsSameDirectory(root, base / "outside"));
    fs::remove_all(base);
}
