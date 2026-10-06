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

"""Implementation of tools/install-mod-guis.sh. Run that script, not this file.

Installs MOD GUIs (modgui:gui descriptions plus their resource files) published
by MOD Devices in github.com/moddevices/mod-lv2-data for LV2 plugins that are
installed from distro packages without one. Each GUI goes into an overlay bundle
(<dest>/<bundle>-modgui.lv2) that only adds the modgui:gui data to the installed
plugin; MOD's plugin descriptions and binaries are never installed.

MOD's GUIs were written for MOD's build of each plugin, so a GUI is only installed
if every port and parameter it refers to exists in the installed plugin. A port
that the installed plugin has under another symbol is mapped to it (see "Port
adaptation"), and a control for a port it lacks is hidden; only the overlay's
copies of the GUI are changed.

RDF is read with libserd (via ctypes) and lilv (python3-lilv), the same parsers
lilv-based hosts such as PiPedal use.
"""

import argparse
import ctypes
import ctypes.util
import filecmp
import html.parser
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.parse

MOD_REPO = "https://github.com/moddevices/mod-lv2-data.git"
MOD_COMMIT = "3c6ebef7be59f71b57683c02c4a30f987dcb9abc"
MOD_BUNDLE_DIRS = ["plugins", "plugins-fixed"]

# Fallback for Guitarix plugins whose MOD GUI is not in mod-lv2-data (e.g. the
# Redeye Chump and Big Chump; MOD only has the VibroChump). Matches Debian/Ubuntu
# guitarix-lv2 0.47.0.
GX_REPO = "https://github.com/brummer10/guitarix.git"
GX_TAG = "V0.47.0"
GX_COMMIT = "516e9084e39698d249d25bada8d76f66b4b556da"
GX_BUNDLE_DIRS = ["trunk/src/LV2"]
GX_URI_PREFIX = "http://guitarix.sourceforge.net/"

MARKER = ".pipedal-mod-gui"
LEGACY_MARKERS = [".pipedal-gx-modgui"]  # overlays from the old install-gx-modgui.sh
OVERLAY_TTL = "modgui.ttl"

MODGUI = "http://moddevices.com/ns/modgui#"
LV2 = "http://lv2plug.in/ns/lv2core#"
RDF_TYPE = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type"
RDFS_SEEALSO = "http://www.w3.org/2000/01/rdf-schema#seeAlso"
PATCH = "http://lv2plug.in/ns/ext/patch#"
XSD = "http://www.w3.org/2001/XMLSchema#"

TEMPLATE_PROPERTIES = ["iconTemplate", "settingsTemplate"]
# Port properties compared when matching a port of MOD's build to an installed one.
PORT_PROPERTIES = ("toggled", "enumeration", "integer")
# The only properties of a modgui:gui that may refer to files (in the GUI's bundle).
FILE_PROPERTIES = {MODGUI + p for p in ("resourcesDirectory", "iconTemplate", "settingsTemplate", "javascript",
                                        "stylesheet", "screenshot", "thumbnail")}
# Besides modgui:*, the properties allowed on the nodes a modgui:gui refers to
# (modgui:port, modgui:monitoredOutputs).
NESTED_PREDICATES = {LV2 + "symbol", LV2 + "name", LV2 + "index", LV2 + "shortName", RDF_TYPE}


class Error(Exception):
    pass


def log(message=""):
    print(message, flush=True)


# --------------------------------------------------------------------------
# Turtle parsing with libserd.
#
# Nodes are tuples: ("U", uri), ("B", blank-id), ("L", text, datatype-uri, lang).

class _SerdNode(ctypes.Structure):
    _fields_ = [
        ("buf", ctypes.c_void_p),
        ("n_bytes", ctypes.c_size_t),
        ("n_chars", ctypes.c_size_t),
        ("flags", ctypes.c_uint32),
        ("type", ctypes.c_int),
    ]


_SERD_LITERAL, _SERD_URI, _SERD_CURIE, _SERD_BLANK = 1, 2, 3, 4
_SERD_TURTLE = 1
_PNode = ctypes.POINTER(_SerdNode)
_BaseSink = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, _PNode)
_PrefixSink = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, _PNode, _PNode)
class _SerdError(ctypes.Structure):
    _fields_ = [
        ("status", ctypes.c_int),
        ("filename", ctypes.c_char_p),
        ("line", ctypes.c_uint),
        ("col", ctypes.c_uint),
        ("fmt", ctypes.c_char_p),
        ("args", ctypes.c_void_p),
    ]


_ErrorSink = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(_SerdError))
_StatementSink = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32,
                                  _PNode, _PNode, _PNode, _PNode, _PNode, _PNode)
_serd = None


def _serd_lib():
    global _serd
    if _serd is None:
        name = ctypes.util.find_library("serd-0") or "libserd-0.so.0"
        try:
            lib = ctypes.CDLL(name)
        except OSError:
            raise Error("libserd is required (sudo apt install libserd-0-0).")
        lib.serd_node_new_file_uri.restype = _SerdNode
        lib.serd_node_new_file_uri.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_bool]
        lib.serd_node_free.argtypes = [_PNode]
        lib.serd_env_new.restype = ctypes.c_void_p
        lib.serd_env_new.argtypes = [_PNode]
        lib.serd_env_free.argtypes = [ctypes.c_void_p]
        lib.serd_env_set_base_uri.argtypes = [ctypes.c_void_p, _PNode]
        lib.serd_env_set_prefix.argtypes = [ctypes.c_void_p, _PNode, _PNode]
        lib.serd_env_expand_node.restype = _SerdNode
        lib.serd_env_expand_node.argtypes = [ctypes.c_void_p, _PNode]
        lib.serd_reader_new.restype = ctypes.c_void_p
        lib.serd_reader_new.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
                                        _BaseSink, _PrefixSink, _StatementSink, ctypes.c_void_p]
        lib.serd_reader_set_error_sink.argtypes = [ctypes.c_void_p, _ErrorSink, ctypes.c_void_p]
        lib.serd_reader_add_blank_prefix.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.serd_reader_read_file.restype = ctypes.c_int
        lib.serd_reader_read_file.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.serd_reader_free.argtypes = [ctypes.c_void_p]
        _serd = lib
    return _serd


def file_uri(path, is_dir=False):
    """The file: URI for path, encoded the way serd encodes base URIs."""
    lib = _serd_lib()
    node = lib.serd_node_new_file_uri(os.path.abspath(path).encode(), None, None, True)
    try:
        uri = ctypes.string_at(node.buf, node.n_bytes).decode()
    finally:
        lib.serd_node_free(ctypes.byref(node))
    return uri + "/" if is_dir and not uri.endswith("/") else uri


def uri_to_path(uri):
    if not uri.startswith("file://"):
        return None
    return urllib.parse.unquote(urllib.parse.urlparse(uri).path)


_blank_counter = [0]


def parse_turtle(path):
    """Returns the triples in the Turtle file at path. Raises Error on syntax errors."""
    lib = _serd_lib()
    triples = []
    errors = []
    base = lib.serd_node_new_file_uri(os.path.abspath(path).encode(), None, None, True)
    env = lib.serd_env_new(ctypes.byref(base))
    _blank_counter[0] += 1
    blank_prefix = "f%d_" % _blank_counter[0]

    def text(node):
        return ctypes.string_at(node.buf, node.n_bytes).decode("utf-8")

    def expand(pnode):
        expanded = lib.serd_env_expand_node(env, pnode)
        if not expanded.buf:
            errors.append("cannot expand <%s>" % text(pnode.contents))
            return "urn:invalid"
        try:
            return text(expanded)
        finally:
            lib.serd_node_free(ctypes.byref(expanded))

    def to_node(pnode, datatype=None, lang=None):
        node = pnode.contents
        if node.type == _SERD_BLANK:
            return ("B", text(node))
        if node.type in (_SERD_URI, _SERD_CURIE):
            return ("U", expand(pnode))
        if node.type == _SERD_LITERAL:
            dt = expand(datatype) if datatype else None
            lg = text(lang.contents) if lang else None
            return ("L", text(node), dt, lg)
        errors.append("unexpected node type %d" % node.type)
        return ("L", "", None, None)

    def on_base(handle, uri):
        return lib.serd_env_set_base_uri(env, uri)

    def on_prefix(handle, name, uri):
        return lib.serd_env_set_prefix(env, name, uri)

    def on_statement(handle, flags, graph, subject, predicate, obj, datatype, lang):
        try:
            triples.append((to_node(subject), to_node(predicate), to_node(obj, datatype, lang)))
        except Exception as e:  # never let an exception unwind through C
            errors.append(str(e))
        return 0

    def on_error(handle, error):
        # The message itself is a printf format plus a va_list, which ctypes can't format.
        errors.append("line %d, column %d" % (error.contents.line, error.contents.col))
        return 0

    sinks = (_BaseSink(on_base), _PrefixSink(on_prefix), _StatementSink(on_statement), _ErrorSink(on_error))
    reader = lib.serd_reader_new(_SERD_TURTLE, None, None, sinks[0], sinks[1], sinks[2], None)
    lib.serd_reader_set_error_sink(reader, sinks[3], None)
    lib.serd_reader_add_blank_prefix(reader, blank_prefix.encode())
    try:
        status = lib.serd_reader_read_file(reader, ctypes.string_at(base.buf, base.n_bytes))
    finally:
        lib.serd_reader_free(reader)
        lib.serd_env_free(env)
        lib.serd_node_free(ctypes.byref(base))
    if status != 0 or errors:
        raise Error("%s: Turtle syntax error%s" % (path, (": " + errors[0]) if errors else ""))
    return triples


class Graph:
    def __init__(self):
        self.by_subject = {}

    def add_file(self, path):
        for s, p, o in parse_turtle(path):
            self.by_subject.setdefault(s, []).append((p, o))

    def objects(self, subject, predicate):
        return [o for p, o in self.by_subject.get(subject, []) if p == ("U", predicate)]

    def value(self, subject, predicate):
        values = self.objects(subject, predicate)
        return values[0] if values else None

    def subjects_with(self, predicate):
        return [s for s, pos in self.by_subject.items() if any(p == ("U", predicate) for p, _ in pos)]


def literal_float(node):
    if node is None or node[0] != "L":
        return None
    try:
        return float(node[1])
    except ValueError:
        return None


# --------------------------------------------------------------------------
# Turtle output (the overlay's modgui.ttl).

def _ttl_literal(node):
    _, text, datatype, lang = node
    escaped = (text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
               .replace("\r", "\\r").replace("\t", "\\t"))
    result = '"%s"' % escaped
    if lang:
        result += "@" + lang
    elif datatype and datatype != XSD + "string":
        result += "^^" + _ttl_iri(datatype)
    return result


def _ttl_iri(uri):
    for c in '<>"{}|^`\\ \n\r\t':
        if c in uri:
            uri = uri.replace(c, urllib.parse.quote(c))
    return "<%s>" % uri


class OverlayWriter:
    """Copies the modgui:gui description of plugins out of a source graph,
    rewriting references to files in the source bundle so that they are relative
    to the overlay bundle."""

    def __init__(self, graph, bundle_uri):
        self.graph = graph
        self.bundle_uri = bundle_uri
        self.lines = []
        self.blank_ids = {}

    def term(self, node):
        if node[0] == "U":
            uri = node[1]
            if uri.startswith(self.bundle_uri):
                return _ttl_iri(uri[len(self.bundle_uri):])
            return _ttl_iri(uri)
        if node[0] == "B":
            return "_:b%d" % self.blank_ids.setdefault(node, len(self.blank_ids) + 1)
        return _ttl_literal(node)

    def add_gui(self, candidate):
        """Adds the candidate's (validated) modgui:gui triples."""
        self.lines.append("%s %s %s ." % (self.term(("U", candidate.uri)), self.term(("U", MODGUI + "gui")),
                                          self.term(candidate.gui)))
        for s, p, o in candidate.overlay_triples():
            self.lines.append("%s %s %s ." % (self.term(s), self.term(p), self.term(o)))
        self.lines.append("")

    def text(self, header):
        return header + "\n".join(self.lines) + "\n"


# --------------------------------------------------------------------------
# Installed plugins (lilv).

class PortInfo:
    def __init__(self, symbol, is_input, kind, minimum=None, maximum=None, index=None, name=None, properties=()):
        self.symbol = symbol
        self.is_input = is_input
        self.kind = kind  # "control", "audio", "atom", "cv", "other"
        self.minimum = minimum
        self.maximum = maximum
        self.index = index
        self.name = name
        self.properties = frozenset(properties)  # of PORT_PROPERTIES


class InstalledPlugin:
    def __init__(self, uri, bundle, name):
        self.uri = uri
        self.bundle = bundle
        self.name = name
        self.ports = {}
        self.parameters = set()
        self.has_working_gui = False
        self.has_broken_gui = False


def _port_kind(world, port):
    for kind, cls in (("control", LV2 + "ControlPort"), ("audio", LV2 + "AudioPort"),
                      ("cv", LV2 + "CVPort"), ("atom", "http://lv2plug.in/ns/ext/atom#AtomPort")):
        if port.is_a(world.new_uri(cls)):
            return kind
    return "other"


def _node_float(node):
    if node is None or not node.is_literal():
        return None
    try:
        return float(str(node))
    except ValueError:
        return None


def _node_path(node):
    try:
        return node.get_path() if node is not None and node.is_uri() else None
    except Exception:
        return None


def scan_installed(lv2_dirs, extra_bundles):
    """Returns {uri: InstalledPlugin} for the plugins in lv2_dirs (and extra_bundles)."""
    try:
        import lilv
    except ImportError:
        raise Error("python3-lilv is required (sudo apt install python3-lilv).")
    os.environ["LV2_PATH"] = ":".join(lv2_dirs)
    world = lilv.World()
    world.load_all()
    for bundle in extra_bundles:
        world.load_bundle(world.new_uri(file_uri(bundle, is_dir=True)))
    n_gui = world.new_uri(MODGUI + "gui")
    n_resources = world.new_uri(MODGUI + "resourcesDirectory")
    n_icon = world.new_uri(MODGUI + "iconTemplate")
    n_input = world.new_uri(LV2 + "InputPort")
    n_writable = world.new_uri(PATCH + "writable")
    n_readable = world.new_uri(PATCH + "readable")
    n_properties = {p: world.new_uri(LV2 + p) for p in PORT_PROPERTIES}

    plugins = {}
    for plugin in world.get_all_plugins():
        uri = str(plugin.get_uri())
        if extra_bundles:
            world.load_resource(plugin.get_uri())
        info = InstalledPlugin(uri, _node_path(plugin.get_bundle_uri()), str(plugin.get_name()))
        for i in range(plugin.get_num_ports()):
            port = plugin.get_port_by_index(i)
            kind = _port_kind(world, port)
            minimum = maximum = None
            if kind == "control":
                _, pmin, pmax = port.get_range()
                minimum, maximum = _node_float(pmin), _node_float(pmax)
            symbol = str(port.get_symbol())
            name = port.get_name()
            properties = [p for p, node in n_properties.items() if port.has_property(node)]
            info.ports[symbol] = PortInfo(symbol, port.is_a(n_input), kind, minimum, maximum,
                                          i, str(name) if name is not None else None, properties)
        for predicate in (n_writable, n_readable):
            for node in world.find_nodes(plugin.get_uri(), predicate, None):
                info.parameters.add(str(node))
        for gui in plugin.get_value(n_gui):
            resources = _node_path(world.get(gui, n_resources, None))
            icon = _node_path(world.get(gui, n_icon, None))
            if resources and os.path.isdir(resources) and icon and os.path.isfile(icon):
                info.has_working_gui = True
            else:
                info.has_broken_gui = True
        plugins[uri] = info
    return plugins


# --------------------------------------------------------------------------
# GUI sources.

def run_git(args, cwd=None):
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0")
    result = subprocess.run(["git"] + args, cwd=cwd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    if result.returncode != 0:
        raise subprocess.CalledProcessError(result.returncode, ["git"] + args, result.stdout, result.stderr)
    return result.stdout.strip()


def _last_line(text):
    lines = (text or "").strip().splitlines()
    return lines[-1] if lines else "unknown error"


def _sparse_escape(name):
    return "".join("\\" + c if c in "*?[]!#\\" else c for c in name)


class Source:
    """A git repository (or a local checkout) with bundles that carry modgui data."""

    def __init__(self, label, repo, commit, bundle_dirs, local_dir=None, priority=0):
        self.label = label
        self.repo = repo
        self.commit = commit
        self.bundle_dirs = bundle_dirs
        self.local_dir = local_dir
        self.dir = local_dir
        self.priority = priority
        self.fetched_bundles = set()
        self.indexed = False
        self.index = None

    def description(self):
        if self.local_dir:
            return "%s (local checkout %s)" % (self.label, self.local_dir)
        return "%s %s" % (self.repo, self.commit)

    def fetch_index(self, work_dir):
        """Make the .ttl files of every bundle available."""
        if self.indexed:
            return
        self.indexed = True
        if self.local_dir:
            if not any(os.path.isdir(os.path.join(self.local_dir, d)) for d in self.bundle_dirs):
                raise Error("%s does not contain %s." % (self.local_dir, " or ".join(self.bundle_dirs)))
            try:
                head = run_git(["rev-parse", "HEAD"], cwd=self.local_dir)
                if head != self.commit:
                    log("Warning: %s is at commit %s, not the tested %s." % (self.local_dir, head, self.commit))
            except (subprocess.CalledProcessError, OSError):
                pass
            return
        if shutil.which("git") is None:
            raise Error("git is required (sudo apt install git).")
        self.dir = os.path.join(work_dir, self.label)
        log("Downloading the %s index (%s at %s)..." % (self.label, self.repo, self.commit[:12]))
        try:
            run_git(["init", "--quiet", self.dir])
            run_git(["remote", "add", "origin", self.repo], cwd=self.dir)
            run_git(["fetch", "--quiet", "--depth", "1", "--filter=blob:none", "origin", self.commit], cwd=self.dir)
            run_git(["sparse-checkout", "set", "--no-cone"] + ["/%s/*/*.ttl" % d for d in self.bundle_dirs],
                    cwd=self.dir)
            run_git(["-c", "advice.detachedHead=false", "checkout", "--quiet", "FETCH_HEAD"], cwd=self.dir)
        except subprocess.CalledProcessError as e:
            raise Error("could not download %s (%s). Check the network connection. Nothing installed."
                        % (self.repo, _last_line(e.stderr)))
        actual = run_git(["rev-parse", "HEAD"], cwd=self.dir)
        if actual != self.commit:
            raise Error("%s: got commit %s, expected %s. Nothing installed." % (self.repo, actual, self.commit))

    def fetch_bundles(self, rel_bundles):
        """Make all files of the given bundles (paths relative to the source) available."""
        if self.local_dir:
            return
        new = sorted(set(rel_bundles) - self.fetched_bundles)
        if not new:
            return
        log("Downloading %d %s bundle(s)..." % (len(new), self.label))
        try:
            run_git(["sparse-checkout", "add"] + ["/%s/" % _sparse_escape(b) for b in new], cwd=self.dir)
        except subprocess.CalledProcessError as e:
            raise Error("could not download files from %s (%s). Nothing installed."
                        % (self.repo, _last_line(e.stderr)))
        self.fetched_bundles.update(new)

    def bundles(self):
        for d in self.bundle_dirs:
            parent = os.path.join(self.dir, d)
            if not os.path.isdir(parent):
                continue
            for name in sorted(os.listdir(parent)):
                path = os.path.join(parent, name)
                if os.path.isdir(path) and not os.path.islink(path):
                    yield os.path.join(d, name), path


class Candidate:
    """A modgui:gui for plugin `uri` in a source bundle."""

    def __init__(self, source, rel_bundle, bundle_path, graph, uri, gui):
        self.source = source
        self.rel_bundle = rel_bundle
        self.bundle_path = bundle_path
        self.bundle_uri = file_uri(bundle_path, is_dir=True)
        self.graph = graph
        self.uri = uri
        self.gui = gui
        self.warnings = []
        self.installed = None       # the InstalledPlugin it is checked against
        self.port_map = {}          # MOD symbol -> (installed symbol, or None if hidden, how)
        self.template_copies = {}   # template -> the overlay-owned adapted copy (relative paths)

    def name(self):
        return "%s:%s" % (self.source.label, self.rel_bundle)

    def label(self):
        node = self.graph.value(self.gui, MODGUI + "label")
        return node[1] if node and node[0] == "L" else None

    def gui_triples(self):
        """The triples that describe the GUI: those of the modgui:gui node and of the
        blank nodes it refers to (modgui:port etc.). Raises Error if the description
        contains anything but MOD GUI data, so that an overlay can never carry plugin
        data (lv2:binary, ports, ...) or refer to files other than the GUI's own."""
        if self.gui[0] != "B":
            raise Error("its modgui:gui is not a blank node")
        result = []
        seen = {self.gui}
        pending = [(self.gui, True)]
        while pending:
            subject, top = pending.pop(0)
            for p, o in self.graph.by_subject.get(subject, []):
                predicate = p[1]
                if top:
                    if not predicate.startswith(MODGUI):
                        raise Error("its modgui:gui has a non-MOD-GUI property <%s>" % predicate)
                elif not (predicate.startswith(MODGUI) or predicate in NESTED_PREDICATES):
                    raise Error("its modgui:gui has a nested property <%s>" % predicate)
                is_file = o[0] == "U" and o[1].startswith("file:")
                if is_file != (top and predicate in FILE_PROPERTIES):
                    if is_file:
                        raise Error("its modgui:gui refers to a file through <%s> (%s)" % (predicate, o[1]))
                    raise Error("its <%s> is not a file in the bundle" % predicate)
                if o[0] == "U" and predicate != RDF_TYPE and not is_file and not top:
                    raise Error("its modgui:gui has a nested reference to <%s>" % o[1])
                if o[0] == "B" and o not in seen:
                    seen.add(o)
                    pending.append((o, False))
                result.append((subject, p, o))
        return result

    def referenced_files(self):
        """(property, relative path) for each file the GUI references. Raises Error for
        a description that gui_triples() rejects, or references outside the bundle."""
        result = []
        for s, p, o in self.gui_triples():
            if s != self.gui or p[1] not in FILE_PROPERTIES:
                continue
            if not o[1].startswith(self.bundle_uri):
                raise Error("references a file outside its bundle (%s)" % o[1])
            rel = urllib.parse.unquote(o[1][len(self.bundle_uri):]).rstrip("/")
            parts = rel.split("/")
            if not rel or rel.startswith("/") or ".." in parts or "." in parts or "" in parts:
                raise Error("bad file reference %s" % o[1])
            result.append((p[1][len(MODGUI):], rel))
        return result

    def hidden(self):
        return sorted(symbol for symbol, (target, _) in self.port_map.items() if target is None)

    def adaptations(self):
        result = []
        for symbol in sorted(self.port_map):
            target, how = self.port_map[symbol]
            if target is None:
                result.append("control '%s' hidden (%s)" % (symbol, how))
            else:
                result.append("port '%s' -> '%s' (%s)" % (symbol, target, how))
        return result

    def overlay_triples(self):
        """gui_triples(), adapted to the installed plugin: modgui:port and
        modgui:monitoredOutputs entries of mapped ports get the installed symbol, those
        of hidden ones are dropped, and templates refer to their adapted copies."""
        triples = self.gui_triples()
        renamed, hidden = {}, set()
        for s, p, o in triples:
            if s == self.gui and p[1] in (MODGUI + "port", MODGUI + "monitoredOutputs"):
                symbol = self.graph.value(o, LV2 + "symbol")
                if symbol and symbol[0] == "L" and symbol[1] in self.port_map:
                    target = self.port_map[symbol[1]][0]
                    if target is None:
                        hidden.add(o)
                    else:
                        renamed[o] = target
        result = []
        for s, p, o in triples:
            if s in hidden or (s == self.gui and o in hidden):
                continue
            if s in renamed and p[1] == LV2 + "symbol":
                o = ("L", renamed[s], o[2], o[3])
            elif s == self.gui and p[1] in (MODGUI + t for t in TEMPLATE_PROPERTIES):
                rel = urllib.parse.unquote(o[1][len(self.bundle_uri):])
                if rel in self.template_copies:
                    o = ("U", self.bundle_uri + urllib.parse.quote(self.template_copies[rel]))
            result.append((s, p, o))
        return result

    def mod_port(self, symbol):
        """The source plugin's lv2:port with the given symbol, if the source describes it."""
        for port in self.graph.objects(("U", self.uri), LV2 + "port"):
            if self.graph.value(port, LV2 + "symbol") == ("L", symbol, None, None):
                return port
        return None


def index_source(source):
    """{plugin uri: [Candidate]} for every modgui:gui in the source's .ttl files."""
    if source.index is None:
        source.index = _index_source(source)
    return source.index


def _index_source(source):
    result = {}
    for rel_bundle, path in source.bundles():
        graph = Graph()
        for name in sorted(os.listdir(path)):
            if not name.endswith(".ttl"):
                continue
            file_path = os.path.join(path, name)
            if os.path.islink(file_path) or not os.path.isfile(file_path):
                continue
            try:
                graph.add_file(file_path)
            except Error as e:
                # Some presets files in mod-lv2-data don't parse; they don't matter here.
                log("Note: ignoring %s/%s (%s)" % (rel_bundle, name, str(e).split(": ", 1)[-1]))
        for subject in graph.subjects_with(MODGUI + "gui"):
            if subject[0] != "U":
                continue
            for gui in graph.objects(subject, MODGUI + "gui"):
                result.setdefault(subject[1], []).append(
                    Candidate(source, rel_bundle, path, graph, subject[1], gui))
    return result


# --------------------------------------------------------------------------
# Compatibility checks.

class _TemplateScanner(html.parser.HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.ports = []        # (role, symbol)
        self.parameters = []   # uri

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        symbol = attrs.get("mod-port-symbol")
        if symbol is not None:
            self.ports.append((attrs.get("mod-role", ""), symbol))
        uri = attrs.get("mod-parameter-uri")
        if uri is not None:
            self.parameters.append(uri)

    handle_startendtag = handle_starttag


def _check_port(installed, symbol, role, where):
    port = installed.ports.get(symbol)
    if port is None:
        return "%s refers to port '%s', which the installed plugin does not have" % (where, symbol)
    expected = {
        "input-control-port": ("control", True), "output-control-port": ("control", False),
        "input-audio-port": ("audio", True), "output-audio-port": ("audio", False),
        "input-cv-port": ("cv", True), "output-cv-port": ("cv", False),
        "input-midi-port": ("atom", True), "output-midi-port": ("atom", False),
    }.get(role)
    if expected and (port.kind, port.is_input) != expected:
        return "%s uses port '%s' as %s, but it is not one in the installed plugin" % (where, symbol, role)
    return None


def _check_range(candidate, installed, mod_symbol, symbol):
    """Notes a difference between the range of port mod_symbol in MOD's build and
    that of port symbol (the same port, perhaps renamed) in the installed plugin."""
    mod_port = candidate.mod_port(mod_symbol)
    port = installed.ports.get(symbol)
    if mod_port is None or port is None:
        return
    mod_min = literal_float(candidate.graph.value(mod_port, LV2 + "minimum"))
    mod_max = literal_float(candidate.graph.value(mod_port, LV2 + "maximum"))
    if None in (mod_min, mod_max, port.minimum, port.maximum):
        return
    span = max(abs(mod_max - mod_min), 1e-9)
    if abs(mod_min - port.minimum) > span * 1e-3 or abs(mod_max - port.maximum) > span * 1e-3:
        candidate.warnings.append("port '%s' range is %g..%g here, %g..%g in MOD's build (the GUI uses the installed range)"
                                  % (symbol, port.minimum, port.maximum, mod_min, mod_max))


# --------------------------------------------------------------------------
# Port adaptation.
#
# MOD's description of a plugin sometimes uses another symbol for a port than the
# installed build does: a typo that was fixed since, or a port that was renamed
# between versions. A GUI that refers to a port the installed plugin lacks is
# adapted rather than skipped: the reference is mapped to the installed port when
# the mapping is known (mod-gui-port-aliases.txt) or unambiguous, and the control
# is hidden otherwise. Only the overlay's copies are changed: its modgui.ttl, and
# overlay-owned copies of the templates.

PORT_ALIASES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mod-gui-port-aliases.txt")
ADAPTED_DIR = "pipedal-adapted"  # in the overlay, for adapted copies of templates
_port_aliases = {"path": PORT_ALIASES, "table": None}

# Template roles that refer to audio, CV or MIDI ports. Those are never adapted.
_NON_CONTROL_ROLE = re.compile(r"^(input|output)-(audio|cv|midi)-port$")


def read_port_aliases(path):
    """{(plugin uri, MOD symbol): (installed symbol, or None to hide the control, reason)}.
    Lines are "<plugin URI> <MOD symbol> <installed symbol or -> [# reason]"."""
    result = {}
    if not os.path.isfile(path):
        return result
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            # Plugin URIs may contain '#': a comment starts with a field that does.
            fields = line.split(None, 3)
            if len(fields) < 3 or (len(fields) == 4 and not fields[3].startswith("#")):
                raise Error("%s:%d: expected \"<plugin URI> <MOD symbol> <installed symbol or -> [# reason]\""
                            % (path, number))
            reason = fields[3].lstrip("#").strip() if len(fields) == 4 else ""
            result[(fields[0], fields[1])] = (None if fields[2] == "-" else fields[2], reason)
    return result


def port_aliases():
    if _port_aliases["table"] is None:
        _port_aliases["table"] = read_port_aliases(_port_aliases["path"])
    return _port_aliases["table"]


def _edit_distance(a, b):
    previous = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        current = [i]
        for j, cb in enumerate(b, 1):
            current.append(min(previous[j] + 1, current[j - 1] + 1, previous[j - 1] + (ca != cb)))
        previous = current
    return previous[-1]


def _near_symbol(a, b):
    """Nearly the same symbol: a typo or two (one for short symbols)."""
    return a != b and _edit_distance(a, b) <= (1 if min(len(a), len(b)) < 8 else 2)


def _similar_names(a, b):
    a, b = (re.sub(r"[^a-z0-9]", "", (x or "").lower()) for x in (a, b))
    if not a or not b:
        return False
    short, long = sorted((a, b), key=len)
    return a == b or _near_symbol(a, b) or (len(short) >= 4 and short in long)


def _same_range(a, b):
    if None in (a.minimum, a.maximum, b.minimum, b.maximum) or a.properties != b.properties:
        return False
    span = max(abs(a.maximum - a.minimum), 1e-9)
    return abs(a.minimum - b.minimum) <= span * 1e-3 and abs(a.maximum - b.maximum) <= span * 1e-3


def _mod_port_info(candidate, symbol):
    """The PortInfo of a port of MOD's build of the plugin, if its description has it."""
    node = candidate.mod_port(symbol)
    if node is None:
        return None
    graph = candidate.graph
    types = {o[1] for o in graph.objects(node, RDF_TYPE) if o[0] == "U"}
    kind = "other"
    for k, cls in (("control", LV2 + "ControlPort"), ("audio", LV2 + "AudioPort"),
                   ("cv", LV2 + "CVPort"), ("atom", "http://lv2plug.in/ns/ext/atom#AtomPort")):
        if cls in types:
            kind = k
    index = literal_float(graph.value(node, LV2 + "index"))
    name = graph.value(node, LV2 + "name")
    properties = [p for p in PORT_PROPERTIES if ("U", LV2 + p) in graph.objects(node, LV2 + "portProperty")]
    return PortInfo(symbol, LV2 + "InputPort" in types, kind,
                    literal_float(graph.value(node, LV2 + "minimum")), literal_float(graph.value(node, LV2 + "maximum")),
                    None if index is None else int(index), name[1] if name and name[0] == "L" else None, properties)


def _mod_symbols(candidate):
    symbols = set()
    for port in candidate.graph.objects(("U", candidate.uri), LV2 + "port"):
        symbol = candidate.graph.value(port, LV2 + "symbol")
        if symbol and symbol[0] == "L":
            symbols.add(symbol[1])
    return symbols


def _auto_match(candidate, symbol, is_input):
    """The installed control port that port `symbol` of the GUI unambiguously is, or
    None. Returns (installed symbol or None, how)."""
    installed = candidate.installed
    mod = _mod_port_info(candidate, symbol)
    if mod is not None and (mod.kind != "control" or mod.is_input != is_input):
        return None, "not a control port of the same direction in MOD's build"
    pool = [p for p in installed.ports.values() if p.kind == "control" and p.is_input == is_input]
    if mod is not None:
        # A port that MOD's build has under the same symbol is a different port.
        mod_symbols = _mod_symbols(candidate)
        pool = [p for p in pool if p.symbol not in mod_symbols]
    matches = {}
    if mod is not None and mod.index is not None:
        for p in pool:
            if p.index == mod.index and (_similar_names(mod.name, p.name) or _same_range(mod, p)):
                matches.setdefault(p.symbol, "same index and %s" % ("name" if _similar_names(mod.name, p.name)
                                                                    else "range"))
    for p in pool:
        if _near_symbol(symbol, p.symbol):
            matches.setdefault(p.symbol, "nearly the same symbol")
    if len(matches) == 1:
        target, how = matches.popitem()
        return target, "automatic: %s" % how
    if matches:
        return None, "ambiguous: %s" % ", ".join(sorted(matches))
    return None, "no matching port in the installed plugin"


def resolve_port(candidate, symbol, is_input):
    """For a port the GUI refers to that the installed plugin lacks: the installed
    control port to use instead, or None if the control is to be hidden. Raises Error
    if mod-gui-port-aliases.txt maps it to a port that doesn't fit."""
    if symbol in candidate.port_map:
        return candidate.port_map[symbol][0]
    alias = port_aliases().get((candidate.uri, symbol))
    if alias is not None:
        target, reason = alias
        if target is not None:
            port = candidate.installed.ports.get(target)
            if port is None or port.kind != "control" or port.is_input != is_input:
                raise Error("mod-gui-port-aliases.txt maps port '%s' to '%s', which is not an %s control port of "
                            "the installed plugin" % (symbol, target, "input" if is_input else "output"))
        how = "alias" + (": " + reason if reason else "")
    else:
        target, how = _auto_match(candidate, symbol, is_input)
    candidate.port_map[symbol] = (target, how)
    return target


def check_adaptation(candidate, controls):
    """Checks that the adaptations the GUI needs can be made. controls: the symbols
    of every control the GUI has. Returns a skip reason, or None."""
    hidden = candidate.hidden()
    if hidden and 2 * len(hidden) > len(controls):
        return "most of its controls refer to ports the installed plugin does not have (%s)" % ", ".join(hidden)
    # Scripts and stylesheets aren't adapted: they must not refer to the ports.
    for prop, rel in candidate.referenced_files():
        if prop not in ("javascript", "stylesheet"):
            continue
        with open(os.path.join(candidate.bundle_path, rel), encoding="utf-8", errors="replace") as f:
            text = f.read()
        for symbol in candidate.port_map:
            if prop == "javascript":
                pattern = r"""['"`]%s['"`]""" % re.escape(symbol)
            else:
                pattern = r"""mod-port-symbol\s*[~|^$*]?=\s*['"]?%s\b""" % re.escape(symbol)
            if re.search(pattern, text):
                return "its %s refers to port '%s', which the installed plugin does not have" % (rel, symbol)
    return None


_VOID_ELEMENTS = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param",
                  "source", "track", "wbr"}


class _TemplateRewriter(html.parser.HTMLParser):
    """Rewrites the mod-port-symbol attributes of a template for a port map: mapped
    ports get the installed symbol; elements of hidden ones are removed."""

    def __init__(self, text, port_map):
        super().__init__(convert_charrefs=False)
        self.text = text
        self.port_map = port_map
        self.line_starts = [0]
        for line in text.splitlines(keepends=True):
            self.line_starts.append(self.line_starts[-1] + len(line))
        self.open = []    # (tag, start offset, hidden)
        self.edits = []   # (start, end, replacement)

    def _offset(self):
        line, column = self.getpos()
        return self.line_starts[line - 1] + column

    def _start(self, tag, attrs, has_end):
        start = self._offset()
        raw = self.get_starttag_text()
        symbol = dict(attrs).get("mod-port-symbol")
        target = self.port_map.get(symbol, (symbol, None))[0] if symbol is not None else None
        hidden = symbol in self.port_map and target is None
        if symbol in self.port_map and target is not None:
            new = re.sub(r"""(\bmod-port-symbol\s*=\s*)(["']?)%s\2(?=[\s/>])""" % re.escape(symbol),
                         lambda m: m.group(1) + m.group(2) + target + m.group(2), raw, count=1)
            if new == raw:
                raise Error("cannot adapt %r" % raw)
            self.edits.append((start, start + len(raw), new))
        if has_end and tag not in _VOID_ELEMENTS:
            self.open.append((tag, start, hidden))
        elif hidden:
            self.edits.append((start, start + len(raw), ""))

    def handle_starttag(self, tag, attrs):
        self._start(tag, attrs, True)

    def handle_startendtag(self, tag, attrs):
        self._start(tag, attrs, False)

    def handle_endtag(self, tag):
        start = self._offset()
        end = self.text.index(">", start) + 1
        for i in range(len(self.open) - 1, -1, -1):
            if self.open[i][0] == tag:
                break
        else:
            return  # a stray end tag
        unclosed = self.open[i + 1:]
        _, element_start, hidden = self.open[i]
        del self.open[i:]
        if any(h for _, _, h in unclosed):
            raise Error("cannot hide an element that has no end tag")
        if hidden:
            self.edits.append((element_start, end, ""))

    def rewrite(self):
        self.feed(self.text)
        self.close()
        if any(h for _, _, h in self.open):
            raise Error("cannot hide an element that has no end tag")
        # Edits inside a removed element go with it.
        removed = [(s, e) for s, e, r in self.edits if r == ""]
        edits = [(s, e, r) for s, e, r in self.edits
                 if not any(rs <= s and e <= rend and (rs, rend, "") != (s, e, r) for rs, rend in removed)]
        text = self.text
        for s, e, r in sorted(edits, reverse=True):
            text = text[:s] + r + text[e:]
        return text


def adapt_template(text, port_map):
    """The template text, adapted for port_map. Raises Error if it can't be."""
    if not port_map:
        return text
    return _TemplateRewriter(text, port_map).rewrite()


def check_description(candidate, installed):
    """Checks that only need the .ttl files. Returns a skip reason, or None."""
    candidate.installed = installed
    try:
        files = candidate.referenced_files()
    except Error as e:
        return str(e)
    props = {p for p, _ in files}
    for required in ("resourcesDirectory", "iconTemplate"):
        if required not in props:
            return "the MOD GUI has no modgui:%s" % required
    used = {}
    for port in candidate.graph.objects(candidate.gui, MODGUI + "port"):
        symbol = candidate.graph.value(port, LV2 + "symbol")
        if symbol is None or symbol[0] != "L":
            return "a modgui:port has no lv2:symbol"
        target = symbol[1]
        if target not in installed.ports:
            try:
                target = resolve_port(candidate, symbol[1], True)
            except Error as e:
                return str(e)
            if target is None:
                continue
        problem = _check_port(installed, target, "input-control-port", "modgui:port")
        if problem:
            return problem
        # (Some MOD GUIs list a port twice; that is harmless.)
        if used.get(target, symbol[1]) != symbol[1]:
            return "modgui:port '%s' and '%s' would both be port '%s'" % (used[target], symbol[1], target)
        used[target] = symbol[1]
        _check_range(candidate, installed, symbol[1], target)
    for output in candidate.graph.objects(candidate.gui, MODGUI + "monitoredOutputs"):
        symbol = candidate.graph.value(output, LV2 + "symbol")
        if symbol and symbol[0] == "L" and symbol[1] not in installed.ports:
            try:
                resolve_port(candidate, symbol[1], False)
            except Error as e:
                return str(e)
    return None


# Files are only ever copied from inside the GUI's own bundle. Symbolic links are
# never followed: a GUI whose files include one is skipped, even if it points
# inside the bundle or the repository. (mod-lv2-data has symbolic links only in
# its *-bad.lv2 bundles, which link to files of the bundle they duplicate; the
# GUI is installed from that bundle instead.)

def _bundle_root(candidate):
    root = os.path.realpath(candidate.bundle_path)
    source_root = os.path.realpath(candidate.source.dir)
    if not root.startswith(source_root + os.sep):
        raise Error("the MOD bundle is outside the source checkout")
    return root


def _inside(path, root):
    return os.path.realpath(path).startswith(root + os.sep)


def _source_path(candidate, rel):
    """The path of rel in the candidate's bundle. Raises Error if any part of it is a
    symbolic link, or it is not inside the bundle."""
    root = _bundle_root(candidate)
    path = root
    for part in rel.split("/"):
        path = os.path.join(path, part)
        try:
            mode = os.lstat(path).st_mode
        except FileNotFoundError:
            raise Error("the MOD bundle is missing %s" % rel)
        if stat.S_ISLNK(mode):
            raise Error("the MOD bundle contains a symbolic link (%s)" % os.path.relpath(path, root))
    if not _inside(path, root):
        raise Error("%s is not inside the MOD bundle" % rel)
    return path, root


def _tree_entries(path, root):
    """(source path, relative path, is_dir) for path and everything under it. Raises
    Error for symbolic links, special files, and anything outside root."""
    mode = os.lstat(path).st_mode
    if stat.S_ISREG(mode):
        return [(path, "", False)]
    if not stat.S_ISDIR(mode):
        raise Error("the MOD bundle contains a special file (%s)" % os.path.relpath(path, root))
    entries = [(path, "", True)]
    for dir_path, dirs, files in os.walk(path, followlinks=False):
        for name in sorted(dirs) + sorted(files):
            full = os.path.join(dir_path, name)
            mode = os.lstat(full).st_mode
            if stat.S_ISLNK(mode):
                raise Error("the MOD bundle contains a symbolic link (%s)" % os.path.relpath(full, root))
            if not (stat.S_ISDIR(mode) or stat.S_ISREG(mode)):
                raise Error("the MOD bundle contains a special file (%s)" % os.path.relpath(full, root))
            if not _inside(full, root):
                raise Error("%s is not inside the MOD bundle" % os.path.relpath(full, root))
            entries.append((full, os.path.relpath(full, path), stat.S_ISDIR(mode)))
    return entries


def _copy_checked(candidate, rel, dst):
    """Copies rel from the candidate's bundle to dst, re-checking every file as it goes."""
    src, root = _source_path(candidate, rel)
    for full, sub, is_dir in _tree_entries(src, root):
        target = os.path.join(dst, sub) if sub else dst
        if is_dir:
            os.makedirs(target, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(target), exist_ok=True)
        fd = os.open(full, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(fd, "rb") as fin:
            if not stat.S_ISREG(os.fstat(fin.fileno()).st_mode):
                raise Error("the MOD bundle contains a special file (%s)" % os.path.relpath(full, root))
            with open(target, "wb") as fout:
                shutil.copyfileobj(fin, fout)


def _scan_template(path):
    scanner = _TemplateScanner()
    with open(path, encoding="utf-8", errors="replace") as f:
        scanner.feed(f.read())
    scanner.close()
    return scanner


def _read_checked(candidate, rel):
    """The text of file rel of the candidate's bundle (checked as _copy_checked does)."""
    path, root = _source_path(candidate, rel)
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, "r", encoding="utf-8", errors="surrogateescape") as f:
        if not stat.S_ISREG(os.fstat(f.fileno()).st_mode):
            raise Error("the MOD bundle contains a special file (%s)" % rel)
        return f.read()


def check_files(candidate, installed):
    """Checks that need the GUI's resource files. Returns a skip reason, or None."""
    candidate.installed = installed
    try:
        for prop, rel in candidate.referenced_files():
            full, root = _source_path(candidate, rel)
            is_dir = os.path.isdir(full)
            if prop == "resourcesDirectory" and not is_dir:
                return "the MOD bundle has no %s/ folder" % rel
            if prop != "resourcesDirectory" and is_dir:
                return "the MOD bundle's %s is a folder" % rel
            _tree_entries(full, root)
    except Error as e:
        return str(e)
    controls = set()
    for port in candidate.graph.objects(candidate.gui, MODGUI + "port"):
        symbol = candidate.graph.value(port, LV2 + "symbol")
        if symbol and symbol[0] == "L":
            controls.add(symbol[1])
    templates = [rel for prop, rel in candidate.referenced_files() if prop in TEMPLATE_PROPERTIES]
    for rel in templates:
        try:
            scanner = _scan_template(os.path.join(candidate.bundle_path, rel))
        except Exception as e:
            return "cannot parse %s (%s)" % (rel, e)
        where = os.path.basename(rel)
        for role, symbol in scanner.ports:
            if "{{" in symbol or symbol.startswith(":"):
                continue  # template-generated, or :bypass/:presets
            adaptable = not _NON_CONTROL_ROLE.match(role)
            if adaptable:
                controls.add(symbol)
            target = symbol
            if symbol not in installed.ports and adaptable:
                try:
                    target = resolve_port(candidate, symbol, not role.startswith("output-"))
                except Error as e:
                    return str(e)
                if target is None:
                    continue
            problem = _check_port(installed, target, role, where)
            if problem:
                return problem
            if role == "input-control-port":
                _check_range(candidate, installed, symbol, target)
        for uri in scanner.parameters:
            if "{{" not in uri and uri not in installed.parameters:
                return "%s refers to parameter <%s>, which the installed plugin does not have" % (where, uri)
    for rel in templates if candidate.port_map else []:
        try:
            with open(os.path.join(candidate.bundle_path, rel), encoding="utf-8", errors="surrogateescape") as f:
                adapt_template(f.read(), candidate.port_map)
        except Error as e:
            return "cannot adapt %s (%s)" % (rel, e)
    return check_adaptation(candidate, controls)


def verify_overlay(graph, candidates):
    """Runs the compatibility check again on the GUIs as written to an overlay (i.e.
    after adaptation). Raises Error if one fails it."""
    for candidate in candidates:
        installed = candidate.installed
        for gui in graph.objects(("U", candidate.uri), MODGUI + "gui"):
            problems = []
            for port in graph.objects(gui, MODGUI + "port"):
                symbol = graph.value(port, LV2 + "symbol")
                problems.append(_check_port(installed, symbol[1], "input-control-port", "modgui:port"))
            for output in graph.objects(gui, MODGUI + "monitoredOutputs"):
                symbol = graph.value(output, LV2 + "symbol")
                if symbol and symbol[0] == "L" and symbol[1] not in installed.ports:
                    problems.append("modgui:monitoredOutputs refers to port '%s'" % symbol[1])
            for prop in TEMPLATE_PROPERTIES:
                node = graph.value(gui, MODGUI + prop)
                if node is None:
                    continue
                path = uri_to_path(node[1])
                scanner = _scan_template(path)
                for role, symbol in scanner.ports:
                    if "{{" not in symbol and not symbol.startswith(":"):
                        problems.append(_check_port(installed, symbol, role, os.path.basename(path)))
                for uri in scanner.parameters:
                    if "{{" not in uri and uri not in installed.parameters:
                        problems.append("%s refers to parameter <%s>" % (os.path.basename(path), uri))
            problems = [p for p in problems if p]
            if problems:
                raise Error("the adapted MOD GUI fails the compatibility check: %s" % problems[0])


# --------------------------------------------------------------------------
# Overlays.

def is_our_overlay(path):
    return os.path.isdir(path) and not os.path.islink(path) and any(
        os.path.isfile(os.path.join(path, m)) for m in [MARKER] + LEGACY_MARKERS)


def overlay_name(source, rel_bundle):
    stem = os.path.basename(rel_bundle)
    if stem.endswith(".lv2"):
        stem = stem[:-4]
    if source.label == "guitarix":
        stem += "-gx"
    return stem + "-modgui.lv2"


def build_overlay(staging, candidates):
    """Writes an overlay bundle for candidates (all from the same source bundle)."""
    first = candidates[0]
    os.makedirs(staging)
    # Adapted templates are copied to files of their own: the originals are in the
    # resources folder, which other GUIs of the bundle may share.
    adapted = {}
    for candidate in candidates:
        candidate.template_copies = {}
        if not candidate.port_map:
            continue
        for prop, rel in candidate.referenced_files():
            if prop not in TEMPLATE_PROPERTIES:
                continue
            text = _read_checked(candidate, rel)
            new_text = adapt_template(text, candidate.port_map)
            if new_text != text:
                new_rel = "%s/%s/%s" % (ADAPTED_DIR, re.sub(r"[^A-Za-z0-9_-]+", "_", candidate.uri),
                                        os.path.basename(rel))
                candidate.template_copies[rel] = new_rel
                adapted[new_rel] = new_text
    writer = OverlayWriter(first.graph, first.bundle_uri)
    to_copy = set()
    for candidate in sorted(candidates, key=lambda c: c.uri):
        writer.add_gui(candidate)
        for prop, rel in candidate.referenced_files():
            to_copy.add(rel)
    header = ("# Generated by PiPedal tools/install-mod-guis.sh. Do not edit.\n"
              "# The MOD GUI for the plugins below, from %s\n"
              "# (%s). The resource files are from the same bundle.\n\n"
              % (first.source.description(), first.rel_bundle))
    with open(os.path.join(staging, OVERLAY_TTL), "w", encoding="utf-8") as f:
        f.write(writer.text(header))
    with open(os.path.join(staging, "manifest.ttl"), "w", encoding="utf-8") as f:
        f.write("# Generated by PiPedal tools/install-mod-guis.sh. Do not edit.\n"
                "# Adds a MOD GUI to installed plugins. Declares no plugins of its own.\n\n"
                "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n\n")
        for candidate in sorted(candidates, key=lambda c: c.uri):
            f.write("%s rdfs:seeAlso <%s> .\n" % (_ttl_iri(candidate.uri), OVERLAY_TTL))
    # Copy directories first, so files inside them aren't copied twice.
    for rel in sorted(to_copy, key=lambda r: (not os.path.isdir(os.path.join(first.bundle_path, r)), r)):
        dst = os.path.join(staging, rel)
        if os.path.exists(dst):
            continue
        _copy_checked(first, rel, dst)
    for rel, text in adapted.items():
        dst = os.path.join(staging, rel)
        if os.path.lexists(dst):
            raise Error("internal error: %s exists" % rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "w", encoding="utf-8", errors="surrogateescape") as f:
            f.write(text)
    with open(os.path.join(staging, MARKER), "w", encoding="utf-8") as f:
        f.write("source=%s\n" % first.source.description())
        f.write("source-bundle=%s\n" % first.rel_bundle)
        for candidate in sorted(candidates, key=lambda c: c.uri):
            f.write("plugin=%s\n" % candidate.uri)
    for root, dirs, files in os.walk(staging):
        os.chmod(root, 0o755)
        for name in files:
            os.chmod(os.path.join(root, name), 0o644)
    # The generated data must parse, and must still reference the files it needs.
    graph = Graph()
    graph.add_file(os.path.join(staging, "manifest.ttl"))
    graph.add_file(os.path.join(staging, OVERLAY_TTL))
    plugin_subjects = {("U", c.uri) for c in candidates}
    for subject in graph.by_subject:
        if subject[0] != "B" and subject not in plugin_subjects:
            raise Error("internal error: overlay describes <%s>" % subject[1])
    for candidate in candidates:
        for p, o in graph.by_subject.get(("U", candidate.uri), []):
            if p[1] not in (MODGUI + "gui", RDFS_SEEALSO):
                raise Error("internal error: overlay describes <%s> of the plugin" % p[1])
    staging_uri = file_uri(staging, is_dir=True)
    for candidate in candidates:
        for gui in graph.objects(("U", candidate.uri), MODGUI + "gui"):
            for p, o in graph.by_subject.get(gui, []):
                if o[0] == "U" and o[1].startswith("file:"):
                    if not o[1].startswith(staging_uri) or not os.path.exists(uri_to_path(o[1])):
                        raise Error("internal error: overlay reference %s does not resolve" % o[1])
    verify_overlay(graph, candidates)


def same_tree(a, b):
    cmp = filecmp.dircmp(a, b)
    if cmp.left_only or cmp.right_only or cmp.funny_files:
        return False
    _, mismatch, errors = filecmp.cmpfiles(a, b, cmp.common_files, shallow=False)
    if mismatch or errors:
        return False
    return all(same_tree(os.path.join(a, d), os.path.join(b, d)) for d in cmp.common_dirs)


# Work area in the destination, on the same file system, for swap_in(). It only
# exists while the script runs: lilv reports an error for every directory in
# LV2_PATH without a manifest.ttl, and warns about every file.
TMP_DIR = ".pipedal-mod-gui-tmp"
# Where older versions of this script kept their lock file (removed when found).
LEGACY_LOCK_FILE = ".pipedal-mod-gui.lock"
LOCK_DIRS = ["/run/lock", "/var/lock"]


def lock_path():
    """The lock file, outside the destination (in which lilv would warn about it).
    One for root, and one for each other user, so that no user can lock out another."""
    uid = os.geteuid()
    name = "pipedal-mod-gui.lock" if uid == 0 else "pipedal-mod-gui-%d.lock" % uid
    for d in LOCK_DIRS:
        if os.path.isdir(d) and os.access(d, os.W_OK | os.X_OK):
            return os.path.join(d, name)
    return os.path.join(tempfile.gettempdir(), name)


class DestLock:
    """Serialises runs of this script."""

    def __init__(self, dest):
        self.dest = dest
        self.fd = None

    def __enter__(self):
        import fcntl
        os.makedirs(self.dest, exist_ok=True)
        path = lock_path()
        try:
            self.fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o644)
        except OSError as e:
            raise Error("cannot open the lock file %s (%s)." % (path, e.strerror))
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            log("Waiting for another run of install-mod-guis.sh to finish...")
            fcntl.flock(self.fd, fcntl.LOCK_EX)
        # Left behind by an interrupted run, or by an older version of this script.
        shutil.rmtree(os.path.join(self.dest, TMP_DIR), ignore_errors=True)
        legacy = os.path.join(self.dest, LEGACY_LOCK_FILE)
        try:
            if not stat.S_ISDIR(os.lstat(legacy).st_mode):
                os.unlink(legacy)
        except FileNotFoundError:
            pass
        return self

    def __exit__(self, *exc):
        shutil.rmtree(os.path.join(self.dest, TMP_DIR), ignore_errors=True)
        os.close(self.fd)


def swap_in(staging, target):
    """Replaces target with a copy of staging. The copy is made in the destination's
    work area, then renamed into place; the old version is renamed out of the way
    first, so there is a brief moment with no overlay (not an atomic swap). Call
    with the DestLock held."""
    parent = os.path.dirname(target)
    tmp_dir = os.path.join(parent, TMP_DIR)
    os.makedirs(tmp_dir, exist_ok=True)
    tmp_new = os.path.join(tmp_dir, os.path.basename(target) + ".new")
    tmp_old = os.path.join(tmp_dir, os.path.basename(target) + ".old")
    shutil.rmtree(tmp_new, ignore_errors=True)
    shutil.rmtree(tmp_old, ignore_errors=True)
    shutil.copytree(staging, tmp_new, symlinks=True)
    if os.path.lexists(target):
        os.rename(target, tmp_old)
        os.rename(tmp_new, target)
        shutil.rmtree(tmp_old)
    else:
        os.rename(tmp_new, target)


# --------------------------------------------------------------------------
# Plugin packages (--install-plugins).

PACKAGE_MAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mod-gui-packages.txt")

# Packages are only installed from the distribution's own archive: no PPAs or
# third-party repositories.
OFFICIAL_ARCHIVE_HOSTS = (
    "archive.ubuntu.com", "ports.ubuntu.com", "security.ubuntu.com",
    "deb.debian.org", "security.debian.org", "ftp.debian.org",
    "archive.raspberrypi.com", "archive.raspberrypi.org",
    "raspbian.raspberrypi.com", "raspbian.raspberrypi.org",
)


def _is_official(url):
    host = urllib.parse.urlparse(url).hostname or ""
    return any(host == h or host.endswith("." + h) for h in OFFICIAL_ARCHIVE_HOSTS)


def apt_info(package):
    """(installed version or None, candidate version or None, candidate is from an official archive)."""
    installed, candidate, sources = _apt_policy(package)
    official = bool(candidate) and any(_is_official(url) for url in sources.get(candidate, []))
    return installed, candidate, official


def apt_version_is_official(package, version):
    _, _, sources = _apt_policy(package)
    return any(_is_official(url) for url in sources.get(version, []))


def _apt_policy(package):
    result = subprocess.run(["apt-cache", "policy", package], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True, env=dict(os.environ, LC_ALL="C"))
    installed = candidate = None
    sources = {}
    version = None
    in_table = False
    for line in result.stdout.splitlines():
        stripped = line.strip()
        if stripped.startswith("Installed:"):
            installed = stripped.split(None, 1)[1]
        elif stripped.startswith("Candidate:"):
            candidate = stripped.split(None, 1)[1]
        elif stripped.startswith("Version table:"):
            in_table = True
        elif in_table:
            fields = stripped.replace("*** ", "").split()
            if len(fields) == 2 and not line.startswith("        "):
                version = fields[0]
            elif len(fields) >= 2 and version:
                sources.setdefault(version, []).append(fields[1])
    installed = None if installed in (None, "(none)") else installed
    candidate = None if candidate in (None, "(none)") else candidate
    return installed, candidate, sources


def read_package_map():
    """{package: set(plugin uri)} from mod-gui-packages.txt."""
    result = {}
    if not os.path.isfile(PACKAGE_MAP):
        return result
    with open(PACKAGE_MAP, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split()
            if len(fields) != 2 or not valid_package_name(fields[0]):
                raise Error("%s:%d: expected \"<package> <plugin URI>\"" % (PACKAGE_MAP, number))
            result.setdefault(fields[0], set()).add(fields[1])
    return result


_PACKAGE_NAME = re.compile(r"^[a-z0-9][a-z0-9+.-]+$")


def valid_package_name(name):
    return bool(_PACKAGE_NAME.match(name))


def check_apt_simulation(packages):
    """Simulates installing packages; raises Error if anything (including dependencies)
    would come from outside the official distribution archive. Returns the number of
    packages that would be installed."""
    result = subprocess.run(["apt-get", "-s", "install", "--no-install-recommends"] + packages,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            env=dict(os.environ, LC_ALL="C"))
    if result.returncode != 0:
        raise Error("apt-get can't install %s: %s" % (" ".join(packages), _last_line(result.stderr)))
    count = 0
    for line in result.stdout.splitlines():
        m = re.match(r"^Inst (\S+) (?:\[\S+\] )?\((\S+) ", line)
        if not m:
            continue
        package, version = m.group(1).split(":")[0], m.group(2)
        if not valid_package_name(package) or not apt_version_is_official(package, version):
            raise Error("installing %s would install %s %s, which is not from the official distribution "
                        "archive. Nothing installed." % (" ".join(packages), package, version))
        count += 1
    return count


def apt_file_packages(index, installed_uris):
    """{package: set(plugin uri)} for MOD bundles whose bundle folder a package ships,
    according to apt-file. Bundle folder names are usually, but not always, the same."""
    by_bundle = {}
    for uri, candidates in index.items():
        if uri in installed_uris:
            continue
        for c in candidates:
            by_bundle.setdefault(os.path.basename(c.rel_bundle), set()).add(uri)
    if not by_bundle:
        return {}
    regexp = "^/usr/lib/(.*/)?lv2/(%s)/manifest\\.ttl$" % "|".join(re.escape(b) for b in sorted(by_bundle))
    result = subprocess.run(["apt-file", "search", "--regexp", regexp], stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, env=dict(os.environ, LC_ALL="C"))
    packages = {}
    for line in result.stdout.splitlines():
        package, _, path = line.partition(": ")
        bundle = os.path.basename(os.path.dirname(path.strip()))
        if bundle in by_bundle:
            packages.setdefault(package.strip(), set()).update(by_bundle[bundle])
    return packages


def install_plugins(args, mod_source, work_dir):
    """Installs the distro packages of plugins that MOD has GUIs for. Returns the packages."""
    if shutil.which("apt-get") is None or shutil.which("apt-cache") is None:
        raise Error("--install-plugins needs apt (Debian, Ubuntu or Raspberry Pi OS).")
    wanted = None if args.install_plugins == "all" else set(args.install_plugins.replace(",", " ").split())
    for name in wanted or []:
        if not valid_package_name(name):
            raise Error("%r is not a package name." % name)
    installed, _ = scan(args, os.path.abspath(args.dest))
    packages = read_package_map()
    if shutil.which("apt-file"):
        mod_source.fetch_index(work_dir)
        log("Looking up the packages of MOD's plugin bundles with apt-file...")
        for package, uris in apt_file_packages(index_source(mod_source), set(installed)).items():
            if valid_package_name(package):
                packages.setdefault(package, set()).update(uris)
    if wanted:
        unknown = wanted - set(packages)
        if unknown:
            raise Error("no MOD GUIs are known for the plugins in: %s" % " ".join(sorted(unknown)))
        packages = {p: u for p, u in packages.items() if p in wanted}

    rows = []
    for package in sorted(packages):
        new_uris = packages[package] - set(installed)
        if not new_uris:
            continue
        inst, candidate, official = apt_info(package)
        if inst:
            continue
        if not candidate:
            log("Not available here: %s" % package)
            continue
        if not official:
            log("Not installing %s: version %s is not from the official distribution archive." % (package, candidate))
            continue
        rows.append((package, candidate, len(new_uris)))
    if not rows:
        log("The packages of the plugins MOD has GUIs for are installed already (or not available).")
        return []
    log("Packages with plugins that MOD has GUIs for:")
    for package, candidate, count in rows:
        log("    %-28s %-36s %3d plugin%s with a MOD GUI" % (package, candidate, count, "" if count == 1 else "s"))
    command = ["apt-get", "install", "-y", "--no-install-recommends"] + [r[0] for r in rows]
    total = check_apt_simulation([r[0] for r in rows])
    log("(%d packages in all, with dependencies, all from the official distribution archive.)" % total)
    if args.dry_run:
        log("Would run: %s" % " ".join(command))
        return [r[0] for r in rows]
    if os.geteuid() != 0:
        raise Error("installing packages requires root.")
    log("Running: %s" % " ".join(command))
    result = subprocess.run(command, env=dict(os.environ, DEBIAN_FRONTEND="noninteractive"))
    if result.returncode != 0:
        raise Error("apt-get failed (exit status %d). No GUIs installed." % result.returncode)
    args.packages_installed = True
    return [r[0] for r in rows]


# --------------------------------------------------------------------------
# Main.

def uninstall(dest, dry_run):
    found = False
    if os.path.isdir(dest):
        overlays = [os.path.join(dest, name) for name in sorted(os.listdir(dest))
                    if name.endswith("-modgui.lv2") and is_our_overlay(os.path.join(dest, name))]
        found = bool(overlays)
        if dry_run:
            for path in overlays:
                log("Would remove %s" % path)
        else:
            with DestLock(dest):
                for path in overlays:
                    log("Removing %s" % path)
                    shutil.rmtree(path)
    if not found:
        log("No MOD GUI overlays installed by this script in %s. Nothing to uninstall." % dest)


def scan(args, dest):
    lv2_dirs = [os.path.abspath(d) for d in args.lv2_dir]
    for d in lv2_dirs:
        if not os.path.isdir(d):
            raise Error("%s does not exist." % d)
    # Bundles in dest that weren't installed by this script (e.g. plugins from other
    # sources in /usr/local/lib/lv2) count as installed plugins too, so that we never
    # add a second GUI to a plugin that has one.
    extra = []
    if os.path.isdir(dest) and dest not in lv2_dirs:
        for name in sorted(os.listdir(dest)):
            path = os.path.join(dest, name)
            if os.path.isdir(path) and not name.startswith(".") and not is_our_overlay(path) \
                    and os.path.isfile(os.path.join(path, "manifest.ttl")):
                extra.append(path)
    return scan_installed(lv2_dirs, extra), lv2_dirs + extra


def install(args, sources, work_dir):
    dest = os.path.abspath(args.dest)
    installed, scanned = scan(args, dest)
    needy = {uri: p for uri, p in installed.items() if not p.has_working_gui}
    log("%d installed plugins in %s; %d have a working MOD GUI already, %d don't."
        % (len(installed), ":".join(scanned), len(installed) - len(needy), len(needy)))
    return _install(args, dest, installed, needy, work_dir, sources)


def _install(args, dest, installed, needy, work_dir, sources):
    mod_source, gx_source = sources

    good = {}        # uri -> [Candidate] that pass every check, in source order
    rejected = {}    # uri -> [(candidate name, reason)]

    def try_source(source, uris):
        source.fetch_index(work_dir)
        index = index_source(source)
        passing = []
        for uri in sorted(uris):
            for candidate in index.get(uri, []):
                reason = check_description(candidate, installed[uri])
                if reason:
                    rejected.setdefault(uri, []).append((candidate.name(), reason))
                else:
                    passing.append(candidate)
        source.fetch_bundles({c.rel_bundle for c in passing})
        for candidate in passing:
            reason = check_files(candidate, installed[candidate.uri])
            if reason:
                rejected.setdefault(candidate.uri, []).append((candidate.name(), reason))
            else:
                good.setdefault(candidate.uri, []).append(candidate)

    if needy:
        try_source(mod_source, needy)
    # Guitarix fallback: the upstream release that matches the distro package, for
    # Guitarix plugins that MOD has no matching GUI for, or whose MOD GUI was made
    # for different control ranges.
    gx_uris = [uri for uri in needy if uri.startswith(GX_URI_PREFIX)
               and (uri not in good or good[uri][0].warnings or good[uri][0].port_map)]
    if gx_uris:
        try_source(gx_source, gx_uris)

    # Prefer the GUI with the fewest hidden controls, then the fewest other
    # differences (ranges, mapped ports); then MOD's.
    chosen = {uri: min(candidates, key=lambda c: (len(c.hidden()), len(c.warnings) + len(c.port_map)))
              for uri, candidates in good.items()}
    skipped = {uri: reasons for uri, reasons in rejected.items() if uri not in chosen}

    # Group the chosen GUIs by source bundle: one overlay each.
    overlays = {}
    for uri, candidate in chosen.items():
        name = overlay_name(candidate.source, candidate.rel_bundle)
        overlays.setdefault(name, []).append(candidate)
    for name, candidates in overlays.items():
        if len({(c.source.label, c.rel_bundle) for c in candidates}) != 1:
            raise Error("internal error: overlay name clash for %s" % name)

    # Build every overlay before changing anything in dest. A GUI that fails here
    # (e.g. a file changed since it was checked) is skipped, not fatal.
    staging_root = os.path.join(work_dir, "staging")
    shutil.rmtree(staging_root, ignore_errors=True)
    os.makedirs(staging_root)

    def skip(candidate, reason):
        skipped.setdefault(candidate.uri, []).append((candidate.name(), reason))
        chosen.pop(candidate.uri, None)

    for name in sorted(overlays):
        staging = os.path.join(staging_root, name)
        target = os.path.join(dest, name)
        if os.path.lexists(target) and not is_our_overlay(target):
            for c in overlays.pop(name):
                skip(c, "%s exists and was not installed by this script" % target)
            continue
        try:
            build_overlay(staging, overlays[name])
            continue
        except (Error, OSError) as e:
            shutil.rmtree(staging, ignore_errors=True)
            if len(overlays[name]) == 1:
                skip(overlays.pop(name)[0], str(e))
                continue
        # Find the GUIs that fail, and build the overlay without them.
        good_ones = []
        for c in overlays[name]:
            trial = os.path.join(work_dir, "trial")
            try:
                build_overlay(trial, [c])
                good_ones.append(c)
            except (Error, OSError) as e:
                skip(c, str(e))
            finally:
                shutil.rmtree(trial, ignore_errors=True)
        overlays[name] = good_ones
        try:
            if not good_ones:
                raise Error("no GUIs left")
            build_overlay(staging, good_ones)
        except (Error, OSError) as e:
            shutil.rmtree(staging, ignore_errors=True)
            for c in overlays.pop(name):
                skip(c, str(e))

    def apply_changes():
        changed = unchanged = removed = 0
        for name in sorted(overlays):
            staging = os.path.join(staging_root, name)
            target = os.path.join(dest, name)
            if os.path.isdir(target) and not os.path.islink(target) and same_tree(staging, target):
                unchanged += 1
                continue
            changed += 1
            log("%s %s (%d plugin%s)" % ("Would install" if args.dry_run else "Installing", target,
                                          len(overlays[name]), "" if len(overlays[name]) == 1 else "s"))
            if not args.dry_run:
                swap_in(staging, target)

        # Remove overlays this script installed earlier that are no longer wanted: the
        # plugin was removed, now has its own GUI, or the GUI now comes from elsewhere.
        if os.path.isdir(dest):
            for name in sorted(os.listdir(dest)):
                path = os.path.join(dest, name)
                if name.endswith("-modgui.lv2") and name not in overlays and is_our_overlay(path):
                    removed += 1
                    log("%s stale overlay %s" % ("Would remove" if args.dry_run else "Removing", path))
                    if not args.dry_run:
                        shutil.rmtree(path)
        return changed, unchanged, removed

    if args.dry_run:
        changed, unchanged, removed = apply_changes()
    else:
        with DestLock(dest):
            changed, unchanged, removed = apply_changes()

    report(args, installed, needy, chosen, skipped, changed, unchanged, removed)
    return 0


def report(args, installed, needy, chosen, skipped, changed, unchanged, removed):
    log()
    adapted = {uri for uri, c in chosen.items() if c.port_map}

    def list_plugins(uris):
        for uri in sorted(uris, key=lambda u: (installed[u].name.lower(), u)):
            c = chosen[uri]
            log("    %-28s <%s>  [%s]" % (installed[uri].name, uri, c.name()))
            for adaptation in c.adaptations():
                log("        adapted: %s" % adaptation)
            for warning in c.warnings:
                log("        note: %s" % warning)

    if len(chosen) > len(adapted):
        log("Plugins %s:" % ("that would get a MOD GUI" if args.dry_run else "with an installed MOD GUI"))
        list_plugins(set(chosen) - adapted)
    if adapted:
        log()
        log("Plugins %s a MOD GUI adapted to the installed plugin (ports that MOD's build of the plugin names "
            "differently are mapped, or their controls hidden):" % ("that would get" if args.dry_run else "with"))
        list_plugins(adapted)
    if skipped:
        log()
        log("MOD GUIs not installed (incompatible with the installed plugin, or unusable):")
        for uri in sorted(skipped):
            for name, reason in skipped[uri][:1]:
                log("    <%s>: %s [%s]" % (uri, reason, name))
    log()
    log("Summary: %d plugin%s with an added MOD GUI, %d of them adapted (%d overlay%s %s, %d up to date, "
        "%d stale %s); %d skipped as incompatible; %d plugins without a working MOD GUI have none in the sources."
        % (len(chosen), "" if len(chosen) == 1 else "s", len(adapted),
           changed, "" if changed == 1 else "s", "to write" if args.dry_run else "written",
           unchanged, removed, "to remove" if args.dry_run else "removed", len(skipped), len(needy) - len(chosen) - len(skipped)))
    if not args.dry_run and (changed or removed or getattr(args, "packages_installed", False)):
        log("Restart PiPedal to pick up the changes (its plugin list is refreshed when it starts):")
        log("    sudo systemctl restart pipedald")


def main():
    parser = argparse.ArgumentParser(prog="install-mod-guis.sh", add_help=False)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--uninstall", action="store_true")
    parser.add_argument("--install-plugins", nargs="?", const="all")
    parser.add_argument("--dest", default="/usr/local/lib/lv2")
    parser.add_argument("--lv2-dir", action="append")
    parser.add_argument("--source")
    parser.add_argument("--guitarix-source")
    parser.add_argument("--port-aliases")
    args = parser.parse_args()
    if args.port_aliases is not None:
        if not os.path.isfile(args.port_aliases):
            raise Error("%s is not a file." % args.port_aliases)
        _port_aliases["path"] = os.path.abspath(args.port_aliases)
    args.lv2_dir = args.lv2_dir or ["/usr/lib/lv2"]
    for name in ("source", "guitarix_source"):
        value = getattr(args, name)
        if value is not None:
            if not os.path.isdir(value):
                raise Error("%s is not a directory." % value)
            setattr(args, name, os.path.abspath(value))
    if args.uninstall:
        if args.install_plugins:
            raise Error("--uninstall and --install-plugins can't be combined.")
        uninstall(os.path.abspath(args.dest), args.dry_run)
        return 0
    sources = (Source("mod-lv2-data", MOD_REPO, MOD_COMMIT, MOD_BUNDLE_DIRS, args.source),
               Source("guitarix", GX_REPO, GX_COMMIT, GX_BUNDLE_DIRS, args.guitarix_source))
    work_dir = tempfile.mkdtemp(prefix="pipedal-mod-guis-")
    try:
        new_packages = []
        if args.install_plugins:
            new_packages = install_plugins(args, sources[0], work_dir)
            log()
        result = install(args, sources, work_dir)
        if new_packages and args.dry_run:
            log("(The GUIs listed above are for the plugins installed now. GUIs for the plugins in %s"
                % " ".join(new_packages))
            log(" are checked against those plugins once the packages are installed.)")
        return result
    finally:
        shutil.rmtree(work_dir, ignore_errors=True)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Error as e:
        print("Error: %s" % e, file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)
