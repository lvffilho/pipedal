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

import { ObservableProperty } from './ObservableProperty';

// Horizontal placement of the pedal chain and of the plugin control panel. A browser-local
// display preference, so it is stored in localStorage rather than in server settings.
export enum ContentAlignment {
    Start = "Start",
    Center = "Center"
};

const CONTENT_ALIGNMENT_STORAGE_KEY = "com.twoplay.pipedal.content_alignment";
const DEFAULT_CONTENT_ALIGNMENT = ContentAlignment.Center;

function readStoredContentAlignment(): ContentAlignment {
    switch (localStorage.getItem(CONTENT_ALIGNMENT_STORAGE_KEY)) {
        case ContentAlignment.Start:
            return ContentAlignment.Start;
        case ContentAlignment.Center:
            return ContentAlignment.Center;
        default:
            return DEFAULT_CONTENT_ALIGNMENT;
    }
}

// Observed by the pedalboard and control views, so toggling re-lays them out without a reload.
export const contentAlignmentProperty: ObservableProperty<ContentAlignment>
    = new ObservableProperty<ContentAlignment>(readStoredContentAlignment());

export function getContentAlignment(): ContentAlignment {
    return contentAlignmentProperty.get();
}

export function setContentAlignment(value: ContentAlignment): void {
    localStorage.setItem(CONTENT_ALIGNMENT_STORAGE_KEY, value);
    contentAlignmentProperty.set(value);
}
