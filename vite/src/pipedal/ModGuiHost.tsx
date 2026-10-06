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

import { UiPlugin, UiControl, ControlType, UiFileProperty } from './Lv2Plugin';
import React from 'react';
import CloseIcon from '@mui/icons-material/Close';
import IconButtonEx from './IconButtonEx';
import Typography from '@mui/material/Typography/Typography';
import ModGuiErrorBoundary from './ModGuiErrorBoundary';
import OkDialog from './OkDialog';
import { pathFileName, pathParentDirectory } from './FileUtils';
import JsonAtom from './JsonAtom';

import {
    PiPedalModel, PiPedalModelFactory, MonitorPortHandle, State,
    ListenHandle, FileRequestResult
} from './PiPedalModel';
import { Pedalboard } from './Pedalboard';
import { CustomSelectControl } from './ModGuiCustomSelect';
import {
    ModGuiScriptHost, ModGuiScriptCallback, ModGuiScriptPort,
    compileModGuiScript, decodePatchValue, BYPASS_SYMBOL
} from './ModGuiScript';


const RANGE_SCALE = 120; // 120 pixels to move from 0 to 1.
const FINE_RANGE_SCALE = RANGE_SCALE * 10; // 1200 pixels to move from 0 to 1.
const ULTRA_FINE_RANGE_SCALE = RANGE_SCALE * 50; // 12000 pixels to move from 0 to 1.

const WHEEL_RANGE_SCALE = 120 * 20;
const FINE_WHEEL_RANGE_SCALE = WHEEL_RANGE_SCALE * 10;
const ULTRA_FINE_WHEEL_RANGE_SCALE = WHEEL_RANGE_SCALE * 50;


export type BypassChangedCallback = (value: boolean) => void;
 
function HtmlEncode(value: string): string {
    value = value.replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
    return value;

}

function htmlEncodeTest() {
    const testString = "&<>'\"\\";

    const encoded = HtmlEncode(testString);
    const div = document.createElement("div");
    div.innerHTML = `<div data-text='${encoded}'>${encoded}</div>`;
    const innerDiv = div.children[0] as HTMLDivElement;
    if (innerDiv.getAttribute("data-text") !== testString) {
        throw new Error("HtmlEncode failed to encode properly: " + encoded);
    }

}

htmlEncodeTest();

function PathToString(json: unknown): string {
    const t = new JsonAtom(json);
    if (t.isPath()) {
        return t.asPath();
    }
    if (t.isString()) {
        return t.asString();
    }
    return "";
}


function StringToPath(path: string): unknown {
    return JsonAtom._Path(path).asAny();
}


export interface ModGuiHostProps {
    instanceId: number,
    plugin: UiPlugin;
    onClose: () => void;
    width?: number;
    height?: number;
    test?: boolean;
    handleFileSelect: (instanceId: number, propertyUri: string, filePath: string) => void;
    onContentReady: (ready: boolean) => void;


}
export interface ModGuiControl {

    //setControlValue: (value: number) => void;
    onMounted: () => void;
    onUnmount: () => void;
    render(): HTMLElement | null;
};

interface CustomSelectPathControlProps {
    instanceId: number;
    plugin: UiPlugin;
    propertyUri: string,
    hostSite: PiPedalModel;

    handleFileSelect: (instanceId: number, propertyUri: string, filePath: string) => void;

};


class CustomSelectPathControl implements ModGuiControl {

    private props: CustomSelectPathControlProps;
    private frameElement?: HTMLElement;

    private animationFrameId: number | null = null;

    constructor(props: CustomSelectPathControlProps) {
        this.props = props;
    }

    private isInlineList: boolean = false;

    requestUpdate() {
        if (!this.mounted) return;
        if (!this.frameElement) {
            return;
        }


        if (this.animationFrameId === null) {
            this.animationFrameId = window.requestAnimationFrame(() => {
                this.animationFrameId = null;
                this.updateImage();
            });
        }
    }
    cancelRequestUpdate() {
        const animationFrameId = this.animationFrameId;
        if (animationFrameId !== null) {
            window.cancelAnimationFrame(animationFrameId);
            this.animationFrameId = null;
        }
    }

    private getFileProperty(): UiFileProperty | null {
        for (const property of this.props.plugin.fileProperties) {
            if (property.patchProperty === this.props.propertyUri) {
                return property;
            }
        }
        return null;
    }

    navBreadcrumbText(breadcrumbs: { pathname: string, displayName: string }[]): string {
        let breadcrumbText = "";
        for (let i = 0; i < breadcrumbs.length - 1; ++i) {
            const breadcrumb = breadcrumbs[i];
            breadcrumbText += "<div mod-role='enumeration-option' mod-filetype='directory'"
                + " mod-parameter-value='" + HtmlEncode(breadcrumb.pathname) + "'"
                + " class='ppmod-dotdot-breadcrumb_link'>"
                + HtmlEncode(breadcrumb.displayName)
                + "</div> / ";
        }
        const breadcrumb = breadcrumbs[breadcrumbs.length - 1];
        breadcrumbText += "<div class='ppmod-dotdot-breadcrumb_current'>"
            + HtmlEncode(breadcrumb.displayName)
            + "</div>";
        return breadcrumbText;
    }

    makeEnumeratedListItem(
        type: string,
        basename: string,
        fullname: string,
        encodebaseName?: boolean
    ): HTMLElement {
        type = HtmlEncode(type);
        fullname = HtmlEncode(fullname);
        if (encodebaseName !== false) {
            basename = HtmlEncode(basename);
        }
        const itemHtml = this.enumeratedListItemTemplate
            .replace(/{{basename}}/g, basename)
            .replace(/{{filetype}}/g, type)
            .replace(/{{fullname}}/g, fullname);
        const t = document.createElement("div");
        t.innerHTML = itemHtml;
        const itemElement = t.firstElementChild as HTMLElement;
        return itemElement;

    }

    fileRequestResult: FileRequestResult | null = null;

    async requestDirectoryUpdate() {
        const model = PiPedalModelFactory.getInstance();
        const fileProperty = this.getFileProperty();
        if (!fileProperty) {
            return;
        }

        const files = await model.requestFileList2(this.navDirectory || "", fileProperty);
        this.fileRequestResult = files;
        if (!this.mounted) {
            return;
        }
        if (this.enumeratedListContainerElement) {
            this.enumeratedListContainerElement.innerHTML = ""; // remove all children.

            if (files.breadcrumbs.length >= 2) {
                const parentDirectory = files.breadcrumbs[files.breadcrumbs.length - 2].pathname;

                const dotdotHtml = "<div class='ppmod-dotdot-flex'>"
                    + "<div mod-role='enumeration-option' mod-filetype='directory'"
                    + " mod-parameter-value='" + HtmlEncode(parentDirectory) + "'"
                    + " class='ppmod-dotdot-text'>[ ../ ]</div>"
                    + "<div class='ppmod-dotdot-path'>"
                    + this.navBreadcrumbText(files.breadcrumbs)
                    + "</div>";

                const dotdotElement = this.makeEnumeratedListItem(
                    "directory",
                    dotdotHtml,
                    parentDirectory,
                    false
                );
                dotdotElement.style.setProperty("border-bottom", "1px solid #888");
                this.enumeratedListContainerElement.appendChild(dotdotElement);
            } else {

                // mod-role="enumeration-option" mod-filetype="{{filetype}}" mod-parameter-value="{{fullname}}"
                const dotdotHtml = "<div class='ppmod-dotdot-flex'>"
                    + "<div class='ppmod-dotdot-text'>&nbsp;</div>"
                    + "<div class='ppmod-dotdot-path'>"
                    + this.navBreadcrumbText(files.breadcrumbs)
                    + "</div>";

                const dotdotElement = this.makeEnumeratedListItem(
                    "directory",
                    dotdotHtml,
                    "",
                    false
                );
                dotdotElement.style.setProperty("border-bottom", "1px solid #888");
                this.enumeratedListContainerElement.appendChild(dotdotElement);

            }

            for (const file of files.files) {
                let displayName = file.displayName;
                if (file.isDirectory) {
                    displayName = '[ ' + displayName + '/ ]';
                }
                const fileType = file.isDirectory ? "directory" : "file";
                const itemElement = this.makeEnumeratedListItem(
                    fileType,
                    displayName,
                    file.pathname
                );
                this.enumeratedListContainerElement.appendChild(itemElement);
            }
            this.updateSelection();
        }

    }

    private navDirectory: string | null = null;

    setNavDirectory(navDirectory: string | null) {
        if (this.navDirectory !== navDirectory) {
            this.navDirectory = navDirectory;
            this.requestDirectoryUpdate();
        }
    }
    private pathValue: string | null = null;
    setPathValue(value: string) {
        if (this.pathValue !== value) {
            this.pathValue = value;
            if (this.pathValue !== "" || this.navDirectory === null) {
                const browsePath = pathParentDirectory(value);
                this.setNavDirectory(browsePath);
            }
            this.requestUpdate();
        }
        if (this.enumeratedValueElement) {
            if (this.pathValue === "") {
                this.enumeratedValueElement.textContent = "<none>";
            } else {
                this.enumeratedValueElement.textContent = pathFileName(this.pathValue);
            }
        }
    }

    private updateImage() {
        if (!this.frameElement || !this.mounted) {
            return;
        }

        // this.frameElement.querySelectorAll('[.mod_enumerated_list]')
        //     .forEach((inputControl: Element) => {

        //         (inputControl as HTMLElement).textContent = text;
        //     });
        // this.frameElement.querySelectorAll('[mod-role=enumeration-option]')
        //     .forEach((inputControl: Element) => {
        //         let strValue = inputControl.getAttribute("mod-port-value");
        //         let value = parseFloat(strValue || ""); 
        //         inputControl.classList.remove("selected");
        //         if (value === this.value) {
        //             inputControl.classList.add("selected");
        //         }

        //         (inputControl as HTMLElement).textContent = text;
        //     });
        //this.frameElement.textContent = text;

    }

    handlePropertyValueChange(value: unknown) {
        const path: string = PathToString(value);
        this.setPathValue(path);
    }
    private mounted: boolean = false;

    private listenHandle: ListenHandle | null = null;
    onMounted() {
        this.mounted = true;
        this.props.hostSite.monitorPatchProperty(this.props.instanceId, this.props.propertyUri,
            (instanceId, propertyUri, value) => {
                this.handlePropertyValueChange(value);
            }
        );
        this.props.hostSite.getPatchProperty(this.props.instanceId, this.props.propertyUri)
            .then((value: unknown) => {
                this.handlePropertyValueChange(value);
            }).
            catch((error: unknown) => {
                if (error instanceof Error) {
                    console.error(error.message);
                } else {
                    console.error(String(error));
                }
                this.handlePropertyValueChange("");
            });
        this.requestUpdate();
    }

    onUnmount() {
        if (this.listenHandle) {
            this.props.hostSite.cancelMonitorPatchProperty(this.listenHandle);
            this.listenHandle = null;
        }
        this.mounted = false;
        this.cancelRequestUpdate();
    }


    render(): HTMLElement | null {
        return null;
    }

    toggleVisibility(element: HTMLElement) {
        element.classList.toggle("hidden");
        const display = element.style.display;
        if (display === "none" || display === "") {
            element.style.display = "block";
        } else {
            element.style.display = "none";
        }
    }


    getValueElement(): HTMLElement | null {
        return null;
    }

    updateSelection() {
        if (!this.frameElement) {
            return;
        }
        const valueElement = this.getValueElement();
        if (valueElement) {
            if (this.pathValue === null || this.pathValue === "") {
                valueElement.textContent = "No file selected";
                return;
            }
            valueElement.textContent = this.pathValue;
        }
        if (this.isInlineList && this.enumeratedListContainerElement) {
            for (let i = 0; i < this.enumeratedListContainerElement.children.length; i++) {
                const child = this.enumeratedListContainerElement.children[i];
                const strValue = child.getAttribute("mod-parameter-value");
                const strType = child.getAttribute("mod-filetype");
                if (strValue === null) return;
                if (strValue === this.pathValue && strType !== "directory") {
                    child.classList.add("selected");
                } else {
                    child.classList.remove("selected");
                }
            };
        }
    }
    handleClick(event: MouseEvent) {
        if (!this.frameElement) {
            return;
        }
        event.preventDefault();
        event.stopPropagation();

        const valueElement = this.getValueElement();

        if (event.target === valueElement) {
            // A click on the value element, so launch the file browser. hooboy!
            return;
        }
        if (!event.target) {
            return;
        }
        const target = event.target as HTMLElement;
        if (target.getAttribute("mod-role") === "enumeration-option") {
            const strValue = target.getAttribute("mod-parameter-value");
            if (strValue === null) return; const strType = target.getAttribute("mod-filetype");
            if (strType === "directory") {
                this.setNavDirectory(strValue);
                return;
            }

            this.setPathValue(strValue);
            this.updateSelection();
            this.props.hostSite.setPatchProperty(this.props.instanceId, this.props.propertyUri, StringToPath(strValue));
            return;
        }
    }

    private enumeratedListContainerElement: HTMLElement | null = null;
    private enumeratedListItemTemplate: string = "";
    private enumeratedValueElement: HTMLElement | null = null;

    getValueControlElement(): HTMLElement | null {
        if (!this.frameElement) {
            return null;
        }
        return this.frameElement.querySelector('[mod-role=input-parameter-value]') as HTMLElement | null;
    }

    async handleValueClick(event: MouseEvent) {
        event.preventDefault();
        event.stopPropagation();


        this.props.handleFileSelect(
            this.props.instanceId,
            this.props.propertyUri,
            this.pathValue || ""
        );
    }

    attach(frameElement: HTMLElement) {

        this.frameElement = frameElement;
        this.requestUpdate();
        this.frameElement.onclick = (event: MouseEvent) => { this.handleClick(event); }

        // swipe the enumeraation-option generated by the template. 
        // We'll use it as a template to create new file entries.
        const enumeratedList = this.frameElement.querySelector(".mod-enumerated-list");
        if (enumeratedList) {
            this.enumeratedListContainerElement = enumeratedList as HTMLElement;
            if (this.enumeratedListContainerElement.children.length === 1) {
                const firstChild = this.enumeratedListContainerElement.children[0];
                if (firstChild) {
                    this.enumeratedListItemTemplate = firstChild.outerHTML;
                    this.enumeratedListContainerElement.removeChild(firstChild);
                }
            }
        }
        this.enumeratedValueElement = this.getValueControlElement();

        this.isInlineList = this.enumeratedValueElement === null;
        if (this.enumeratedValueElement) {
            this.enumeratedValueElement.onclick = (event: MouseEvent) => {
                this.handleValueClick(event);
            }
        }



    }
};


interface BypassLightControlProps {
    instanceId: number;
    model: PiPedalModel;
};

function ModMessage(message: string) {
    return "Failed to generate UI. (" + message + ")";
}

class BypassLightControl implements ModGuiControl {

    private props: BypassLightControlProps;
    private frameElement?: HTMLElement;

    private value: number = -1.038327E-15
    private animationFrameId: number | null = null;

    constructor(props: BypassLightControlProps) {
        this.props = props;
    }


    requestUpdate() {
        if (!this.mounted) return;
        if (!this.frameElement) {
            return;
        }


        if (this.animationFrameId === null) {
            this.animationFrameId = window.requestAnimationFrame(() => {
                this.animationFrameId = null;
                this.updateImage();
            });
        }
    }
    cancelRequestUpdate() {
        const animationFrameId = this.animationFrameId;
        if (animationFrameId !== null) {
            window.cancelAnimationFrame(animationFrameId);
            this.animationFrameId = null;
        }
    }
    setControlValue(value: number) {
        if (this.value !== value) {
            this.value = value;
            this.requestUpdate();
        }
    }

    private updateImage() {
        if (!this.frameElement || !this.mounted) {
            return;
        }

        this.frameElement.classList.remove('on', 'off');
        this.frameElement.classList.add(
            this.value ?
                'on' : 'off'
        );

    }
    private mounted: boolean = false;

    private listenHandle: ListenHandle | null = null;
    onMounted() {
        this.mounted = true;
        this.listenHandle = this.props.model.addPedalboardItemEnabledChangeListener(
            this.props.instanceId,
            (instanceId: number, isEnabled: boolean) => {
                const bypass = isEnabled ? 1.0: 0.0;
                if (bypass !== this.value) {
                    this.setControlValue(bypass);
                }
            }
        );
        this.requestUpdate();
    }

    onUnmount() {
        if (this.listenHandle) {
            this.props.model.removePedalboardItemEnabledChangeListener(this.listenHandle);
            this.listenHandle = null;
        }
        this.mounted = false;
        this.cancelRequestUpdate();
    }


    render(): HTMLElement | null {
        return null;
    }
    attach(frameElement: HTMLElement) {

        this.frameElement = frameElement;
        this.requestUpdate();
    }
};


interface FilmstripControlProps {
    instanceId: number;
    pluginControl: UiControl;
    filmStrip: string;
    verticalStrip: boolean;

    onValueChanged: (instanceId: number, symbol: string, value: number) => void;
    monitorPort: (
        instanceId: number,
        symbol: string,
        interval: number,
        callback: (value: number) => void) => MonitorPortHandle;
    unmonitorPort: (handle: MonitorPortHandle) => void;
};

class FilmstripControl implements ModGuiControl {

    private props: FilmstripControlProps;
    private frameElement?: HTMLElement;

    private value: number = -1.038327E-15
    private pointerDownValue: number = -1.038327E-15;
    private isPointerDown: boolean = false;
    private animationFrameId: number | null = null;

    constructor(props: FilmstripControlProps) {
        this.props = props;
    }


    requestUpdate() {
        if (!this.mounted) return;
        if (!this.frameElement) {
            return;
        }


        if (this.animationFrameId === null) {
            this.animationFrameId = window.requestAnimationFrame(() => {
                this.animationFrameId = null;
                this.updateImagePosition();
            });
        }
    }
    cancelRequestUpdate() {
        const animationFrameId = this.animationFrameId;
        if (animationFrameId !== null) {
            window.cancelAnimationFrame(animationFrameId);
            this.animationFrameId = null;
        }
    }
    setControlValue(value: number) {
        if (this.value !== value) {
            this.value = value;
            this.requestUpdate();
        }
    }
    static urlRegex = /url\(["']?([^"')]+)["']?\)/i;
    getImageUrl() {
        if (!this.frameElement) {
            throw new Error("Logic  error: frameElement is not set.");
        }

        const bgImage = getComputedStyle(this.frameElement).backgroundImage;
        const urlMatch = bgImage.match(FilmstripControl.urlRegex);
        if (urlMatch && urlMatch.length > 1) {
            return urlMatch[1];
        }
        return null;
    }

    private currentImageUrl: string = "";

    private updateImagePosition() {
        if (!this.frameElement || !this.mounted) {
            return;
        }

        const rotationAttr = this.frameElement.getAttribute('mod-widget-rotation');
        if (rotationAttr && rotationAttr !== "") {
            const rotation = parseFloat(rotationAttr);
            if (isNaN(rotation)) {
                console.error("pipedal: Invalid mod-widget-rotation value: " + rotationAttr);
                return;
            }
            const range = this.props.pluginControl.valueToRange(this.value);
            this.frameElement.style.transform = `rotate(${rotation * range - rotation / 2}deg)`;
            return;

        }
        const imageUrl = this.getImageUrl();
        if (!imageUrl) {
            return;
        }
        if (imageUrl != this.currentImageUrl) {
            this.filmstripInfo = null;

            this.currentImageUrl = imageUrl;
            this.imgElement = new Image();
            const img = this.imgElement;
            img.onload = () => {
                if (!this.frameElement) {
                    return;
                }
                const computedStyle = window.getComputedStyle(this.frameElement);
                const strWidth = computedStyle.getPropertyValue('width') || "0";
                let frameWidth = parseFloat(strWidth);
                if (isNaN(frameWidth) || frameWidth <= 0) { frameWidth = 0; }

                const strHeight = computedStyle.getPropertyValue("height") || "0";
                let frameHeight = parseFloat(strHeight);
                if (isNaN(frameHeight) || frameHeight <= 0) { frameHeight = 0; }

                let iHeight = 0;
                const bgSize = computedStyle.getPropertyValue('background-size');
                if (bgSize) {
                    const bgSizeParts = bgSize.split(' ');
                    if (bgSizeParts.length === 2) {
                        iHeight = parseFloat(bgSizeParts[1]);
                    }
                }
                if (iHeight === 0) {
                    iHeight = frameHeight;
                }
                const imgFrameWidth = Math.round(frameWidth / iHeight * img.naturalHeight);
                const nFrames = Math.round(img.naturalWidth / imgFrameWidth);

                this.filmstripInfo = {
                    width: img.naturalWidth,
                    height: img.naturalHeight,
                    frameWidth: frameWidth,
                    frameHeight: frameHeight,
                    nFrames: nFrames
                };
                this.requestUpdate();
            };
            img.src = imageUrl;
            return;
        }
        if (!this.filmstripInfo) {
            return; // Not loaded yet
        }


        let xOffset = 0;
        let yOffset = 0;
        const pluginControl = this.props.pluginControl;

        let value = this.isPointerDown ? this.pointerDownValue : this.value;
        value = this.props.pluginControl.clampValue(value);

        const isHorizontalStrip =
            this.filmstripInfo.frameWidth / this.filmstripInfo.frameHeight
            < this.filmstripInfo.width / this.filmstripInfo.height;
        if (isHorizontalStrip) {
            let range = this.props.pluginControl.valueToRange(value);

            if (range < 0) {
                range = 0;
            }
            if (range > 1) {
                range = 1;
            }
            const nFrame = Math.round(range * (this.filmstripInfo.nFrames - 1));
            xOffset = -nFrame * this.filmstripInfo.frameWidth;
            yOffset = 0;

        } else {
            xOffset = 0;
            if (this.value <= (pluginControl.min_value + pluginControl.max_value) / 2) {
                yOffset = 0; // Off position
            } else {
                yOffset = -this.frameElement.clientHeight; // On position
            }
        }
        this.frameElement.style.backgroundPosition = `${xOffset}px ${yOffset}px`;
    }
    private mounted: boolean = false;

    private monitorHandle: MonitorPortHandle | null = null;
    onMounted() {
        this.mounted = true;
        this.monitorHandle = this.props.monitorPort(
            this.props.instanceId,
            this.props.pluginControl.symbol,
            1.0 / 15,
            (value: number) => {
                if (value != this.value) {
                    this.setControlValue(value);
                }
            }
        );
        this.requestUpdate();
    }

    onUnmount() {
        if (this.monitorHandle) {
            this.props.unmonitorPort(this.monitorHandle);
            this.monitorHandle = null;
        }
        this.mounted = false;
        this.cancelRequestUpdate();
    }

    private filmstripInfo: {
        width: number,
        height: number,
        frameWidth: number,
        frameHeight: number,
        nFrames: number
    } | null = null;

    private pointerIds: number[] = [];
    private firstPointerId: number | null = null;
    lastX: number = 0;
    lastY: number = 0;

    handleToggleClick(event: PointerEvent) {
        event.preventDefault();
        event.stopPropagation();
        this.firstPointerId = null;
        this.isPointerDown = false;
        if (!this.frameElement) {
            return; // Not mounted yet
        }
        if (event.pointerType === "mouse" && event.button !== 0) {
            return; // Only left mouse button
        }
        let newValue: number;
        if (this.value === this.props.pluginControl.min_value) {
            newValue = this.props.pluginControl.max_value;
        } else {
            newValue = this.props.pluginControl.min_value;
        }
        this.setControlValue(newValue);
        this.props.onValueChanged(this.props.instanceId, this.props.pluginControl.symbol, newValue);
    }

    handlePointerDown(event: PointerEvent) {
        if (!this.frameElement) {
            return; // Not mounted yet
        }
        if (event.pointerType === "mouse" && event.button !== 0) {
            return; // Only left mouse button
        }
        if (this.props.pluginControl.controlType === ControlType.BypassLight) {
            return;
        }
        this.frameElement.setPointerCapture(event.pointerId);
        if (this.pointerIds.length === 0) {
            this.firstPointerId = event.pointerId;
            this.lastX = event.pageX;
            this.lastY = event.pageY;
        }
        this.isPointerDown = true;
        this.pointerIds.push(event.pointerId);

        if (this.props.pluginControl.controlType === ControlType.OnOffSwitch ||
            this.props.pluginControl.controlType === ControlType.ABSwitch) {
            this.handleToggleClick(event);
            return;
        }

        event.preventDefault();
        event.stopPropagation();
        this.pointerDownValue = this.value;
    }

    handlePointerMove(event: PointerEvent) {
        if (!this.frameElement) {
            return; // Not mounted yet
        }

        if (this.firstPointerId === null || !this.frameElement.hasPointerCapture(event.pointerId)) {
            return; // Not the first pointer or not captured
        }
        event.preventDefault();
        event.stopPropagation();
        const dy = event.pageY - this.lastY;
        this.lastY = event.pageY;
        this.lastX = event.pageX;

        let rate = RANGE_SCALE; // pixels for full range.
        if (this.pointerIds.length == 2) {
            rate = FINE_RANGE_SCALE;
        } else if (this.pointerIds.length > 2) {
            rate = ULTRA_FINE_RANGE_SCALE;
        } else if (event.ctrlKey) {
            rate = ULTRA_FINE_RANGE_SCALE;
        } else if (event.shiftKey) {
            rate = FINE_RANGE_SCALE;
        }

        const dRange = -dy / rate;

        let range = this.props.pluginControl.valueToRange(this.pointerDownValue);
        range += dRange;
        if (range < 0) range = 0;
        if (range > 1) range = 1;
        const newValue = this.props.pluginControl.rangeToValue(range);
        this.pointerDownValue = newValue;
        this.setControlValue(newValue);
        this.props.onValueChanged(this.props.instanceId, this.props.pluginControl.symbol, this.props.pluginControl.clampValue(newValue));
    }
    handlePointerUp(event: PointerEvent) {
        const index = this.pointerIds.indexOf(event.pointerId);
        if (index >= 0) {
            this.pointerIds.splice(index, 1);
        } else {
            return;
        }
        this.isPointerDown = false;
        this.requestUpdate();
        if (event.pointerId === this.firstPointerId) {
            this.firstPointerId = null;
        }
        if (!this.frameElement) {
            return; // Not mounted yet
        }

        this.frameElement.releasePointerCapture(event.pointerId);
        event.preventDefault();
        event.stopPropagation();
    }
    handlePointerCancel(event: PointerEvent) {
        if (!this.frameElement) {
            return; // Not mounted yet
        }
        const index = this.pointerIds.indexOf(event.pointerId);
        if (index >= 0) {
            this.pointerIds.splice(index, 1);
        }
        if (event.pointerId === this.firstPointerId) {
            this.firstPointerId = null;
        }
        if (this.pointerIds.length === 0) {
            this.firstPointerId = null;
            this.lastY = 0;
            this.isPointerDown = false;
            this.requestUpdate();

        }
    }
    handleMouseWheel(event: WheelEvent) {
        if (!this.frameElement) {
            return; // Not mounted yet
        }
        event.preventDefault();
        event.stopPropagation();
        let rate = WHEEL_RANGE_SCALE; // pixels for full range.
        if (event.ctrlKey) {
            rate = ULTRA_FINE_WHEEL_RANGE_SCALE;
        } else if (event.shiftKey) {
            rate = FINE_WHEEL_RANGE_SCALE;
        }
        const dRange = event.deltaY / rate;

        let range = this.props.pluginControl.valueToRange(this.value);
        range += dRange;
        if (range < 0) range = 0;
        if (range > 1) range = 1;
        const newValue = this.props.pluginControl.rangeToValue(range);
        this.setControlValue(newValue);
        this.props.onValueChanged(this.props.instanceId, this.props.pluginControl.symbol, newValue);
    }


    private imgElement: HTMLImageElement | null = null;
    render(): HTMLElement | null {
        return null;
    }
    attach(frameElement: HTMLElement) {

        this.frameElement = frameElement;
        this.frameElement.onpointerdown = (event: PointerEvent) => {
            this.handlePointerDown(event);
        };
        this.frameElement.onpointerup = (event: PointerEvent) => {
            if (!this.frameElement) {
                return; // Not mounted yet
            }
            this.handlePointerUp(event);
        };
        this.frameElement.onpointermove = (event: PointerEvent) => {

            if (!this.frameElement) {
                return; // Not mounted yet
            }
            if (this.frameElement.hasPointerCapture(event.pointerId)) {
                this.handlePointerMove(event);
                return;
            }
        }
        this.frameElement.onpointercancel = (event: PointerEvent) => {
            this.handlePointerCancel(event);
        }
        this.frameElement.onwheel = (event: WheelEvent) => {
            this.handleMouseWheel(event);
        };
        this.requestUpdate();
    }
};


// mod-role="input-control-value" elements outside a custom-select (which keeps its own):
// show the port's value, formatted, or its scale point label, as mod-ui does.
class ValueFieldsControl implements ModGuiControl {
    private model: PiPedalModel;
    private instanceId: number;
    private fields: { control: UiControl, element: HTMLElement }[] = [];
    private pedalboardHandler = (pedalboard: Pedalboard) => { this.update(pedalboard); };

    constructor(model: PiPedalModel, instanceId: number, plugin: UiPlugin, frameElement: Element) {
        this.model = model;
        this.instanceId = instanceId;
        frameElement.querySelectorAll('[mod-role=input-control-value][mod-port-symbol]')
            .forEach((element) => {
                if (element.closest('[mod-widget=custom-select]')) {
                    return;
                }
                const control = plugin.getControl(element.getAttribute("mod-port-symbol") ?? "");
                if (control && control.is_input) {
                    this.fields.push({ control: control, element: element as HTMLElement });
                }
            });
    }
    get isEmpty(): boolean { return this.fields.length === 0; }

    private update(pedalboard: Pedalboard) {
        const item = pedalboard.tryGetItem(this.instanceId);
        if (!item) return;
        for (const field of this.fields) {
            const text = field.control.formatDisplayValue(item.getControlValue(field.control.symbol));
            if (field.element.textContent !== text) {
                field.element.textContent = text;
            }
        }
    }
    onMounted() {
        this.model.pedalboard.addOnChangedHandler(this.pedalboardHandler);
        this.update(this.model.pedalboard.get());
    }
    onUnmount() {
        this.model.pedalboard.removeOnChangedHandler(this.pedalboardHandler);
    }
    render(): HTMLElement | null {
        return null;
    }
}

// jQuery is only needed by ModGUIs that have a modgui:javascript, so it's loaded on
// first use, in its own chunk. The npm module doesn't set window.$; scripts get $ and
// jQuery as parameters (see compileModGuiScript).
let jqueryPromise: Promise<JQueryStatic> | null = null;
function loadJQuery(): Promise<JQueryStatic> {
    if (!jqueryPromise) {
        jqueryPromise = import('jquery').then((module) => module.default);
        jqueryPromise.catch(() => { jqueryPromise = null; });
    }
    return jqueryPromise;
}

// Compiled modgui:javascript callbacks, by URL (which includes the plugin's version):
// like mod-ui, each script is evaluated once, however many instances are shown.
const modGuiScripts = new Map<string, Promise<ModGuiScriptCallback | null>>();

function loadModGuiScript(url: string): Promise<ModGuiScriptCallback | null> {
    let result = modGuiScripts.get(url);
    if (!result) {
        result = (async () => {
            try {
                const [response, jquery] = await Promise.all([fetch(url), loadJQuery()]);
                if (!response.ok) {
                    console.warn("pipedal: Failed to load ModGUI javascript " + url + ": " + response.statusText);
                    modGuiScripts.delete(url);
                    return null;
                }
                return compileModGuiScript(await response.text(), jquery);
            } catch (error) {
                console.warn("pipedal: Failed to load ModGUI javascript " + url, error);
                modGuiScripts.delete(url); // e.g. a network error: try again next time.
                return null;
            }
        })();
        modGuiScripts.set(url, result);
    }
    return result;
}

function ModGuiHost(props: ModGuiHostProps) {
    const model: PiPedalModel = PiPedalModelFactory.getInstance();
    const { plugin, onClose } = props;
    const [hostDivRef, setHostDivRef] = React.useState<HTMLDivElement | null>(null);
    const [errorMessage, setErrorMessage] = React.useState<string | null>(null);
    const [contentReady, setContentReady] = React.useState<boolean|null>(null);
    const [modGuiControls] = React.useState<ModGuiControl[]>([]);
    const [ready, setReady] = React.useState<boolean>(model.state.get() === State.Ready);
    const [maximizedUi] = React.useState<boolean>(false);


    function updateContentReady(value: boolean) {
        if (value !== contentReady) {
            setContentReady(value);
            props.onContentReady(value);
        }
    }

    
    function addPortClass(element: Element, selector: string, className: string) {
        const children = element.querySelectorAll(selector);
        // call addClass to each element that matches the selector
        children.forEach((child) => {
            child.classList.add(className);
        });
    }

    function createCustomSelectControl(control: Element, symbol: string) {
        const customSelectControl = new CustomSelectControl(
            {
                instanceId: props.instanceId,
                symbol: symbol,
                onValueChanged: (instanceId: number, symbol: string, value: number) => {
                    try {
                        model.setPedalboardControl(instanceId, symbol, value);
                    } catch (error) {
                        console.warn("pipedal: " + String(error));
                    }
                },
                monitorPort: model.monitorPort.bind(model),
                unmonitorPort: model.unmonitorPort.bind(model)
            }
        );
        customSelectControl.attach(control as HTMLElement);
        modGuiControls.push(customSelectControl);
        const modGuiElement = customSelectControl.render();
        if (modGuiElement !== null) {
            control.appendChild(modGuiElement);
        }
        customSelectControl.onMounted();
    }
    function monitorBypassPort(
        instanceId: number,
        symbol: string,
        interval: number,
        callback: (value: number) => void): MonitorPortHandle {
        return model.addPedalboardItemEnabledChangeListener(
            instanceId, (instanceId,value) => {
                callback(value ? 1 : 0);
            }
        );  
    }
    function unmonitorBypassPort(handle: MonitorPortHandle) {
        model.removePedalboardItemEnabledChangeListener(handle as ListenHandle);
    }
    function createFootswitchControl(control: Element, symbol: string, controlType: ControlType) {
        const uiControl = new UiControl();
        uiControl.symbol = symbol;
        uiControl.controlType = controlType;
        uiControl.min_value = 0;
        uiControl.max_value = 1;
        uiControl.is_logarithmic = false;
        uiControl.integer_property = true;

        const filmstripControl = new FilmstripControl(
            {
                instanceId: props.instanceId,
                pluginControl: uiControl,
                filmStrip: "/img/footswitch_strip.png",
                verticalStrip: true,
                onValueChanged: (instanceId: number, symbol: string, value: number) => {
                    if (symbol === "_bypass") {
                        model.setPedalboardItemEnabled(instanceId, value !== 0);
                    } else {
                        try {
                            model.setPedalboardControl(instanceId, symbol, value);
                        } catch (error) {
                            console.warn("pipedal: " + String(error));
                        }
                    }
                },
                monitorPort: 
                    symbol === "_bypass" ?
                        monitorBypassPort :
                        model.monitorPort.bind(model),
                unmonitorPort: 
                    symbol == "_bypass" ?
                        unmonitorBypassPort : 
                        model.unmonitorPort.bind(model)
            }
        );
        filmstripControl.attach(control as HTMLElement);
        modGuiControls.push(filmstripControl);
        const modGuiElement = filmstripControl.render();
        if (modGuiElement !== null) {
            control.appendChild(modGuiElement);
        }
        filmstripControl.onMounted();
    }
    function createCustomSelectPathControl(control: Element) {
        const pathUri = control.getAttribute("mod-parameter-uri");
        if (!pathUri) {
            setModError("No mod-parameter-uri attribute found on control element.");
            return;
        }
        const selectPathControl = new CustomSelectPathControl({
            instanceId: props.instanceId,
            propertyUri: pathUri,
            plugin: props.plugin,
            hostSite: model,
            handleFileSelect: props.handleFileSelect
        });

        selectPathControl.attach(control as HTMLElement);
        modGuiControls.push(selectPathControl);
        selectPathControl.onMounted();
    }


    function createBypassLightControl(control: Element) {

        const bypassLightControl = new BypassLightControl(
            {
                instanceId: props.instanceId,
                model: model
            }
        );
        bypassLightControl.attach(control as HTMLElement);
        modGuiControls.push(bypassLightControl);
        const modGuiElement = bypassLightControl.render();
        if (modGuiElement !== null) {
            control.appendChild(modGuiElement);
        }
        bypassLightControl.onMounted();
    }
    function createDialControl(control: Element, pluginControl: UiControl) {
        const modGui = plugin.modGui;
        if (!modGui) {
            alert("Logic error");
            return;
        }
        let knobUrl: string = "/img/BlackKnob.png";
        if (modGui.knob !== "") {
            knobUrl = modGui.knob;
        }
        const modGuiControl = new FilmstripControl(
            {
                instanceId: props.instanceId,
                pluginControl: pluginControl,
                filmStrip: knobUrl,
                verticalStrip: false,
                onValueChanged: (instanceId: number, symbol: string, value: number) => {
                    try {
                        model.setPedalboardControl(instanceId, symbol, value);
                    } catch (error) {
                        console.warn("pipedal: " + String(error));
                    }
                },
                monitorPort: model.monitorPort.bind(model),
                unmonitorPort: model.unmonitorPort.bind(model)

            }
        );
        modGuiControls.push(modGuiControl);
        modGuiControl.attach(control as HTMLElement);

        const modGuiElement = modGuiControl.render();
        if (modGuiElement !== null) {
            control.appendChild(modGuiElement);
        }
        modGuiControl.onMounted();

    }
    function prepareElement(element: Element) {
        addPortClass(element, '[mod-role="input-audio-port"]', "mod-audio-input");
        addPortClass(element, '[mod-role="output-audio-port"]', "mod-audio-output");
        // addPortClass(element,'[mod-role="input-midi-port"]', "mod-midi-input");
        // addPortClass(element,'[mod-role="output-midi-port"]', "mod-midi-output");
        // addPortClass(element,'[mod-role="input-cv-port"]', "mod-cv-input");
        // addPortClass(element,'[mod-role="output-cv-port"]', "mod-cv-output");


        element.querySelectorAll('[mod-role=input-control-port]')
            .forEach((control) => {
                const symbol = control.getAttribute("mod-port-symbol");

                if (symbol) {
                    const pluginControl: UiControl | undefined = plugin.getControl(symbol);
                    if (!pluginControl) {
                        setModError(`No plugin info found for symbol ${symbol}`);
                        return;
                    }
                    const widgetType = control.getAttribute("mod-widget") || "";
                    // 'film': 'film',
                    // 'switch': 'switchWidget',
                    // 'bypass': 'bypassWidget',
                    // 'select': 'selectWidget',
                    // 'string': 'stringWidget',
                    // 'custom-select': 'customSelect',
                    // 'custom-select-path': 'customSelectPath',

                    switch (widgetType) {
                        case "":
                        case "film":
                            createDialControl(control, pluginControl);
                            break;
                        case "switch":
                            createFootswitchControl(control, symbol, ControlType.OnOffSwitch);
                            break;
                        case "custom-select":
                            createCustomSelectControl(control, symbol);
                            break;
                        case "custom-select-path":
                        case "bypass":
                        case "string":
                            setModError("Unsupported widget type: " + widgetType);
                            break;
                    }
                } else {
                    setModError("No mod-symbol attribute found on control element.");
                }
            });
        element.querySelectorAll('[mod-role=bypass]')
            .forEach((control) => {
                const symbol = "_bypass";
                const controlType = ControlType.OnOffSwitch;
                createFootswitchControl(control, symbol, controlType);
            });
        element.querySelectorAll('[mod-role=bypass-light]')
            .forEach((control) => {
                createBypassLightControl(control);
            });
        element.querySelectorAll('[mod-role=input-parameter]')
            .forEach((control) => {
                // stub: mod-widget="custom-select-path".  Are there other widgets for this?
                createCustomSelectPathControl(control);
            });
        const valueFields = new ValueFieldsControl(model, props.instanceId, plugin, element);
        if (!valueFields.isEmpty) {
            modGuiControls.push(valueFields);
            valueFields.onMounted();
        }
    }

    // Run the ModGUI's modgui:javascript against the rendered icon (see ModGuiScript.ts).
    function bindModGuiScript(
        element: HTMLElement, callback: ModGuiScriptCallback, jquery: JQueryStatic,
        resourceUrl: string, queryParams: string) {
        const instanceId = props.instanceId;
        const ports: ModGuiScriptPort[] = plugin.controls
            .filter((control) => control.symbol !== "")
            .map((control) => ({
                symbol: control.symbol,
                index: control.index,
                isInput: control.is_input,
                minValue: control.min_value,
                maxValue: control.max_value
            }));
        const inputSymbols = new Set<string>(ports.filter((port) => port.isInput).map((port) => port.symbol));

        const scriptHost: ModGuiScriptHost = new ModGuiScriptHost(callback, {
            ports: ports,
            setPortValue: (symbol: string, value: number) => {
                // The same path as a widget's change.
                if (symbol === BYPASS_SYMBOL) {
                    model.setPedalboardItemEnabled(instanceId, value === 0);
                } else {
                    model.setPedalboardControl(instanceId, symbol, value);
                }
            },
            patchGet: (uri: string) => {
                // As in mod-ui, the reply reaches the script through the patch monitor
                // below, not from here (which would deliver it twice).
                model.getPatchProperty(instanceId, uri)
                    .catch((error) => { console.warn("pipedal: ModGUI javascript: patch_get failed. " + String(error)); });
            },
            patchSet: (uri: string, atomJson: unknown) => {
                model.setPatchProperty(instanceId, uri, atomJson)
                    .catch((error) => { console.warn("pipedal: ModGUI javascript: patch_set failed. " + String(error)); });
            },
            customResourceUrl: (filename: string) => {
                const path = filename.split('/').filter((segment) => segment !== "").map(encodeURIComponent).join('/');
                return resourceUrl + path + queryParams;
            }
        }, jquery(element));

        // Control port changes from widgets, the script and MIDI, synchronously.
        const controlValueHandle = model.addControlValueChangeListener(instanceId, (key: string, value: number) => {
            if (inputSymbols.has(key)) {
                scriptHost.portChanged(key, value);
            }
        });
        // Bypass, and control values replaced wholesale with the pedalboard (plugin
        // presets, snapshots, pedalboard loads), which the listener above doesn't see.
        // The script host passes on actual changes only, so nothing is sent twice.
        const pedalboardHandler = (pedalboard: Pedalboard) => {
            const item = pedalboard.tryGetItem(instanceId);
            if (!item) return;
            scriptHost.portChanged(BYPASS_SYMBOL, item.isEnabled ? 0 : 1);
            for (const controlValue of item.controlValues) {
                if (inputSymbols.has(controlValue.key)) {
                    scriptHost.portChanged(controlValue.key, controlValue.value);
                }
            }
        };

        const item = model.pedalboard.get().tryGetItem(instanceId);
        const startPorts: { symbol: string, value: number }[] = [
            { symbol: BYPASS_SYMBOL, value: item && !item.isEnabled ? 1 : 0 }
        ];
        for (const control of plugin.controls) {
            if (control.is_input && control.symbol !== "") {
                const value = item ? item.getControlValue(control.symbol) : control.default_value;
                startPorts.push({ symbol: control.symbol, value: value });
            }
        }
        const parameters: { uri: string, value: unknown }[] = [];
        const seen = new Set<string>();
        if (item) {
            for (const uri of Object.keys(item.pathProperties)) {
                try {
                    parameters.push({ uri: uri, value: decodePatchValue(JSON.parse(item.pathProperties[uri])) });
                    seen.add(uri);
                } catch {
                    // not JSON: leave it out.
                }
            }
        }
        for (const patchProperty of plugin.patchProperties) {
            if (!seen.has(patchProperty.uri)) {
                // Other properties' current values aren't known here; mod-ui also starts with the default.
                parameters.push({ uri: patchProperty.uri, value: patchProperty.isNumeric() ? patchProperty.defaultValue : "" });
            }
        }

        scriptHost.start(startPorts, parameters);

        model.pedalboard.addOnChangedHandler(pedalboardHandler);
        const outputMonitors: MonitorPortHandle[] = [];
        for (const control of plugin.controls) {
            if (!control.is_input && control.symbol !== "") {
                const symbol = control.symbol;
                outputMonitors.push(model.monitorPort(instanceId, symbol, 1.0 / 15, (value: number) => {
                    scriptHost.portChanged(symbol, value);
                }));
            }
        }
        // "": every patch:Set the plugin sends.
        const patchListenHandle = model.monitorPatchProperty(instanceId, "", (_instanceId, uri, value) => {
            scriptHost.parameterChanged(uri, decodePatchValue(value));
        });

        modGuiControls.push({
            onMounted: () => { },
            onUnmount: () => {
                scriptHost.dispose();
                model.removeControlValueChangeListener(controlValueHandle);
                model.pedalboard.removeOnChangedHandler(pedalboardHandler);
                for (const handle of outputMonitors) {
                    model.unmonitorPort(handle);
                }
                model.cancelMonitorPatchProperty(patchListenHandle);
                try {
                    // Drops the jQuery data and event handlers the script attached.
                    jquery(element).remove();
                } catch (error) {
                    console.warn("pipedal: ModGUI javascript: cleanup failed.", error);
                }
            },
            render: () => null
        });
    }

    function setModError(message: string) {
        setErrorMessage(ModMessage(message));
    }
    async function requestContent(cancelled: { value: boolean }) {
        updateContentReady(false);
        try {
            const modGui = plugin.modGui;
            if (!modGui) {
                setModError("No MOD GUI declared for this plugin.");
                return;
            }
            if (!modGui.iconTemplate) {
                setModError("Template file missing.");
                return;
            }
            if (!modGui.stylesheet) {
                setModError("Stylesheet file missing.");
                return;
            }

            const encodedUri = encodeURIComponent(props.plugin.uri);
            const version = props.plugin.minorVersion * 1000 + props.plugin.microVersion;

            const resourceUrl = model.modResourcesUrl;
            const queryParams = "?ns=" + encodedUri + "&v=" + version.toString();
            const templateUri = resourceUrl + "_/iconTemplate" + queryParams;
            const cssUri = resourceUrl + "_/stylesheet" + queryParams;

            // Loads concurrently with the template. Never rejects.
            const scriptPromise: Promise<ModGuiScriptCallback | null> | null = modGui.javascript
                ? loadModGuiScript(resourceUrl + "_/javascript" + queryParams)
                : null;

            const fetchResult = await fetch(templateUri);
            if (!fetchResult.ok) {
                setModError("Failed to load template: " + fetchResult.statusText);
                return;
            }
            const parser = new DOMParser();
            const docText = await fetchResult.text();
            const doc = parser.parseFromString(docText, "text/html");
            const element = doc.body.firstElementChild;
            if (!element) {
                setModError("No root element found in template.");
                return;
            }
            const cssResult = await fetch(cssUri);
            if (!cssResult.ok) {
                setModError("Failed to load stylesheet: " + cssResult.statusText);
                return;
            }
            if (hostDivRef === null || cancelled.value) {
                return; // cancelled.
            }

            const t = await cssResult.text();
            const cssText = "<style id='mod-gui-style'>" + t + "</style>";
            const cssDoc = parser.parseFromString(cssText, "text/html");
            const cssElement = cssDoc.getElementById('mod-gui-style');
            if (!cssElement) {
                setModError("No style element found.");
                return;
            }
            let scriptCallback: ModGuiScriptCallback | null = null;
            let jquery: JQueryStatic | null = null;
            if (scriptPromise) {
                scriptCallback = await scriptPromise;
                if (scriptCallback) {
                    jquery = await loadJQuery();
                }
            }
            if (hostDivRef === null || cancelled.value) {
                return; // cancelled.
            }
            prepareElement(element);
            // add element to the hostDivRef
            hostDivRef.appendChild(cssElement);
            hostDivRef.appendChild(element);
            if (scriptCallback && jquery) {
                // After the controls are bound, and before measuring: the script may
                // change the layout (e.g. select a tab).
                bindModGuiScript(element as HTMLElement, scriptCallback, jquery, resourceUrl, queryParams);
            }
            const width = element.clientWidth;
            const height = element.clientHeight;
            hostDivRef.style.width = width + "px";
            hostDivRef.style.height = height + "px";
            updateContentReady(true);
        } catch (error) {
            setModError("Error loading UI: " + (error instanceof Error ? error.message : String(error)));
        }

    }

    React.useEffect(() => {
        const tHostDivRef = hostDivRef;
        const cancelled = { value: false };
        if (ready && plugin.modGui) {
            if (hostDivRef !== null) {
                requestContent(cancelled);
            }
        }
        const stateHandler = (state: State) => {
            setReady(state === State.Ready);
        }
        model.state.addOnChangedHandler(stateHandler)


        return () => {
            cancelled.value = true;
            model.state.removeOnChangedHandler(stateHandler);
            
            if (tHostDivRef !== null) {
                // unmount the custom content.
                const children = tHostDivRef.children;
                for (let i = children.length - 1; i >= 0; i--) {
                    const child = children[i];
                    if (child instanceof HTMLElement) {
                        child.remove();
                    }
                }
            }
            updateContentReady(false);
            const mc = modGuiControls;
            for (let i = 0; i < mc.length; i++) {
                const modGuiControl = mc[i];
                modGuiControl.onUnmount();
            }
            mc.length = 0; // Clear the controls array
        };
    }, [hostDivRef, plugin, ready]);

    // After the hooks, which must run on every render.
    if (!plugin.modGui) {
        return (
            <div>
                <Typography variant="h6">No Mod GUI</Typography>
            </div>
        );
    }


    return (
        <ModGuiErrorBoundary plugin={props.plugin} onClose={() => { props.onClose(); setErrorMessage(null); }}>
            <div 
            style={{
                display: "inline-block",
                paddingLeft: 20, paddingRight: 20,
                overflow: "hidden",
            }}
                onClick={(event) => {
                    // Prevent click events from propagating to the parent element.
                    event.stopPropagation();
                }}
            >
                {maximizedUi && (
                    <IconButtonEx tooltip="Close" onClick={() => onClose()} style={{
                        position: "absolute", top: 16, right: 16,
                        background: "#FFF5", zIndex: 600
                    }}>
                        <CloseIcon />
                    </IconButtonEx>
                )}

                <div style={{ display: "block", position: "relative" }}>
                    <div ref={setHostDivRef} style={{ display: "block", position: "relative" }}
                        onClick={(event) => {
                            // Prevent click events from propagating to the parent element.
                            event.stopPropagation();
                        }
                        }
                    />
                </div>

            </div>
            {errorMessage !== null && (
                <OkDialog title="Error" open={errorMessage !== null}
                    onClose={() => {
                        setErrorMessage(null);
                        onClose();
                    }}
                    text={errorMessage ?? ""}
                />
            )}

        </ModGuiErrorBoundary>
    );
}

interface ModGuiPluginPreference {
    pluginUri: string;
    useModGui: boolean;
}

let modGuiPluginPreferences: ModGuiPluginPreference[] | null = null;


function loadModGuiPreferences() {
    if (modGuiPluginPreferences === null) {
        const prefs = window.localStorage.getItem("modGuiPluginPreferences");
        try {
            if (prefs) {
                modGuiPluginPreferences = JSON.parse(prefs);
            } else {
                modGuiPluginPreferences = [];
            }       
        } catch (error) {
            console.error("pipedal: Error parsing modGuiPluginPreferences from localStorage: " + String(error));
            modGuiPluginPreferences = [];
        }
    }
}

export function getDefaultModGuiPreference(pluginUri: string) {
    loadModGuiPreferences();
    if (modGuiPluginPreferences === null) {
        modGuiPluginPreferences = [];
    }
    if (modGuiPluginPreferences) {
        const preference = modGuiPluginPreferences.find(p => p.pluginUri === pluginUri);
        if (preference) {
            return preference.useModGui;
        }
    }
    return false;
}


export function setDefaultModGuiPreference(pluginUri: string, useModGui: boolean) {
    loadModGuiPreferences();
    if (modGuiPluginPreferences === null) {
        modGuiPluginPreferences = [];
    }

    const preference = modGuiPluginPreferences.find(p => p.pluginUri === pluginUri);
    if (preference) {
        preference.useModGui = useModGui;
    } else {
        modGuiPluginPreferences.push({ pluginUri: pluginUri, useModGui });
    }
    try {
        window.localStorage.setItem("modGuiPluginPreferences", JSON.stringify(modGuiPluginPreferences));
    } catch(error) {
        console.error("pipedal: Error saving modGuiPluginPreferences to localStorage: " + String(error));
    }
}   


export default ModGuiHost;