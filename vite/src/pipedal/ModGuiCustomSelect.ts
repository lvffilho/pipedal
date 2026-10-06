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

// mod-widget="custom-select": an enumerated control port whose options the ModGUI
// template renders itself, as
//     <div mod-role="input-control-port" mod-port-symbol="..." mod-widget="custom-select">
//         <div mod-role="input-control-value"></div>
//         <div class="mod-enumerated-list">
//             <div mod-role="enumeration-option" mod-port-value="0">Label</div> ...
//         </div>
//     </div>
// mod-ui implements the widget itself (JqueryClass('customSelect') in html/js/modgui.js),
// not the plugin's script: a click on the widget toggles .mod-enumerated-list; a click
// on an option selects its value; the selected option gets the 'selected' class, and
// the input-control-value element shows its text. This does the same.
//
// No runtime imports, so that it can be exercised outside the React app.

import type { MonitorPortHandle } from './PiPedalModel';

export interface CustomSelectControlProps {
    instanceId: number;
    symbol: string;
    onValueChanged: (instanceId: number, symbol: string, value: number) => void;
    monitorPort: (
        instanceId: number,
        symbol: string,
        interval: number,
        callback: (value: number) => void) => MonitorPortHandle;
    unmonitorPort: (handle: MonitorPortHandle) => void;
};

// The option for value: an exact match, else within 0.0001, else the nearest
// (mod-ui's customSelect getSelectedByValue, which matches the server's
// get_nearest_valid_scalepoint_value).
export function getSelectedOption(options: Element[], value: number): Element | null {
    if (options.length === 0 || isNaN(value)) {
        return null;
    }
    let best: Element | null = null;
    let bestError = Infinity;
    for (const option of options) {
        const optionValue = parseFloat(option.getAttribute("mod-port-value") ?? "");
        if (isNaN(optionValue)) continue;
        const error = Math.abs(optionValue - value);
        if (error < bestError) {
            bestError = error;
            best = option;
        }
    }
    return best;
}

export class CustomSelectControl {

    private props: CustomSelectControlProps;
    private frameElement?: HTMLElement;

    private value: number = NaN;
    private animationFrameId: number | null = null;
    private mounted: boolean = false;
    private monitorHandle: MonitorPortHandle | null = null;
    private clickListener = (event: MouseEvent) => { this.handleClick(event); };

    constructor(props: CustomSelectControlProps) {
        this.props = props;
    }

    requestUpdate() {
        if (!this.mounted || !this.frameElement) {
            return;
        }
        if (this.animationFrameId === null) {
            this.animationFrameId = window.requestAnimationFrame(() => {
                this.animationFrameId = null;
                this.updateSelection();
            });
        }
    }
    cancelRequestUpdate() {
        if (this.animationFrameId !== null) {
            window.cancelAnimationFrame(this.animationFrameId);
            this.animationFrameId = null;
        }
    }
    setControlValue(value: number) {
        if (this.value !== value) {
            this.value = value;
            this.requestUpdate();
        }
    }

    private getOptions(): Element[] {
        if (!this.frameElement) return [];
        return Array.from(this.frameElement.querySelectorAll('[mod-role=enumeration-option]'));
    }

    updateSelection() {
        if (!this.frameElement || !this.mounted) {
            return;
        }
        const options = this.getOptions();
        for (const option of options) {
            option.classList.remove("selected");
        }
        const selected = getSelectedOption(options, this.value);
        if (selected) {
            selected.classList.add("selected");
        }
        const text = selected ? (selected.textContent ?? "") : "";
        this.frameElement.querySelectorAll('[mod-role=input-control-value]')
            .forEach((valueElement: Element) => {
                valueElement.textContent = text;
            });
    }

    onMounted() {
        this.mounted = true;
        this.monitorHandle = this.props.monitorPort(
            this.props.instanceId,
            this.props.symbol,
            1.0 / 15,
            (value: number) => {
                this.setControlValue(value);
            }
        );
        this.requestUpdate();
    }

    onUnmount() {
        if (this.monitorHandle) {
            this.props.unmonitorPort(this.monitorHandle);
            this.monitorHandle = null;
        }
        if (this.frameElement) {
            this.frameElement.removeEventListener("click", this.clickListener);
        }
        this.mounted = false;
        this.cancelRequestUpdate();
    }

    render(): HTMLElement | null {
        return null;
    }

    // jQuery's toggle(): hidden (by the stylesheet or inline) -> shown, and back.
    private static toggleVisibility(element: HTMLElement) {
        if (window.getComputedStyle(element).display === "none") {
            element.style.display = "block";
        } else {
            element.style.display = "none";
        }
    }

    handleClick(event: MouseEvent) {
        if (!this.frameElement) {
            return;
        }
        // A click on an option selects it. Like mod-ui, the click then also reaches
        // the widget, which closes the list.
        const target = event.target instanceof Element
            ? event.target.closest('[mod-role=enumeration-option]')
            : null;
        if (target && this.frameElement.contains(target)) {
            const value = parseFloat(target.getAttribute("mod-port-value") ?? "");
            if (!isNaN(value)) {
                this.setControlValue(value);
                this.updateSelection();
                this.props.onValueChanged(this.props.instanceId, this.props.symbol, value);
            }
        }
        const list = this.frameElement.querySelector(".mod-enumerated-list");
        if (list instanceof HTMLElement) {
            CustomSelectControl.toggleVisibility(list);
        }
    }

    attach(frameElement: HTMLElement) {
        this.frameElement = frameElement;
        this.frameElement.addEventListener("click", this.clickListener);
        this.requestUpdate();
    }
};
