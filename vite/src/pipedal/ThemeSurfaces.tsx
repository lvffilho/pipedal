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

// Surface values shared by the MUI theme and by individual dialogs, which must agree.

const DARK_BAR_GRADIENT = 'linear-gradient(180deg, rgba(48,42,66,0.96) 0%, rgba(34,32,48,0.92) 100%)';
const LIGHT_BAR_GRADIENT = 'linear-gradient(180deg, rgba(255,255,255,0.96) 0%, rgba(248,247,252,0.92) 100%)';
const LIGHT_ACTION_BAR_GRADIENT = 'linear-gradient(180deg, rgba(103,80,164,0.96) 0%, rgba(83,64,134,0.92) 100%)';

export const HAIRLINE_DARK = 'rgba(255,255,255,0.06)';
export const HAIRLINE_LIGHT = 'rgba(0,0,0,0.06)';

export const TOOLBAR_ICON_OPACITY = 0.85;

export function appBarBackgroundImage(isDark: boolean): string {
    return isDark ? DARK_BAR_GRADIENT : LIGHT_BAR_GRADIENT;
}

// Keeps white text in both themes, so it stays coloured in light mode.
export function dialogActionBarBackgroundImage(isDark: boolean): string {
    return isDark ? DARK_BAR_GRADIENT : LIGHT_ACTION_BAR_GRADIENT;
}

export function hairlineColor(isDark: boolean): string {
    return isDark ? HAIRLINE_DARK : HAIRLINE_LIGHT;
}
