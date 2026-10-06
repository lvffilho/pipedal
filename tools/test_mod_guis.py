#!/usr/bin/python3
# Copyright (c) Robin E.R. Davies
#
# Permission is hereby granted, free of charge, to any person obtaining a copy of
# this software and associated documentation files (the "Software"), to deal in
# the Software without restriction, including without limitation the rights to
# use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
# the Software, and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
# FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
# COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
# IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

"""Tests for tools/mod_guis.py (install-mod-guis.sh), offline, with made-up plugins
and a made-up MOD source. Run with: /usr/bin/python3 tools/test_mod_guis.py"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(TOOLS, "mod_guis.py")
PLUGIN = "urn:pipedal:test:modgui-installer"
ADAPTED_DIR = ("pipedal-adapted", "urn_pipedal_test_modgui-installer")

sys.path.insert(0, TOOLS)
sys.dont_write_bytecode = True  # no __pycache__ in tools/
import mod_guis  # noqa: E402

PLUGIN_HEADER = """@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .
<%s> a lv2:Plugin ; lv2:binary <test.so> ;
    lv2:port [ a lv2:AudioPort, lv2:InputPort ; lv2:index 0 ; lv2:symbol "in" ; lv2:name "In" ] ,
             [ a lv2:AudioPort, lv2:OutputPort ; lv2:index 1 ; lv2:symbol "out" ; lv2:name "Out" ]""" % PLUGIN


def plugin_ttl(*controls):
    """A plugin description with audio ports and the given input control ports,
    (symbol, name[, minimum, maximum]), from index 2 up."""
    text = PLUGIN_HEADER
    for index, control in enumerate(controls, 2):
        symbol, name = control[:2]
        minimum, maximum = control[2:] if len(control) > 2 else (0.0, 1.0)
        text += (' ,\n             [ a lv2:ControlPort, lv2:InputPort ; lv2:index %d ; lv2:symbol "%s" ; '
                 'lv2:name "%s" ; lv2:default %r ; lv2:minimum %r ; lv2:maximum %r ]'
                 % (index, symbol, name, float(minimum), float(minimum), float(maximum)))
    return text + " .\n"


PLUGIN_TTL = plugin_ttl(("gain", "Gain"))

GOOD_GUI = """
        modgui:resourcesDirectory <modgui> ;
        modgui:iconTemplate <modgui/icon.html> ;
        modgui:stylesheet <modgui/style.css> ;
        modgui:thumbnail <modgui/thumb.png> ;
        modgui:label "Test" ;
        modgui:port [ lv2:index 0 ; lv2:symbol "gain" ; lv2:name "Gain" ] ;
"""


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


class InstallerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="pipedal-mod-guis-test-")
        self.lv2 = os.path.join(self.tmp, "lv2")
        self.source = os.path.join(self.tmp, "mod-lv2-data")
        self.dest = os.path.join(self.tmp, "dest")
        write(os.path.join(self.lv2, "test.lv2", "manifest.ttl"), PLUGIN_TTL)
        self.bundle = os.path.join(self.source, "plugins", "test.lv2")
        # MOD's bundle: its own plugin description (never to be copied) and a GUI.
        write(os.path.join(self.bundle, "manifest.ttl"), PLUGIN_TTL.replace(
            "lv2:binary <test.so> ;", "lv2:binary <test.so> ; rdfs:seeAlso <modgui.ttl> ;"))
        write(os.path.join(self.bundle, "modgui", "icon.html"),
              '<div class="mod-pedal"><div mod-role="input-control-port" mod-port-symbol="gain"></div></div>\n')
        write(os.path.join(self.bundle, "modgui", "style.css"), ".mod-pedal {}\n")
        write(os.path.join(self.bundle, "modgui", "thumb.png"), "")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def write_plugins(self, installed, mod):
        """Replaces the descriptions of the installed plugin and of MOD's build of it."""
        write(os.path.join(self.lv2, "test.lv2", "manifest.ttl"), installed)
        write(os.path.join(self.bundle, "manifest.ttl"), mod.replace(
            "lv2:binary <test.so> ;", "lv2:binary <test.so> ; rdfs:seeAlso <modgui.ttl> ;"))

    def write_icon(self, *symbols):
        """An icon template with a knob for each symbol; the first also gets a value display."""
        knobs = "".join('<div class="knob"><div mod-role="input-control-port" mod-port-symbol="%s"></div>'
                        '<span>%s</span></div>\n' % (s, s) for s in symbols)
        write(os.path.join(self.bundle, "modgui", "icon.html"),
              '<div class="mod-pedal">\n%s<div mod-role="input-control-value" mod-port-symbol="%s"></div>\n'
              '<br><img src="x.png"></div>\n' % (knobs, symbols[0]))

    def write_aliases(self, text):
        path = os.path.join(self.tmp, "aliases.txt")
        write(path, text)
        return path

    def overlay_file(self, *parts):
        with open(os.path.join(self.overlay(), *parts)) as f:
            return f.read()

    def write_gui(self, gui_body):
        write(os.path.join(self.bundle, "modgui.ttl"),
              "@prefix modgui: <http://moddevices.com/ns/modgui#> .\n"
              "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
              "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n"
              "<%s> modgui:gui [ %s ] .\n" % (PLUGIN, gui_body))

    def run_script(self, *extra):
        result = subprocess.run(
            [sys.executable, SCRIPT, "--source", self.source, "--lv2-dir", self.lv2, "--dest", self.dest] + list(extra),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        return result.stdout

    def overlay(self):
        return os.path.join(self.dest, "test-modgui.lv2")

    def assert_not_installed(self, output, reason):
        self.assertFalse(os.path.exists(self.overlay()), output)
        self.assertIn(reason, output)

    def test_install_and_uninstall(self):
        self.write_gui(GOOD_GUI)
        output = self.run_script()
        self.assertIn("1 plugin with an added MOD GUI", output)
        with open(os.path.join(self.overlay(), "modgui.ttl")) as f:
            ttl = f.read()
        self.assertIn("<modgui/icon.html>", ttl)
        for forbidden in ("binary", "lv2core#port", "AudioPort", "file:"):
            self.assertNotIn(forbidden, ttl)
        self.assertTrue(os.path.isfile(os.path.join(self.overlay(), "modgui", "style.css")))
        self.assertIn("1 up to date", self.run_script())
        self.assertIn("Removing", self.run_script("--uninstall"))
        self.assertFalse(os.path.exists(self.overlay()))
        self.assertIn("Nothing to uninstall", self.run_script("--uninstall"))

    def test_gui_with_plugin_data_is_rejected(self):
        self.write_gui(GOOD_GUI + " lv2:binary <evil.so> ;")
        self.assert_not_installed(self.run_script(), "non-MOD-GUI property")

    def test_gui_must_be_a_blank_node(self):
        write(os.path.join(self.bundle, "modgui.ttl"),
              "@prefix modgui: <http://moddevices.com/ns/modgui#> .\n"
              "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
              "<%s> modgui:gui <#gui> .\n<#gui> %s .\n" % (PLUGIN, GOOD_GUI.strip().rstrip(";")))
        self.assert_not_installed(self.run_script(), "not a blank node")

    def test_nested_file_reference_is_rejected(self):
        self.write_gui(GOOD_GUI.replace('lv2:name "Gain" ]', 'lv2:name "Gain" ; rdfs:seeAlso <file:///etc/shadow> ]'))
        self.assert_not_installed(self.run_script(), "nested property")

    def test_nested_modgui_file_reference_is_rejected(self):
        self.write_gui(GOOD_GUI.replace('lv2:name "Gain" ]', 'lv2:name "Gain" ; modgui:x <file:///etc/shadow> ]'))
        self.assert_not_installed(self.run_script(), "refers to a file")

    def test_file_through_other_property_is_rejected(self):
        self.write_gui(GOOD_GUI + " modgui:documentation <file:///etc/shadow> ;")
        self.assert_not_installed(self.run_script(), "refers to a file")

    def test_file_outside_bundle_is_rejected(self):
        self.write_gui(GOOD_GUI.replace("<modgui/style.css>", "<file:///etc/hostname>"))
        self.assert_not_installed(self.run_script(), "outside its bundle")

    def test_intermediate_symlink_escape_is_rejected(self):
        os.symlink("/etc", os.path.join(self.bundle, "esc"))
        self.write_gui(GOOD_GUI.replace("<modgui/style.css>", "<esc/hostname>"))
        self.assert_not_installed(self.run_script(), "symbolic link (esc)")
        self.write_gui(GOOD_GUI.replace("modgui:resourcesDirectory <modgui>", "modgui:resourcesDirectory <esc>"))
        self.assert_not_installed(self.run_script(), "symbolic link (esc)")

    def test_symlink_inside_resources_is_rejected(self):
        os.symlink("/etc/passwd", os.path.join(self.bundle, "modgui", "evil.png"))
        self.write_gui(GOOD_GUI)
        self.assert_not_installed(self.run_script(), "symbolic link (modgui/evil.png)")

    def test_symlinked_sibling_resources_are_rejected(self):
        # Like mod-lv2-data's *-bad.lv2 bundles: links to a sibling bundle's files.
        sibling = os.path.join(self.source, "plugins", "other.lv2", "modgui")
        shutil.move(os.path.join(self.bundle, "modgui"), sibling)
        os.symlink("../other.lv2/modgui", os.path.join(self.bundle, "modgui"))
        self.write_gui(GOOD_GUI)
        self.assert_not_installed(self.run_script(), "symbolic link (modgui)")

    def test_incompatible_gui_is_rejected(self):
        # Its only control is for a port the installed plugin doesn't have.
        self.write_icon("drive")
        self.write_gui(GOOD_GUI.replace('lv2:symbol "gain"', 'lv2:symbol "drive"'))
        self.assert_not_installed(self.run_script(), "most of its controls refer to ports")

    def test_port_of_wrong_kind_is_rejected(self):
        self.write_gui(GOOD_GUI.replace('lv2:symbol "gain"', 'lv2:symbol "in"'))
        self.assert_not_installed(self.run_script(), "port 'in'")

    def test_alias_maps_port_in_ttl_and_template(self):
        self.write_plugins(plugin_ttl(("gain", "Gain"), ("tone", "Tone")),
                           plugin_ttl(("level", "Level"), ("tone", "Tone")))
        self.write_icon("level", "tone")
        self.write_gui(GOOD_GUI.replace('lv2:symbol "gain" ; lv2:name "Gain" ]',
                                        'lv2:symbol "level" ; lv2:name "Level" ] , '
                                        '[ lv2:index 1 ; lv2:symbol "tone" ; lv2:name "Tone" ]'))
        aliases = self.write_aliases("# comment\n%s level gain  # renamed since MOD's build\n" % PLUGIN)
        output = self.run_script("--port-aliases", aliases)
        self.assertIn("1 plugin with an added MOD GUI, 1 of them adapted", output)
        self.assertIn("adapted: port 'level' -> 'gain' (alias: renamed since MOD's build)", output)
        ttl = self.overlay_file("modgui.ttl")
        self.assertIn('lv2core#symbol> "gain"', ttl)
        self.assertIn('lv2core#symbol> "tone"', ttl)
        self.assertNotIn('"level"', ttl)
        # The template is adapted in a copy of the overlay's own; the original is
        # copied unchanged (other GUIs of the bundle may use it).
        self.assertIn("<%s/icon.html>" % "/".join(ADAPTED_DIR), ttl)
        self.assertNotIn("<modgui/icon.html>", ttl)
        icon = self.overlay_file(*ADAPTED_DIR, "icon.html")
        self.assertEqual(icon.count('mod-port-symbol="gain"'), 2)
        self.assertNotIn('"level"', icon)
        self.assertIn('mod-port-symbol="level"', self.overlay_file("modgui", "icon.html"))
        self.assertIn("1 up to date", self.run_script("--port-aliases", aliases))

    def test_alias_to_missing_port_is_rejected(self):
        self.write_icon("level", "gain")
        self.write_gui(GOOD_GUI)
        aliases = self.write_aliases("%s level volume  # wrong\n" % PLUGIN)
        self.assert_not_installed(self.run_script("--port-aliases", aliases), "maps port 'level' to 'volume'")

    def test_alias_can_hide_a_control(self):
        self.write_icon("gain", "level")
        self.write_gui(GOOD_GUI)
        aliases = self.write_aliases("%s level -  # not in this version\n" % PLUGIN)
        output = self.run_script("--port-aliases", aliases)
        self.assertIn("adapted: control 'level' hidden (alias: not in this version)", output)
        self.assertNotIn('mod-port-symbol="level"', self.overlay_file(*ADAPTED_DIR, "icon.html"))

    def test_typo_is_mapped_automatically(self):
        self.write_plugins(plugin_ttl(("gain", "Gain"), ("exp_fm_gain", "Exp FM gain", 0, 10)),
                           plugin_ttl(("gain", "Gain"), ("exp_gm_gain", "Exp FM gain", 0, 10)))
        self.write_icon("gain")
        self.write_gui(GOOD_GUI.replace('lv2:name "Gain" ]', 'lv2:name "Gain" ] , '
                                        '[ lv2:index 1 ; lv2:symbol "exp_gm_gain" ; lv2:name "Exp FM" ]'))
        output = self.run_script()
        self.assertIn("adapted: port 'exp_gm_gain' -> 'exp_fm_gain' (automatic: same index and name)", output)
        ttl = self.overlay_file("modgui.ttl")
        self.assertIn('"exp_fm_gain"', ttl)
        self.assertNotIn("exp_gm_gain", ttl)
        # The template needed no change, so the original is used.
        self.assertIn("<modgui/icon.html>", ttl)
        self.assertFalse(os.path.exists(os.path.join(self.overlay(), ADAPTED_DIR[0])))

    def test_typo_in_template_only_is_mapped_automatically(self):
        # Like MOD's midifilter GUIs: the template has a symbol that neither build has.
        ports = plugin_ttl(("gain", "Gain"), ("channelf", "Filter Channel", 0, 16))
        self.write_plugins(ports, ports)
        self.write_icon("channel", "gain")
        self.write_gui(GOOD_GUI)
        output = self.run_script()
        self.assertIn("adapted: port 'channel' -> 'channelf' (automatic: nearly the same symbol)", output)
        icon = self.overlay_file(*ADAPTED_DIR, "icon.html")
        self.assertEqual(icon.count('mod-port-symbol="channelf"'), 2)

    def test_ambiguous_port_is_hidden(self):
        ports = plugin_ttl(("gain1", "Gain 1"), ("gain2", "Gain 2"), ("tone", "Tone"))
        self.write_plugins(ports, ports)
        self.write_icon("gain3", "gain1", "gain2", "tone")
        self.write_gui(GOOD_GUI.replace('lv2:symbol "gain" ; lv2:name "Gain" ]',
                                        'lv2:symbol "gain3" ; lv2:name "Gain 3" ] , '
                                        '[ lv2:index 1 ; lv2:symbol "tone" ; lv2:name "Tone" ]'))
        output = self.run_script()
        self.assertIn("adapted: control 'gain3' hidden (ambiguous: gain1, gain2)", output)
        ttl = self.overlay_file("modgui.ttl")
        self.assertNotIn("gain3", ttl)
        self.assertIn('"tone"', ttl)
        icon = self.overlay_file(*ADAPTED_DIR, "icon.html")
        # The elements of the control (knob, value display) are gone; the rest,
        # including the label next to the knob, is as it was.
        self.assertNotIn('mod-port-symbol="gain3"', icon)
        self.assertIn('<div class="knob"><span>gain3</span></div>', icon)
        self.assertEqual(icon.count('mod-role="input-control-port"'), 3)
        self.assertNotIn('mod-role="input-control-value"', icon)
        self.assertIn('<br><img src="x.png"></div>', icon)

    def test_adapted_gui_passes_the_gate(self):
        ports = plugin_ttl(("gain1", "Gain 1"), ("gain2", "Gain 2"), ("tone", "Tone"))
        self.write_plugins(ports, ports)
        self.write_icon("gain3", "tone", "gain1")
        self.write_gui(GOOD_GUI.replace('lv2:symbol "gain"', 'lv2:symbol "tone"'))
        self.run_script("--port-aliases", self.write_aliases("%s gain3 gain2\n" % PLUGIN))
        graph = mod_guis.Graph()
        graph.add_file(os.path.join(self.overlay(), "manifest.ttl"))
        graph.add_file(os.path.join(self.overlay(), "modgui.ttl"))
        gui = graph.value(("U", PLUGIN), mod_guis.MODGUI + "gui")
        icon = mod_guis.uri_to_path(graph.value(gui, mod_guis.MODGUI + "iconTemplate")[1])
        self.assertEqual({symbol for _, symbol in mod_guis._scan_template(icon).ports}, {"gain2", "tone", "gain1"})
        # The gate that the overlay passed rejects it for a plugin without gain2.
        info = mod_guis.InstalledPlugin(PLUGIN, None, "Test")
        for symbol in ("gain1", "tone"):
            info.ports[symbol] = mod_guis.PortInfo(symbol, True, "control")
        candidate = mod_guis.Candidate.__new__(mod_guis.Candidate)
        candidate.uri, candidate.installed = PLUGIN, info
        with self.assertRaisesRegex(mod_guis.Error, "fails the compatibility check.*'gain2'"):
            mod_guis.verify_overlay(graph, [candidate])
        info.ports["gain2"] = mod_guis.PortInfo("gain2", True, "control")
        mod_guis.verify_overlay(graph, [candidate])

    def test_template_rewriting(self):
        port_map = {"a": ("b", ""), "x": (None, "")}
        text = ('<div mod-port-symbol="a"></div><p mod-port-symbol=\'x\'><span mod-port-symbol="a">'
                '<br></span></p><img mod-port-symbol="x"/><input mod-port-symbol="x">'
                '{{#controls}}<i mod-port-symbol="{{symbol}}"></i>{{/controls}}')
        self.assertEqual(mod_guis.adapt_template(text, port_map),
                         '<div mod-port-symbol="b"></div>{{#controls}}<i mod-port-symbol="{{symbol}}"></i>'
                         '{{/controls}}')
        with self.assertRaises(mod_guis.Error):
            mod_guis.adapt_template('<div><p mod-port-symbol="x"></div>', port_map)

    def test_lock_is_outside_the_destination(self):
        self.write_gui(GOOD_GUI)
        os.makedirs(self.dest)
        legacy = os.path.join(self.dest, ".pipedal-mod-gui.lock")
        write(legacy, "")
        self.run_script()
        self.assertEqual(os.listdir(self.dest), ["test-modgui.lv2"])
        self.assertFalse(mod_guis.lock_path().startswith(self.tmp))
        self.assertTrue(os.path.isfile(mod_guis.lock_path()))
        write(legacy, "")
        self.run_script("--uninstall")
        self.assertEqual(os.listdir(self.dest), [])

    def test_foreign_overlay_is_left_alone(self):
        self.write_gui(GOOD_GUI)
        os.makedirs(self.overlay())
        output = self.run_script()
        self.assertIn("was not installed by this script", output)
        self.assertEqual(os.listdir(self.overlay()), [])


if __name__ == "__main__":
    unittest.main()
