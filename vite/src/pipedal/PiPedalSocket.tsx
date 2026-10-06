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

import { PiPedalStateError } from './PiPedalError';

export type MessageHandler = (header: PiPedalMessageHeader, body: any | null) => void;
export type ErrorHandler = (message: string, exception?: Error) => void;
export type ReconnectHandler = () => void;
export type ReconnectingHandler = (retry: number) => void;

// Reconnect backoff: 250ms, 500ms, 1s, 2s, then 3s forever, each with +/-20% jitter.
const RETRY_DELAYS_MS = [250, 500, 1000, 2000, 3000];
const RETRY_JITTER = 0.2;

export const DISCONNECTED_MESSAGE = "Disconnected";

export function isDisconnectedError(error: any): boolean {
    if (error instanceof Error) {
        return error.message === DISCONNECTED_MESSAGE;
    }
    return error === DISCONNECTED_MESSAGE;
}

export type PiPedalMessageHeader = {
    replyTo?: number;
    reply?: number;
    message: string;
}
type ReplyHandler = (header: PiPedalMessageHeader, body: any | null) => void;
type Reservation = {
    handler: ReplyHandler;
    reject: (reason: any) => void;
};

// Idempotent "set a value" commands that are worth replaying after a reconnect.
// Only the latest message per key is kept. Returns undefined for anything else.
function idempotentKey(message: string, body: any): string | undefined {
    switch (message) {
        case "setControl":
            return message + "|" + body?.instanceId + "|" + body?.symbol;
        case "loadPreset":
        case "setSnapshot":
            // latest selection wins, regardless of target.
            return message + "||";
        default:
            return undefined;
    }
}

type PendingSend = {
    message: string;
    body?: any;
    queuedAt: number; // Date.now() when queued.
};

// Queued messages older than this are dropped at reconnect: a preset/snapshot tap or a control
// change that is replayed long after it was made would surprise the player.
export const MAX_PENDING_SEND_AGE_MS = 5000;

export interface PiPedalSocketListener {
    onMessageReceived: (header: PiPedalMessageHeader, body: any | null) => void;
    onError: (message: string, exception?: Error) => void;
    onConnectionLost: () => void;
    onReconnect: () => void;
    onReconnecting: (retry: number) => boolean;
};

class PiPedalSocket {

    listener: PiPedalSocketListener;


    socket?: WebSocket;
    nextResponseCode: number = 0;
    url: string;
    retrying: boolean = false;
    retryCount: number = 0;

    constructor(
        url: string,
        listener: PiPedalSocketListener
    ) {
        this.url = url;
        this.listener = listener;
        this.onOnline = this.onOnline.bind(this);
        this.onVisibilityChange = this.onVisibilityChange.bind(this);
        window.addEventListener("online", this.onOnline);
        document.addEventListener("visibilitychange", this.onVisibilityChange);
    }

    handleOpen(event: Event): any {

    }

    isConnected(): boolean {
        return !this.retrying && !this.isBackground
            && this.socket !== undefined && this.socket.readyState === WebSocket.OPEN;
    }

    private sendInternal_(json: string): boolean {
        if (!this.isConnected()) return false;
        this.socket!.send(json);
        return true;
    }

    private pendingSends: Map<string, PendingSend> = new Map<string, PendingSend>();

    send(message: string, jsonObject?: any) {
        let msg: any;
        if (jsonObject === undefined) {
            msg = [{ message: message }];
        } else {
            msg = [{ message: message }, jsonObject];
        }
        const json = JSON.stringify(msg);
        if (this.sendInternal_(json)) return;

        const key = idempotentKey(message, jsonObject);
        if (key !== undefined) {
            // keep only the latest; move it to the back of the queue.
            this.pendingSends.delete(key);
            this.pendingSends.set(key, { message: message, body: jsonObject, queuedAt: Date.now() });
        } else {
            console.warn("Not connected. Dropping message: " + message);
        }
    }

    // Send queued idempotent messages after a reconnect. Messages older than MAX_PENDING_SEND_AGE_MS
    // are dropped. Bodies that carry a clientId are re-addressed from the previous connection's client
    // id to clientId (the server ignores echoes to the sending client by id). onFlush is called for each
    // message sent so that the caller can reflect it in local state.
    flushPendingSends(clientId: number, onFlush?: (message: string, body: any) => void) {
        const pending = Array.from(this.pendingSends.values());
        this.pendingSends.clear();
        const now = Date.now();
        for (const item of pending) {
            if (now - item.queuedAt > MAX_PENDING_SEND_AGE_MS) {
                console.warn("Dropping stale queued message: " + item.message);
                continue;
            }
            let body = item.body;
            if (body !== null && typeof body === "object" && !Array.isArray(body) && "clientId" in body) {
                body = { ...body, clientId: clientId };
            }
            const json = JSON.stringify(body === undefined ? [{ message: item.message }] : [{ message: item.message }, body]);
            if (this.sendInternal_(json)) {
                onFlush?.(item.message, body);
            } else {
                console.warn("Not connected. Dropping message: " + item.message);
            }
        }
    }

    // Discard queued messages (e.g. the server state could not be reloaded after a reconnect).
    clearPendingSends() {
        this.pendingSends.clear();
    }

    reply(replyTo: number, message: string, jsonObject?: any) {
        if (replyTo !== -1) {
            let msg: any;
            if (jsonObject === undefined) {
                msg = [{ reply: replyTo, message: message }];
            } else {
                msg = [{ reply: replyTo, message: message }, jsonObject];
            }
            const json = JSON.stringify(msg);
            this.sendInternal_(json);
        }
    }

    _nextResponseCode(): number {
        return ++this.nextResponseCode;
    }

    _replyMap: Map<number, Reservation> = new Map<number, Reservation>();

    _discardReplyReservations() {
        const reservations = this._replyMap;
        this._replyMap = new Map<number, Reservation>();
        for (const reservation of reservations.values()) {
            reservation.reject(new Error(DISCONNECTED_MESSAGE));
        }
    }


    request<Type = any>(message_: string, requestArgs?: any): Promise<Type> {
        if (!this.isConnected()) {
            return Promise.reject(new Error(DISCONNECTED_MESSAGE));
        }
        const responseCode = this._nextResponseCode();

        return new Promise<Type>((resolve, reject) => {
            try {
                this._replyMap.set(responseCode, {
                    handler: (header: PiPedalMessageHeader, jsonObject?: any) => {
                        if (header.message === "error") {
                            reject(jsonObject + "");
                        } else {
                            resolve(jsonObject as Type);
                        }
                    },
                    reject: reject
                });
                let msg: any;
                if (requestArgs !== undefined) {
                    msg = [{ message: message_, replyTo: responseCode }, requestArgs];
                } else {
                    msg = [{ message: message_, replyTo: responseCode }];
                }
                const jsonMessage = JSON.stringify(msg);
                this.sendInternal_(jsonMessage);
            } catch (err) {
                this._replyMap.delete(responseCode);
                reject(err);
            }
        });
    }
    handleMessage(event: MessageEvent<string>): any {
        try {
            const message: any = JSON.parse(event.data);
            if (!Array.isArray(message)) {
                throw new PiPedalStateError("Invalid message received from server.");
            }
            const header = message[0] as PiPedalMessageHeader;
            let body = undefined;
            if (message.length === 2) {
                body = message[1];
            }
            if (header.reply !== undefined) {
                const reservation = this._replyMap.get(header.reply);
                if (reservation) {
                    this._replyMap.delete(header.reply);
                    reservation.handler(header, body);
                }
                return;
            } else {
                if (header.message === "error") {
                    throw new PiPedalStateError("Server error: " + body);
                }
                this.listener.onMessageReceived(header, body);
            }
        } catch (error) {
            if (this.listener) {
                this.listener.onError("Invalid server response. " + error, error as Error);
            } else {
                throw new PiPedalStateError("Invalid server response.");
            }
        }
    }

    handleError(_event: Event): any {
        // Once the socket has opened, onclose drives reconnection. A network blip
        // is not a fatal error.
        console.warn("WebSocket error (ignored; waiting for close).");
    }

    canReconnect: boolean = false;

    handleClose(_event: any): any {
        if (this.canReconnect) {
            this.socket = undefined;
            this.listener.onConnectionLost();
            this._reconnect();
        }
    }
    _reconnect() {
        this._discardReplyReservations();
        this.cancelRetry_();
        this.retrying = true;
        this.retryCount = 0;
        this.socket = undefined;

        this.reconnect();
    }

    isBackground: boolean = false;

    enterBackgroundState() {
        this.isBackground = true;
        this.cancelRetry_();
        this.close();
        this._discardReplyReservations();
    }
    exitBackgroundState() {
        this.isBackground = false;
        this._reconnect();
    }

    private retryTimer?: ReturnType<typeof setTimeout>;
    private attemptId: number = 0;
    private pendingWs?: WebSocket;

    private cancelRetry_() {
        if (this.retryTimer !== undefined) {
            clearTimeout(this.retryTimer);
            this.retryTimer = undefined;
        }
        // abandon any connection attempt in flight.
        ++this.attemptId;
        if (this.pendingWs) {
            const ws = this.pendingWs;
            this.pendingWs = undefined;
            ws.onopen = null; ws.onclose = null; ws.onerror = null; ws.onmessage = null;
            try { ws.close(); } catch (ignored) { }
        }
    }

    private nextRetryDelay_(): number {
        const base = RETRY_DELAYS_MS[Math.min(this.retryCount, RETRY_DELAYS_MS.length - 1)];
        const jitter = 1 + (Math.random() * 2 - 1) * RETRY_JITTER;
        return Math.round(base * jitter);
    }

    // Retry right now, skipping any backoff delay.
    retryNow() {
        if (!this.retrying || this.isBackground) return;
        this.cancelRetry_();
        this.reconnect();
    }

    private onOnline() {
        this.retryNow();
    }
    private onVisibilityChange() {
        // The model's exitBackgroundState owns the foreground-return reconnect. Only
        // shortcut a pending backoff delay; never start a second attempt while one is in flight.
        if (document.visibilityState === "visible" && this.pendingWs === undefined) {
            this.retryNow();
        }
    }

    // Detach the window/document listeners and abandon any connection; used when the socket is replaced.
    dispose() {
        window.removeEventListener("online", this.onOnline);
        document.removeEventListener("visibilitychange", this.onVisibilityChange);
        this.canReconnect = false;
        this.retrying = false;
        this.cancelRetry_();
        this.close();
        this._discardReplyReservations();
    }

    reconnect() {

        if (this.socket) {
            this.close();
        }

        if (!this.listener.onReconnecting(this.retryCount)) {
            return;
        }
        const delay = this.nextRetryDelay_();
        ++this.retryCount;
        const attempt = ++this.attemptId;

        this.connectInternal_((ws) => { this.pendingWs = ws; })
            .then((socket) => {
                if (attempt !== this.attemptId) {
                    // abandoned.
                    try { socket.onclose = null; socket.onerror = null; socket.onmessage = null; socket.close(); } catch (ignored) { }
                    return;
                }
                this.pendingWs = undefined;
                this.socket = socket;
                this.retrying = false;
                this.listener.onReconnect();
            })
            .catch(error => {
                if (attempt !== this.attemptId) return;
                this.pendingWs = undefined;
                this.retryTimer = setTimeout(() => {
                    this.retryTimer = undefined;
                    this.reconnect();
                }, delay);
            });
    }

    close(): void {
        try {
            if (this.socket) {
                this.socket.onclose = null;
                this.socket.onerror = null;
                this.socket.onmessage = null;
                this.socket.onopen = null;
                this.socket.close();
                this.socket = undefined;
            }
        } catch (ignored) {

        }
        this.socket = undefined;
    }
    connectInternal_(onCreate?: (ws: WebSocket) => void): Promise<WebSocket> {
        return new Promise<WebSocket>((resolve, reject) => {
            try {
                const ws = new WebSocket(this.url);
                onCreate?.(ws);

                const self = this;

                ws.onmessage = this.handleMessage.bind(this);
                ws.onclose = (event: Event) => {
                    ws.onclose = null;
                    ws.onerror = null;
                    reject("Connection closed unexpectedly.");
                };
                ws.onerror = (evt: Event) => {
                    ws.onclose = null;
                    ws.onerror = null;
                    reject("Connection not accepted.");
                };
                ws.onopen = (event: Event) => {
                    ws.onerror = self.handleError.bind(self);
                    ws.onclose = self.handleClose.bind(self);
                    ws.onopen = null;
                    resolve(ws);
                };
            } catch (e: any) {
                reject("Failed to connect: " + e.toString());
            };
        });
    }
    connect(): Promise<void> {
        return new Promise<void>((resolve, reject) => {

            this.connectInternal_()
                .then((socket) => {
                    this.socket = socket;
                    resolve();
                })
                .catch((reason) => {
                    reject(reason);
                });
        });
    }

}

export default PiPedalSocket;
