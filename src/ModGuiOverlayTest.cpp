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

// A ModGUI supplied by a separate overlay bundle (tools/install-mod-guis.sh) must
// be found whatever the LV2_PATH order. lilv only reads the data files it knew
// about when it first found the plugin, so an overlay scanned after the plugin's
// own bundle (PiPedal's default path is /usr/lib/lv2:/usr/local/lib/lv2) used to
// be ignored.

#include "pch.h"
#include "catch.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include "PluginHost.hpp"
#include "ModGui.hpp"
#include "ModTemplateGenerator.hpp"

using namespace pipedal;
namespace fs = std::filesystem;

static const char *OVERLAY_TEST_PLUGIN_URI = "http://two-play.com/test/pipedal-modgui-overlay-test";

static void WriteFile(const fs::path &path, const std::string &text)
{
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    f << text;
}

static void MakeOverlayTestBundles(const fs::path &root, bool brokenOwnGui)
{
    // The plugin's own bundle: no ModGUI. (The binary is never loaded.)
    fs::path plugin = root / "system" / "overlaytest.lv2";
    WriteFile(plugin / "manifest.ttl",
              "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
              "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
              "<" + std::string(OVERLAY_TEST_PLUGIN_URI) + ">\n"
              "    a lv2:Plugin ;\n"
              "    lv2:binary <overlaytest.so> ;\n"
              "    rdfs:seeAlso <overlaytest.ttl> .\n");
    WriteFile(plugin / "overlaytest.ttl",
              "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
              "@prefix doap: <http://usefulinc.com/ns/doap#> .\n"
              "<" + std::string(OVERLAY_TEST_PLUGIN_URI) + ">\n"
              "    a lv2:Plugin, lv2:AmplifierPlugin ;\n"
              "    doap:name \"PiPedal ModGUI overlay test\" ;\n"
              "    lv2:port [\n"
              "        a lv2:AudioPort, lv2:InputPort ; lv2:index 0 ; lv2:symbol \"in\" ; lv2:name \"In\"\n"
              "    ] , [\n"
              "        a lv2:AudioPort, lv2:OutputPort ; lv2:index 1 ; lv2:symbol \"out\" ; lv2:name \"Out\"\n"
              "    ] , [\n"
              "        a lv2:ControlPort, lv2:InputPort ; lv2:index 2 ; lv2:symbol \"gain\" ; lv2:name \"Gain\" ;\n"
              "        lv2:default 0.5 ; lv2:minimum 0.0 ; lv2:maximum 1.0\n"
              "    ] .\n");
    if (brokenOwnGui)
    {
        // Like the guitarix-lv2 package: a modgui:gui whose resource files weren't packaged.
        WriteFile(plugin / "modgui-broken.ttl",
                  "@prefix modgui: <http://moddevices.com/ns/modgui#> .\n"
                  "<" + std::string(OVERLAY_TEST_PLUGIN_URI) + ">\n"
                  "    modgui:gui [\n"
                  "        modgui:resourcesDirectory <modgui> ;\n"
                  "        modgui:iconTemplate <modgui/icon-missing.html> ;\n"
                  "    ] .\n");
        WriteFile(plugin / "manifest.ttl",
                  "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .\n"
                  "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
                  "<" + std::string(OVERLAY_TEST_PLUGIN_URI) + ">\n"
                  "    a lv2:Plugin ;\n"
                  "    lv2:binary <overlaytest.so> ;\n"
                  "    rdfs:seeAlso <overlaytest.ttl>, <modgui-broken.ttl> .\n");
    }

    // The overlay bundle, laid out as tools/install-mod-guis.sh installs it.
    fs::path overlay = root / "local" / "overlaytest-modgui.lv2";
    WriteFile(overlay / "manifest.ttl",
              "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
              "<" + std::string(OVERLAY_TEST_PLUGIN_URI) + "> rdfs:seeAlso <modguis.ttl> .\n");
    WriteFile(overlay / "modguis.ttl",
              "@prefix modgui: <http://moddevices.com/ns/modgui#> .\n"
              "<" + std::string(OVERLAY_TEST_PLUGIN_URI) + ">\n"
              "    modgui:gui [\n"
              "        modgui:resourcesDirectory <modgui> ;\n"
              "        modgui:iconTemplate <modgui/icon-test.html> ;\n"
              "        modgui:stylesheet <modgui/stylesheet-test.css> ;\n"
              "        modgui:screenshot <modgui/screenshot-test.png> ;\n"
              "        modgui:thumbnail <modgui/thumbnail-test.png> ;\n"
              "        modgui:brand \"PiPedal\" ;\n"
              "        modgui:label \"Overlay test\" ;\n"
              "    ] .\n");
    WriteFile(overlay / "modgui" / "icon-test.html", "<div class=\"mod-pedal\"></div>\n");
    WriteFile(overlay / "modgui" / "stylesheet-test.css", ".mod-pedal {}\n");
    WriteFile(overlay / "modgui" / "screenshot-test.png", "");
    WriteFile(overlay / "modgui" / "thumbnail-test.png", "");
}

static void CheckOverlayModGui(const fs::path &root, const std::string &lv2Path)
{
    PluginHost pluginHost;
    pluginHost.LoadLilv(lv2Path.c_str());

    auto pluginInfo = pluginHost.GetPluginInfo(OVERLAY_TEST_PLUGIN_URI);
    REQUIRE(pluginInfo);
    auto modGui = pluginInfo->modGui();
    REQUIRE(modGui);

    fs::path overlay = fs::canonical(root / "local" / "overlaytest-modgui.lv2");
    REQUIRE(fs::path(modGui->resourceDirectory()) == overlay / "modgui");
    REQUIRE(fs::path(modGui->iconTemplate()) == overlay / "modgui" / "icon-test.html");
    REQUIRE(fs::exists(modGui->iconTemplate()));
    REQUIRE(fs::exists(modGui->stylesheet()));
    REQUIRE(fs::exists(modGui->screenshot()));
    REQUIRE(fs::exists(modGui->thumbnail()));
}

TEST_CASE("ModGui overlay bundle", "[mod_gui_overlay]")
{
    // With brokenOwnGui, the plugin's own bundle also has a modgui:gui whose files are
    // missing; the overlay's must win.
    bool brokenOwnGui = GENERATE(false, true);
    INFO("brokenOwnGui=" << brokenOwnGui);
    fs::path root = fs::temp_directory_path() / ("pipedal-modgui-overlay-test-" + std::to_string(getpid()));
    fs::remove_all(root);
    MakeOverlayTestBundles(root, brokenOwnGui);
    root = fs::canonical(root);

    // /usr/lib/lv2 supplies the lv2 core vocabulary, as in a real install.
    SECTION("overlay after the plugin's bundle (PiPedal's default order)")
    {
        CheckOverlayModGui(root, "/usr/lib/lv2:" + (root / "system").string() + ":" + (root / "local").string());
    }
    SECTION("overlay before the plugin's bundle")
    {
        CheckOverlayModGui(root, "/usr/lib/lv2:" + (root / "local").string() + ":" + (root / "system").string());
    }
    fs::remove_all(root);
}

// Checks the overlays actually installed by tools/install-mod-guis.sh, in
// $PIPEDAL_MOD_GUI_DIR if set (e.g. after --dest <dir>), else /usr/local/lib/lv2:
// every plugin an overlay lists must get the overlay's ModGUI, with all of its
// files, and its templates must render.
TEST_CASE("ModGui installed overlays", "[mod_gui_overlays]")
{
    const char *envDir = getenv("PIPEDAL_MOD_GUI_DIR");
    fs::path overlayDir = envDir ? fs::path(envDir) : fs::path("/usr/local/lib/lv2");

    std::vector<fs::path> overlays;
    if (fs::is_directory(overlayDir))
    {
        for (const auto &entry : fs::directory_iterator(overlayDir))
        {
            if (fs::exists(entry.path() / ".pipedal-mod-gui"))
            {
                overlays.push_back(fs::canonical(entry.path()));
            }
        }
    }
    if (overlays.empty())
    {
        WARN("No overlays from tools/install-mod-guis.sh in " << overlayDir << ". Skipping.");
        return;
    }
    std::sort(overlays.begin(), overlays.end());

    PluginHost pluginHost;
    pluginHost.LoadLilv(("/usr/lib/lv2:" + overlayDir.string()).c_str());

    size_t checked = 0, rendered = 0, notOffered = 0;
    for (const auto &overlay : overlays)
    {
        std::ifstream marker(overlay / ".pipedal-mod-gui");
        std::string line;
        while (std::getline(marker, line))
        {
            if (line.rfind("plugin=", 0) != 0)
            {
                continue;
            }
            std::string uri = line.substr(7);
            INFO(overlay.string() << ": " << uri);
            // PiPedal doesn't offer every plugin (e.g. MIDI-only or CV plugins). Check
            // the ModGUI of those straight from lilv; their templates are never rendered.
            auto pluginInfo = pluginHost.GetPluginInfo(uri);
            ModGui::ptr modGui;
            if (pluginInfo)
            {
                modGui = pluginInfo->modGui();
            }
            else
            {
                AutoLilvNode uriNode = lilv_new_uri(pluginHost.getWorld(), uri.c_str());
                const LilvPlugin *lilvPlugin = lilv_plugins_get_by_uri(
                    lilv_world_get_all_plugins(pluginHost.getWorld()), uriNode);
                REQUIRE(lilvPlugin);
                modGui = ModGui::Create(&pluginHost, lilvPlugin);
                ++notOffered;
            }
            REQUIRE(modGui);
            fs::path resourceDirectory = modGui->resourceDirectory();
            REQUIRE(resourceDirectory.string().rfind(overlay.string() + "/", 0) == 0);
            REQUIRE(fs::is_directory(resourceDirectory));
            for (const std::string &file : {modGui->iconTemplate(), modGui->stylesheet(), modGui->thumbnail()})
            {
                INFO(file);
                REQUIRE(!file.empty());
                REQUIRE(fs::path(file).string().rfind(overlay.string() + "/", 0) == 0);
                REQUIRE(fs::is_regular_file(file));
            }
            for (const std::string &file : {modGui->screenshot(), modGui->settingsTemplate(), modGui->javascript()})
            {
                INFO(file);
                REQUIRE((file.empty() || fs::is_regular_file(file)));
            }

            ++checked;
            if (!pluginInfo)
            {
                continue;
            }
            // As ModWebIntercept renders them.
            json_variant context = MakeModGuiTemplateData(pluginInfo);
            context["_ns"] = "?ns=test&v=1";
            context["_cns"] = "_test_1";
            REQUIRE_NOTHROW(GenerateFromTemplateFile(modGui->iconTemplate(), context));
            if (!modGui->settingsTemplate().empty())
            {
                REQUIRE_NOTHROW(GenerateFromTemplateFile(modGui->settingsTemplate(), context));
            }
            ++rendered;
        }
    }
    REQUIRE(checked != 0);
    WARN("Checked the ModGUIs of " << checked << " plugins in " << overlays.size() << " overlays in " << overlayDir
         << "; rendered the templates of " << rendered << " (" << notOffered << " are plugins PiPedal doesn't offer).");
}
