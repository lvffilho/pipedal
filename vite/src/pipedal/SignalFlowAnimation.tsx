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

import { ObservableProperty } from './ObservableProperty';

// Whether the pedalboard connector dashes and the active-plugin LED animate. Off by default: the
// animations keep the compositor busy, which matters on small hosts that also run the audio.
// A browser-local display preference, stored in localStorage like the other display settings.
const ANIMATE_SIGNAL_FLOW_STORAGE_KEY = "com.twoplay.pipedal.animate_signal_flow";
const ANIMATE_CLASS = "pp-animate-flow";

function readStoredAnimateSignalFlow(): boolean {
    try {
        return localStorage.getItem(ANIMATE_SIGNAL_FLOW_STORAGE_KEY) === "true";
    } catch (e) {
        return false;
    }
}

function applyAnimateClass(value: boolean): void {
    document.documentElement.classList.toggle(ANIMATE_CLASS, value);
}

export const animateSignalFlowProperty: ObservableProperty<boolean>
    = new ObservableProperty<boolean>(readStoredAnimateSignalFlow());

applyAnimateClass(animateSignalFlowProperty.get());

export function getAnimateSignalFlow(): boolean {
    return animateSignalFlowProperty.get();
}

export function setAnimateSignalFlow(value: boolean): void {
    try {
        localStorage.setItem(ANIMATE_SIGNAL_FLOW_STORAGE_KEY, value ? "true" : "false");
    } catch (e) {
        // storage unavailable: preference lasts for this session only.
    }
    applyAnimateClass(value);
    animateSignalFlowProperty.set(value);
}
