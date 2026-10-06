// Tests for the ModGUI javascript bridge (src/pipedal/ModGuiScript.ts).
//
//     cd vite && node --test test/
//
// (node >= 23.6 runs TypeScript directly.)

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    ModGuiScriptHost, compileModGuiScript, decodePatchValue, encodePatchValue,
    BYPASS_SYMBOL, MOD_GUI_SCRIPT_API_VERSION,
} from '../src/pipedal/ModGuiScript.ts';
import type { ModGuiScriptEvent, ModGuiScriptFuncs, ModGuiScriptSite } from '../src/pipedal/ModGuiScript.ts';

interface Recorder {
    site: ModGuiScriptSite;
    setPortValues: [string, number][];
    patchSets: [string, unknown][];
    patchGets: string[];
}

function makeSite(): Recorder {
    const recorder: Recorder = {
        setPortValues: [],
        patchSets: [],
        patchGets: [],
        site: {
            ports: [
                { symbol: "model", index: 4, isInput: true, minValue: 0, maxValue: 13 },
                { symbol: "gain", index: 5, isInput: true, minValue: 0, maxValue: 1 },
                { symbol: "clip", index: 9, isInput: false, minValue: 0, maxValue: 1 },
            ],
            setPortValue: (symbol, value) => { recorder.setPortValues.push([symbol, value]); },
            patchGet: (uri) => { recorder.patchGets.push(uri); },
            patchSet: (uri, atom) => { recorder.patchSets.push([uri, atom]); },
            customResourceUrl: (filename) => "/resources/" + filename + "?ns=x",
        }
    };
    return recorder;
}

function makeHost(onEvent: (event: ModGuiScriptEvent, funcs: ModGuiScriptFuncs) => void) {
    const recorder = makeSite();
    const icon = { isIcon: true };
    const host = new ModGuiScriptHost(onEvent, recorder.site, icon);
    return { host, recorder, icon };
}

test("compileModGuiScript evaluates a MOD script file", () => {
    const jquery = () => "jq";
    const source = "function (event, funcs) {\n    return [event.type, typeof funcs, $(), jQuery()];\n}\n";
    const callback = compileModGuiScript(source, jquery);
    assert.ok(callback);
    assert.deepEqual((callback as unknown as (e: object, f: object) => unknown)({ type: "start" }, {}), ["start", "object", "jq", "jq"]);
});

test("compileModGuiScript tolerates a trailing ';' and comments", () => {
    assert.ok(compileModGuiScript("// header\nfunction (event) {};\n// trailer", null));
});

test("compileModGuiScript rejects broken or non-function scripts", () => {
    const warn = console.warn;
    console.warn = () => { };
    try {
        assert.equal(compileModGuiScript("function (event {", null), null);
        assert.equal(compileModGuiScript("42", null), null);
    } finally {
        console.warn = warn;
    }
});

test("start passes ports, parameters, icon, data and api_version", () => {
    const events: ModGuiScriptEvent[] = [];
    const { host, icon } = makeHost((event) => { events.push({ ...event }); });
    host.portChanged("model", 3); // before start: not delivered.
    assert.equal(events.length, 0);
    host.start(
        [{ symbol: BYPASS_SYMBOL, value: 0 }, { symbol: "model", value: 2 }, { symbol: "gain", value: 0.5 }],
        [{ uri: "urn:p#file", value: "/a/b.nam" }]);
    assert.equal(events.length, 1);
    const start = events[0];
    assert.equal(start.type, "start");
    assert.equal(start.icon, icon);
    assert.equal(start.settings, icon);
    assert.equal(start.api_version, MOD_GUI_SCRIPT_API_VERSION);
    assert.deepEqual(start.data, {});
    assert.deepEqual(start.ports, [
        { symbol: BYPASS_SYMBOL, value: 0 }, { symbol: "model", value: 2 }, { symbol: "gain", value: 0.5 },
        { symbol: "clip", value: 0 }, // output port, at its minimum
    ]);
    assert.deepEqual(start.parameters, [{ uri: "urn:p#file", value: "/a/b.nam" }]);
});

test("change events are sent for actual changes only", () => {
    const events: ModGuiScriptEvent[] = [];
    const { host } = makeHost((event) => { if (event.type === "change") events.push({ ...event }); });
    host.start([{ symbol: "model", value: 2 }], []);
    host.portChanged("model", 2);
    assert.equal(events.length, 0);
    host.portChanged("model", 5);
    host.portChanged("model", 5);
    host.portChanged("clip", 1);
    host.parameterChanged("urn:p#meters", "0.5|0.25");
    host.parameterChanged("urn:p#meters", "0.5|0.25"); // parameters aren't de-duplicated.
    assert.deepEqual(events.map((e) => [e.symbol, e.uri, e.value]), [
        ["model", undefined, 5],
        ["clip", undefined, 1],
        [undefined, "urn:p#meters", "0.5|0.25"],
        [undefined, "urn:p#meters", "0.5|0.25"],
    ]);
});

test("the data object persists between events", () => {
    const seen: object[] = [];
    const { host } = makeHost((event) => { seen.push(event.data as object); (event.data as { n?: number }).n = 1; });
    host.start([], []);
    host.portChanged("gain", 0.25);
    assert.equal(seen[0], seen[1]);
    assert.deepEqual(seen[1], { n: 1 });
});

test("set_port_value sets the port, clamped, and isn't echoed back", () => {
    const events: ModGuiScriptEvent[] = [];
    let funcs: ModGuiScriptFuncs | null = null;
    const { host, recorder } = makeHost((event, f) => { funcs = f; if (event.type === "change") events.push(event); });
    host.start([{ symbol: "model", value: 1 }, { symbol: "gain", value: 0.5 }], []);
    assert.ok(funcs);
    const f = funcs as ModGuiScriptFuncs;
    const warn = console.warn;
    console.warn = () => { };
    try {
        f.set_port_value("model", 5);
        f.set_port_value("model", 5);   // unchanged: not sent again.
        f.set_port_value("gain", 7);    // clamped to 1
        f.set_port_value("nosuch", 1);  // ignored
        f.set_port_value("clip", 1);    // output port: ignored
        f.set_port_value(BYPASS_SYMBOL, 1);
    } finally {
        console.warn = warn;
    }
    assert.deepEqual(recorder.setPortValues, [["model", 5], ["gain", 1], [BYPASS_SYMBOL, 1]]);
    // The model echoes the values back: no change events.
    host.portChanged("model", 5);
    host.portChanged("gain", 1);
    assert.equal(events.length, 0);
    // A different value from elsewhere is a change.
    host.portChanged("model", 6);
    assert.equal(events.length, 1);
});

test("set_port_value that fails doesn't poison the de-duplication", () => {
    const events: ModGuiScriptEvent[] = [];
    let funcs: ModGuiScriptFuncs | null = null;
    const recorder = makeSite();
    recorder.site.setPortValue = () => { throw new Error("socket closed"); };
    const host = new ModGuiScriptHost((event, f) => { funcs = f; if (event.type === "change") events.push(event); }, recorder.site, {});
    host.start([{ symbol: "model", value: 1 }], []);
    const warn = console.warn;
    console.warn = () => { };
    try {
        (funcs as unknown as ModGuiScriptFuncs).set_port_value("model", 4);
    } finally {
        console.warn = warn;
    }
    // The value didn't change, so when it does change to 4 elsewhere, the script hears of it.
    host.portChanged("model", 4);
    assert.equal(events.length, 1);
});

test("patch_set encodes values; patch_get asks the host", () => {
    let funcs: ModGuiScriptFuncs | null = null;
    const { host, recorder } = makeHost((_event, f) => { funcs = f; });
    host.start([], []);
    const f = funcs as unknown as ModGuiScriptFuncs;
    f.patch_set("urn:p#name", "s", "Lead");
    f.patch_set("urn:p#file", "p", "/x/y.wav");
    f.patch_set("urn:p#gain", "f", 0.5);
    f.patch_get("urn:p#name");
    assert.deepEqual(recorder.patchSets, [
        ["urn:p#name", "Lead"],
        ["urn:p#file", { otype_: "Path", value: "/x/y.wav" }],
        ["urn:p#gain", 0.5],
    ]);
    assert.deepEqual(recorder.patchGets, ["urn:p#name"]);
});

test("utility funcs", () => {
    let funcs: ModGuiScriptFuncs | null = null;
    const { host } = makeHost((_event, f) => { funcs = f; });
    host.start([], []);
    const f = funcs as unknown as ModGuiScriptFuncs;
    assert.equal(f.get_custom_resource_filename("img/a.png"), "/resources/img/a.png?ns=x");
    assert.equal(f.get_port_index_for_symbol("gain"), 5);
    assert.equal(f.get_port_index_for_symbol("nosuch"), -1);
    assert.equal(f.get_port_symbol_for_index(9), "clip");
    assert.equal(f.get_port_symbol_for_index(99), null);
});

test("a script that throws is disabled, without breaking the host", () => {
    let calls = 0;
    const warn = console.warn;
    const warnings: unknown[] = [];
    console.warn = (...args: unknown[]) => { warnings.push(args); };
    try {
        const { host } = makeHost(() => { ++calls; throw new Error("boom"); });
        host.start([], []);
        host.portChanged("gain", 0.1);
        assert.equal(calls, 1);
        assert.ok(host.isDisabled);
        assert.equal(warnings.length, 1);
    } finally {
        console.warn = warn;
    }
});

test("after dispose, events and funcs are inert", () => {
    let funcs: ModGuiScriptFuncs | null = null;
    let calls = 0;
    const { host, recorder } = makeHost((_event, f) => { funcs = f; ++calls; });
    host.start([], []);
    host.dispose();
    host.portChanged("gain", 0.75);
    const f = funcs as unknown as ModGuiScriptFuncs;
    f.set_port_value("gain", 0.75);   // e.g. from a script's setTimeout
    f.patch_set("urn:p#name", "s", "x");
    assert.equal(calls, 1);
    assert.deepEqual(recorder.setPortValues, []);
    assert.deepEqual(recorder.patchSets, []);
});

test("decodePatchValue", () => {
    assert.equal(decodePatchValue(0.5), 0.5);
    assert.equal(decodePatchValue("text"), "text");
    assert.equal(decodePatchValue({ otype_: "Path", value: "/a/b" }), "/a/b");
    assert.equal(decodePatchValue({ otype_: "Int", value: 3 }), 3);
    assert.equal(decodePatchValue({ otype_: "Bool", value: true }), 1);
    assert.equal(decodePatchValue(false), 0);
    assert.deepEqual(decodePatchValue({ otype_: "Vector", vtype_: "Float", value: [1, 2] }), [1, 2]);
    assert.equal(decodePatchValue(null), null);
});

test("encodePatchValue", () => {
    assert.equal(encodePatchValue("b", 1), true);
    assert.equal(encodePatchValue("b", 0), false);
    assert.deepEqual(encodePatchValue("i", 2.6), { otype_: "Int", value: 3 });
    assert.deepEqual(encodePatchValue("l", 7), { otype_: "Long", value: 7 });
    assert.equal(encodePatchValue("f", "0.25"), 0.25);
    assert.deepEqual(encodePatchValue("g", 0.5), { otype_: "Double", value: 0.5 });
    assert.equal(encodePatchValue("s", "x"), "x");
    assert.deepEqual(encodePatchValue("u", "urn:x"), { otype_: "URI", value: "urn:x" });
    assert.deepEqual(encodePatchValue("v", ["f", 1, 2]), { otype_: "Vector", vtype_: "Float", value: [1, 2] });
    assert.deepEqual(encodePatchValue("v", ["b", 1, 0]), { otype_: "Vector", vtype_: "Bool", value: [true, false] });
    assert.equal(encodePatchValue("v", [1, 2]), undefined);
    assert.equal(encodePatchValue("v", ["f", 1, "x"]), undefined);
    assert.equal(encodePatchValue("v", ["i", 1, NaN]), undefined);
    assert.equal(encodePatchValue("f", "abc"), undefined);
    assert.equal(encodePatchValue("?", 1), undefined);
});
