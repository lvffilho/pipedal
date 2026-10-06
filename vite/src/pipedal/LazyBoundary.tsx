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

import React from 'react';

// A lazy chunk can fail to load (server upgraded under an open tab, network drop). Vite reports
// this as "vite:preloadError". Reload once to pick up the new build, but only if we're online
// and the server answers: reloading while the server is unreachable would replace the app with a
// browser error page. Otherwise the error propagates to the retrying import (lazyWithRetry) and
// LazyBoundary. The sessionStorage guard prevents a reload loop.
const RELOAD_GUARD_KEY = "com.twoplay.pipedal.chunk_reload";
let reloadCheckInProgress = false;
window.addEventListener("vite:preloadError", (_event: Event) => {
    if (reloadCheckInProgress || !navigator.onLine) {
        return;
    }
    try {
        if (sessionStorage.getItem(RELOAD_GUARD_KEY) === "1") {
            return;
        }
    } catch (e) {
        return; // can't guard against loops; don't reload.
    }
    reloadCheckInProgress = true;
    fetch("/index.html", { method: "HEAD", cache: "no-store" })
        .then((response) => {
            if (!response.ok || !navigator.onLine) {
                return;
            }
            try {
                if (sessionStorage.getItem(RELOAD_GUARD_KEY) === "1") {
                    return;
                }
                sessionStorage.setItem(RELOAD_GUARD_KEY, "1");
            } catch (e) {
                return;
            }
            window.location.reload();
        })
        .catch(() => {
            // server unreachable: don't reload.
        })
        .finally(() => {
            reloadCheckInProgress = false;
        });
});

const RETRY_DELAYS_MS = [1000, 2000];

function delay(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

async function importWithRetry<T>(factory: () => Promise<T>): Promise<T> {
    for (let attempt = 0; ; ++attempt) {
        try {
            return await factory();
        } catch (e) {
            if (attempt >= RETRY_DELAYS_MS.length) {
                throw e;
            }
            await delay(RETRY_DELAYS_MS[attempt]);
        }
    }
}

// Chunk loaders registered by lazyWithRetry, for prefetchLazyChunks().
const lazyLoaders: (() => Promise<unknown>)[] = [];
let prefetchEnabled = false;

function runWhenIdle(fn: () => void) {
    const w = window as any;
    if (typeof w.requestIdleCallback === "function") {
        w.requestIdleCallback(fn, { timeout: 10000 });
    } else {
        setTimeout(fn, 2000);
    }
}

function prefetch(loader: () => Promise<unknown>) {
    runWhenIdle(() => {
        loader().catch(() => { /* loaded (or reported) again when actually used. */ });
    });
}

// Fetch the lazy chunks at idle time, so that dialogs still open if the network drops later.
// Chunks registered later (lazy components defined in lazy chunks) are prefetched as they register.
export function prefetchLazyChunks() {
    if (prefetchEnabled) return;
    prefetchEnabled = true;
    for (const loader of lazyLoaders) {
        prefetch(loader);
    }
}

// React.lazy() caches a failed load forever (the component can never render for the rest of the
// session). This retries the import a few times, and on final failure replaces the lazy component,
// so that the next time it is mounted (e.g. the dialog is opened again) the load is retried.
export function lazyWithRetry<P extends object>(
    factory: () => Promise<{ default: React.ComponentType<P> }>
): React.FC<P> {
    const makeLazy = (): React.LazyExoticComponent<React.ComponentType<P>> =>
        React.lazy(() => importWithRetry(factory).catch((e) => {
            current = makeLazy();
            throw e;
        }));
    let current = makeLazy();
    lazyLoaders.push(factory);
    if (prefetchEnabled) {
        prefetch(factory);
    }
    const LazyWithRetry: React.FC<P> = (props: P) => {
        const Component = current as React.ComponentType<P>;
        return <Component {...props} />;
    };
    return LazyWithRetry;
}

interface LazyBoundaryProps {
    children?: React.ReactNode;
    // Called if the component fails to load. Typically closes the dialog, so that opening it
    // again remounts the boundary and retries the load.
    onLoadFailed?: () => void;
}
interface LazyBoundaryState {
    failed: boolean;
}

// Suspense plus an error boundary for lazy-loaded dialogs, so a failed chunk load drops the
// dialog instead of unmounting the whole app.
export default class LazyBoundary extends React.Component<LazyBoundaryProps, LazyBoundaryState> {
    state: LazyBoundaryState = { failed: false };

    static getDerivedStateFromError(): LazyBoundaryState {
        return { failed: true };
    }
    componentDidCatch(error: unknown): void {
        console.error("Failed to load a UI component:", error);
        this.props.onLoadFailed?.();
    }
    render() {
        if (this.state.failed) {
            return null;
        }
        return (
            <React.Suspense fallback={null}>
                {this.props.children}
            </React.Suspense>
        );
    }
}
