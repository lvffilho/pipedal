/*
 *   Copyright (c) Robin E.R. Davies
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

// Runs a ModGUI's modgui:javascript the way mod-ui does (html/js/modgui.js in
// moddevices/mod-ui, API version 3).
//
// The script file is a single anonymous function expression,
//     function (event, funcs) { ... }
// called with
//     { type: 'start', ports: [{symbol, value}...], parameters: [{uri, value}...] }
// once the icon is rendered, then with
//     { type: 'change', symbol, value }   (control port, input or output; ':bypass')
//     { type: 'change', uri, value }      (patch property)
// whenever a value changes. Every event also carries icon (the jQuery-wrapped icon
// element), settings, data (an object the script may keep state in) and api_version.
//
// funcs: set_port_value(symbol, value), patch_get(uri), patch_set(uri, valuetype, value),
// get_custom_resource_filename(filename), get_port_index_for_symbol(symbol),
// get_port_symbol_for_index(index).
//
// As in mod-ui, a value set by the script itself is not echoed back to it as a
// change event, and a script that throws is disabled.
//
// Scripts are not sandboxed: like in mod-ui, they run in the page's context, with
// the page's privileges. funcs is the only PiPedal API handed to them, but nothing
// stops a script from reaching the DOM or the network. They are trusted to the same
// degree as the plugin bundle (or ModGUI overlay) that installed them.
//
// This file has no runtime imports so that it can be tested under plain node
// (vite/test/ModGuiScript.test.ts).

export const MOD_GUI_SCRIPT_API_VERSION = 3;

export const BYPASS_SYMBOL = ":bypass";

export interface ModGuiScriptPort {
    symbol: string;
    index: number;
    isInput: boolean;
    minValue: number;
    maxValue: number;
}

export interface ModGuiScriptEvent {
    type: "start" | "change";
    icon?: unknown;
    settings?: unknown;
    data?: object;
    api_version?: number;
    ports?: { symbol: string, value: number }[];
    parameters?: { uri: string, value: unknown }[];
    symbol?: string;
    uri?: string;
    value?: unknown;
}

export interface ModGuiScriptFuncs {
    set_port_value: (symbol: string, value: number) => void;
    patch_get: (uri: string) => void;
    patch_set: (uri: string, valuetype: string, value: unknown) => void;
    get_custom_resource_filename: (filename: string) => string;
    get_port_index_for_symbol: (symbol: string) => number;
    get_port_symbol_for_index: (index: number) => string | null;
}

export type ModGuiScriptCallback = (event: ModGuiScriptEvent, funcs: ModGuiScriptFuncs) => void;

// What the host lets a script do. Nothing else of PiPedal is reachable through funcs.
export interface ModGuiScriptSite {
    // Plugin control ports, inputs and outputs.
    ports: ModGuiScriptPort[];
    // Set a control port (or the BYPASS_SYMBOL pseudo-port), as a widget would.
    setPortValue: (symbol: string, value: number) => void;
    // Send a patch:Get for the property. The plugin's reply (a patch:Set) arrives
    // through the host's patch monitor, as a change event; nothing is returned.
    patchGet: (uri: string) => void;
    // Send a patch:Set. value is in PiPedal's atom JSON form (see encodePatchValue).
    patchSet: (uri: string, atomJson: unknown) => void;
    // URL of a file in the ModGUI's resource directory.
    customResourceUrl: (filename: string) => string;
}

function warn(message: string, error?: unknown) {
    if (error === undefined) {
        console.warn("pipedal: ModGUI javascript: " + message);
    } else {
        console.warn("pipedal: ModGUI javascript: " + message, error);
    }
}

// Evaluate a modgui:javascript file the way mod-ui does (eval('method = ' + code)),
// but with $ and jQuery bound to the given jQuery rather than to globals. Returns
// null if the code doesn't evaluate to a function.
export function compileModGuiScript(source: string, jquery: unknown): ModGuiScriptCallback | null {
    try {
        // A trailing newline before the ';', so that a script ending in a // comment
        // still parses.
        const factory = new Function("$", "jQuery",
            "var method;\nmethod = " + source + "\n;\nreturn method;");
        const method = factory(jquery, jquery);
        if (typeof method !== "function") {
            warn("the script is not a function.");
            return null;
        }
        return method as ModGuiScriptCallback;
    } catch (error) {
        warn("failed to evaluate the script.", error);
        return null;
    }
}

// PiPedal atom JSON (as sent by monitorPatchProperty, or stored in pathProperties)
// to the plain values mod-ui gives scripts: numbers, strings, arrays; bools as 1/0.
export function decodePatchValue(json: unknown): unknown {
    if (json === null || json === undefined) {
        return json;
    }
    if (typeof json === "boolean") {
        return json ? 1 : 0;
    }
    if (typeof json !== "object" || Array.isArray(json)) {
        return json;
    }
    const atom = json as { otype_?: string, value?: unknown };
    switch (atom.otype_) {
        case "Bool":
            return atom.value ? 1 : 0;
        case "Int":
        case "Long":
        case "Float":
        case "Double":
        case "String":
        case "Path":
        case "URI":
        case "URID":
        case "Vector":
            return atom.value;
        default:
            return json;
    }
}

const VECTOR_TYPES: { [key: string]: string } = { b: "Bool", i: "Int", l: "Long", f: "Float", g: "Double" };

// A mod-ui patch_set(uri, valuetype, value) value to PiPedal atom JSON. valuetype is
// the LV2 atom type letter mod-ui uses: b(ool), i(nt), l(ong), f(loat), g (double),
// s(tring), p(ath), u(ri), v(ector: value is [childtype, ...values]). Returns
// undefined for a value that can't be converted.
export function encodePatchValue(valuetype: string, value: unknown): unknown {
    switch (valuetype) {
        case "b":
            // PiPedal's atom JSON for an atom:Bool is a plain JSON boolean.
            return !!value;
        case "i":
        case "l": {
            const n = Number(value);
            if (!isFinite(n)) return undefined;
            return { otype_: valuetype === "i" ? "Int" : "Long", value: Math.round(n) };
        }
        case "f": {
            const n = Number(value);
            if (!isFinite(n)) return undefined;
            return n;
        }
        case "g": {
            const n = Number(value);
            if (!isFinite(n)) return undefined;
            return { otype_: "Double", value: n };
        }
        case "s":
            return String(value ?? "");
        case "p":
            return { otype_: "Path", value: String(value ?? "") };
        case "u":
            return { otype_: "URI", value: String(value ?? "") };
        case "v": {
            if (!Array.isArray(value) || value.length === 0 || typeof value[0] !== "string") {
                return undefined;
            }
            const vtype = VECTOR_TYPES[value[0]];
            if (!vtype) return undefined;
            const values = value.slice(1).map((v) => (vtype === "Bool" ? !!v : Number(v)));
            if (vtype !== "Bool" && !values.every((v) => isFinite(v as number))) {
                return undefined;
            }
            return { otype_: "Vector", vtype_: vtype, value: values };
        }
        default:
            return undefined;
    }
}

export class ModGuiScriptHost {
    private callback: ModGuiScriptCallback | null;
    private site: ModGuiScriptSite;
    private icon: unknown;
    private settings: unknown;
    private data: object = {};
    private started = false;
    private disposed = false;
    // Last value of each port, as seen by the script; a change event is only sent
    // when the value differs. Values set by the script land here first, so they
    // aren't echoed back (mod-ui doesn't echo them either).
    private values = new Map<string, number>();
    private portsBySymbol = new Map<string, ModGuiScriptPort>();

    readonly funcs: ModGuiScriptFuncs;

    constructor(callback: ModGuiScriptCallback, site: ModGuiScriptSite, icon: unknown, settings?: unknown) {
        this.callback = callback;
        this.site = site;
        this.icon = icon;
        this.settings = settings ?? icon;
        for (const port of site.ports) {
            this.portsBySymbol.set(port.symbol, port);
        }
        // Closures, not methods: scripts call these detached (and from timers that may
        // fire after the GUI is gone, so every one checks disposed).
        this.funcs = {
            set_port_value: (symbol: string, value: number) => { this.setPortValue(symbol, value); },
            patch_get: (uri: string) => {
                if (this.disposed) return;
                try {
                    this.site.patchGet(String(uri));
                } catch (error) {
                    warn("patch_get(" + uri + ") failed.", error);
                }
            },
            patch_set: (uri: string, valuetype: string, value: unknown) => {
                if (this.disposed) return;
                const atom = encodePatchValue(valuetype, value);
                if (atom === undefined) {
                    warn("patch_set(" + uri + "): can't convert a value of type '" + valuetype + "'.");
                    return;
                }
                try {
                    this.site.patchSet(String(uri), atom);
                } catch (error) {
                    warn("patch_set(" + uri + ") failed.", error);
                }
            },
            get_custom_resource_filename: (filename: string) => this.site.customResourceUrl(String(filename)),
            get_port_index_for_symbol: (symbol: string) => {
                const port = this.portsBySymbol.get(symbol);
                return port ? port.index : -1;
            },
            get_port_symbol_for_index: (index: number) => {
                for (const port of this.site.ports) {
                    if (port.index === index) return port.symbol;
                }
                return null;
            },
        };
    }

    get isStarted(): boolean { return this.started; }
    get isDisabled(): boolean { return this.callback === null; }

    private setPortValue(symbol: string, value: number): void {
        if (this.disposed) return;
        value = Number(value);
        if (isNaN(value)) {
            warn("set_port_value(" + symbol + "): invalid value.");
            return;
        }
        if (symbol === BYPASS_SYMBOL) {
            value = value ? 1 : 0;
        } else {
            const port = this.portsBySymbol.get(symbol);
            if (!port || !port.isInput) {
                warn("set_port_value: no input control port '" + symbol + "'.");
                return;
            }
            // mod-ui clamps too.
            if (value < port.minValue) value = port.minValue;
            if (value > port.maxValue) value = port.maxValue;
        }
        const previous = this.values.get(symbol);
        if (previous === value) {
            return;
        }
        // Before the call: the model may echo the change back synchronously.
        this.values.set(symbol, value);
        try {
            this.site.setPortValue(symbol, value);
        } catch (error) {
            // Not set, so a later change to this value must still reach the script.
            if (previous === undefined) {
                this.values.delete(symbol);
            } else {
                this.values.set(symbol, previous);
            }
            warn("set_port_value(" + symbol + ") failed.", error);
        }
    }

    private trigger(event: ModGuiScriptEvent): void {
        const callback = this.callback;
        if (!callback || this.disposed || !this.started) return;
        event.api_version = MOD_GUI_SCRIPT_API_VERSION;
        event.data = this.data;
        event.icon = this.icon;
        event.settings = this.settings;
        try {
            callback(event, this.funcs);
        } catch (error) {
            // mod-ui disables a script that throws. Never let it break the host UI.
            this.callback = null;
            warn("the script threw, and has been disabled.", error);
        }
    }

    // ports: current values of the input ports (and ':bypass'); output ports are
    // added at their minimum if missing, as mod-ui reports their defaults.
    start(ports: { symbol: string, value: number }[], parameters: { uri: string, value: unknown }[]): void {
        if (this.started || this.disposed) return;
        const jsPorts: { symbol: string, value: number }[] = [];
        for (const port of ports) {
            this.values.set(port.symbol, port.value);
            jsPorts.push({ symbol: port.symbol, value: port.value });
        }
        for (const port of this.site.ports) {
            if (!this.values.has(port.symbol)) {
                this.values.set(port.symbol, port.minValue);
                jsPorts.push({ symbol: port.symbol, value: port.minValue });
            }
        }
        this.started = true;
        this.trigger({
            type: "start",
            ports: jsPorts,
            parameters: parameters.map((p) => ({ uri: p.uri, value: p.value }))
        });
    }

    // A control port (input or output) or ':bypass' changed, from any source.
    portChanged(symbol: string, value: number): void {
        if (this.disposed) return;
        if (this.values.get(symbol) === value) return;
        this.values.set(symbol, value);
        this.trigger({ type: "change", symbol: symbol, value: value });
    }

    // A patch property changed. value is already decoded (decodePatchValue).
    parameterChanged(uri: string, value: unknown): void {
        if (this.disposed) return;
        this.trigger({ type: "change", uri: uri, value: value });
    }

    dispose(): void {
        this.disposed = true;
        this.callback = null;
        this.values.clear();
        this.data = {};
    }
}
