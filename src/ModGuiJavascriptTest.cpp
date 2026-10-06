// Copyright (c) Robin E.R. Davies
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

// /resources/_/javascript: a ModGUI's modgui:javascript, served verbatim.

#include "pch.h"
#include "catch.hpp"
#include <filesystem>
#include <fstream>
#include <map>
#include <unistd.h>
#include "PluginHost.hpp"
#include "ModGui.hpp"
#include "WebServerMod.hpp"

using namespace pipedal;
namespace fs = std::filesystem;

namespace
{
    class TestResponse : public HttpResponse
    {
    public:
        std::map<std::string, std::string> headers;
        std::string body;
        fs::path bodyFile;
        size_t contentLength = (size_t)-1;

        virtual void set(const std::string &key, const std::string &value) override { headers[key] = value; }
        virtual void setContentLength(size_t size) override { contentLength = size; }
        virtual void setBody(const std::string &body) override { this->body = body; }
        virtual void setBodyFile(std::shared_ptr<TemporaryFile> &temporaryFile) override { FAIL("unexpected temporary file"); }
        virtual void setBodyFile(std::filesystem::path &path, bool deleteWhenDone) override
        {
            REQUIRE(!deleteWhenDone);
            bodyFile = path;
        }
        virtual void clearBody() override { body.clear(); }
        virtual void keepAlive(bool value) override {}
    };

    void WriteFile(const fs::path &path, const std::string &text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream f(path);
        f << text;
    }

    const char *JS_TEST_PLUGIN_URI = "http://two-play.com/test/pipedal-modgui-javascript-test";
    const char *SCRIPT = "function (event, funcs) {\n    if (event.type == 'start') {}\n}\n";
}

TEST_CASE("ModGui javascript", "[mod_gui_javascript]")
{
    fs::path root = fs::temp_directory_path() / ("pipedal-modgui-js-test-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::path bundle = root / "jstest.lv2";
    WriteFile(bundle / "manifest.ttl",
              "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
              "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
              "<" + std::string(JS_TEST_PLUGIN_URI) + ">\n"
              "    a lv2:Plugin ;\n"
              "    lv2:binary <jstest.so> ;\n"
              "    rdfs:seeAlso <jstest.ttl> .\n");
    WriteFile(bundle / "jstest.ttl",
              "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
              "@prefix doap: <http://usefulinc.com/ns/doap#> .\n"
              "@prefix modgui: <http://moddevices.com/ns/modgui#> .\n"
              "<" + std::string(JS_TEST_PLUGIN_URI) + ">\n"
              "    a lv2:Plugin, lv2:AmplifierPlugin ;\n"
              "    doap:name \"PiPedal ModGUI javascript test\" ;\n"
              "    modgui:gui [\n"
              "        modgui:resourcesDirectory <modgui> ;\n"
              "        modgui:iconTemplate <modgui/icon-test.html> ;\n"
              "        modgui:stylesheet <modgui/stylesheet-test.css> ;\n"
              "        modgui:javascript <modgui/script-test.js> ;\n"
              "    ] ;\n"
              "    lv2:port [\n"
              "        a lv2:AudioPort, lv2:InputPort ; lv2:index 0 ; lv2:symbol \"in\" ; lv2:name \"In\"\n"
              "    ] , [\n"
              "        a lv2:AudioPort, lv2:OutputPort ; lv2:index 1 ; lv2:symbol \"out\" ; lv2:name \"Out\"\n"
              "    ] , [\n"
              "        a lv2:ControlPort, lv2:InputPort ; lv2:index 2 ; lv2:symbol \"gain\" ; lv2:name \"Gain\" ;\n"
              "        lv2:default 0.5 ; lv2:minimum 0.0 ; lv2:maximum 1.0\n"
              "    ] .\n");
    WriteFile(bundle / "modgui" / "icon-test.html", "<div class=\"mod-pedal\"></div>\n");
    WriteFile(bundle / "modgui" / "stylesheet-test.css", ".mod-pedal {}\n");
    WriteFile(bundle / "modgui" / "script-test.js", SCRIPT);
    root = fs::canonical(root);
    bundle = root / "jstest.lv2";

    PluginHost pluginHost;
    pluginHost.LoadLilv(("/usr/lib/lv2:" + root.string()).c_str());
    auto pluginInfo = pluginHost.GetPluginInfo(JS_TEST_PLUGIN_URI);
    REQUIRE(pluginInfo);
    auto modGui = pluginInfo->modGui();
    REQUIRE(modGui);
    REQUIRE(fs::path(modGui->javascript()) == bundle / "modgui" / "script-test.js");

    SECTION("serves the script verbatim, cacheable")
    {
        TestResponse res;
        std::error_code ec;
        ModWebIntercept::ServeJavascript(modGui->javascript(), res, ec);
        REQUIRE(!ec);
        REQUIRE(res.headers["Content-Type"] == "text/javascript");
        REQUIRE(res.bodyFile == fs::path(modGui->javascript()));
        REQUIRE(res.contentLength == std::string(SCRIPT).length());
        REQUIRE(res.headers.contains("ETag"));
        REQUIRE(res.headers.contains(HttpField::LastModified));
        REQUIRE(res.headers["Cache-Control"].find("max-age") != std::string::npos);
    }
    SECTION("a ModGUI without a script is not found")
    {
        TestResponse res;
        std::error_code ec;
        ModWebIntercept::ServeJavascript("", res, ec);
        REQUIRE(ec == std::make_error_code(std::errc::no_such_file_or_directory));
        REQUIRE(res.bodyFile.empty());
    }
    SECTION("a missing script or a directory is not found")
    {
        for (const fs::path &path : {bundle / "modgui" / "missing.js", bundle / "modgui"})
        {
            INFO(path.string());
            TestResponse res;
            std::error_code ec;
            ModWebIntercept::ServeJavascript(path.string(), res, ec);
            REQUIRE(ec == std::make_error_code(std::errc::no_such_file_or_directory));
            REQUIRE(res.bodyFile.empty());
        }
    }
    fs::remove_all(root);
}
