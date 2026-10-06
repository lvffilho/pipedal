/**
 * TONE3000 catalog browsing, proxied by the PiPedal server (src/Tone3000Catalog.cpp), which
 * holds the TONE3000 session: the browser never sees the access token. Shapes match the
 * server's reduced reply classes; the server validates every parameter.
 */

import { PiPedalModel } from "../PiPedalModel";

export type CatalogKind = "search" | "favorited" | "downloaded";
export type CatalogSort = "" | "bestMatch" | "trending" | "popular" | "newest" | "oldest";
// TONE3000's gear values, plus "ir": any impulse response (the server sends it as format=ir).
// The server also still accepts the deprecated "full-rig" (= "amp-cab").
export type CatalogGear = "" | "amp" | "amp-cab" | "pedal" | "outboard" | "cab" | "space" | "experimental" | "ir";
export type CatalogFormat = "" | "nam" | "ir";

export const CATALOG_PAGE_SIZE = 25;
export const MODELS_PAGE_SIZE = 50;
export const MAX_QUERY_LENGTH = 200; // must agree with t3k_catalog::MAX_QUERY_LENGTH.


// Truncate to at most maxBytes of UTF-8 without splitting a code point (the server's limit is in bytes).
export function truncateUtf8(text: string, maxBytes: number = MAX_QUERY_LENGTH): string {
    const encoder = new TextEncoder();
    if (encoder.encode(text).length <= maxBytes) return text;
    let bytes = 0;
    let end = 0;
    for (const ch of text) { // iterates by code point
        const n = encoder.encode(ch).length;
        if (bytes + n > maxBytes) break;
        bytes += n;
        end += ch.length;
    }
    return text.substring(0, end);
}

export const GEAR_OPTIONS: { value: CatalogGear; label: string }[] = [
    { value: "amp", label: "Amp" },
    { value: "amp-cab", label: "Amp + cab" },
    { value: "pedal", label: "Pedal" },
    { value: "outboard", label: "Outboard" },
    { value: "cab", label: "Cab" },
    { value: "space", label: "Space" },
    { value: "experimental", label: "Experimental" },
    { value: "ir", label: "IR" },
];

/** Gear whose tones are impulse responses only (no calibrated captures). */
export function isIrOnlyGear(gear: CatalogGear): boolean {
    return gear === "ir" || gear === "cab" || gear === "space";
}

export const FORMAT_OPTIONS: { value: CatalogFormat; label: string }[] = [
    { value: "nam", label: "NAM" },
    { value: "ir", label: "IR" },
];

export const SORT_OPTIONS: { value: CatalogSort; label: string }[] = [
    { value: "bestMatch", label: "Best match" },
    { value: "trending", label: "Trending" },
    { value: "popular", label: "Popular" },
    { value: "newest", label: "Newest" },
    { value: "oldest", label: "Oldest" },
];

/** The API's own default: best match with search text, else trending. */
export function defaultSort(queryText: string): CatalogSort {
    return queryText.trim() !== "" ? "bestMatch" : "trending";
}

export interface CatalogQuery {
    kind: CatalogKind;
    query: string;
    sort: CatalogSort;
    gear: CatalogGear;
    format: CatalogFormat;
    tags: string[];
    makes: string[];
    creators: string[];
    calibrated: boolean;
    verified: boolean;
    architecture: number; // 0 = any.
    page: number;
    pageSize: number;
}

export function emptyCatalogQuery(): CatalogQuery {
    return {
        kind: "search", query: "", sort: "", gear: "", format: "", tags: [], makes: [], creators: [],
        calibrated: false, verified: false, architecture: 0, page: 1, pageSize: CATALOG_PAGE_SIZE
    };
}

export interface CatalogTone {
    id: number;
    title: string;
    description: string;
    gear: string;
    format: string;
    thumbnail: string;
    userName: string;
    userVerified: boolean;
    modelsCount: number;
    a2ModelsCount: number;
    favoritesCount: number;
    downloadsCount: number;
    hasFavoriteState: boolean;
    isFavorite: boolean;
    makes: string[];
    tags: string[];
    sizes: string[];
    license: string;
    url: string;
}

interface CatalogReplyBase {
    ok: boolean;
    needsSignIn: boolean;
    error: string;
}

export interface CatalogTonesReply extends CatalogReplyBase {
    signedIn: boolean;
    page: number;
    pageSize: number;
    total: number;
    totalPages: number;
    tones: CatalogTone[];
}

export interface CatalogModel {
    id: number;
    name: string;
    size: string;
}

export interface CatalogModelsReply extends CatalogReplyBase {
    page: number;
    total: number;
    totalPages: number;
    models: CatalogModel[];
}

export interface CatalogNamesReply extends CatalogReplyBase {
    names: string[];
}

export interface CatalogFavoriteReply extends CatalogReplyBase {
    toneId: number;
    favorite: boolean;
}

/** Thrown when the request needs a TONE3000 session ("Sign in with phone"). */
export class CatalogSignInRequiredError extends Error {
    constructor(message: string) {
        super(message || "Sign in to TONE3000 to use this.");
        this.name = "CatalogSignInRequiredError";
    }
}

function check<T extends CatalogReplyBase>(reply: T): T {
    if (!reply.ok) {
        if (reply.needsSignIn) {
            throw new CatalogSignInRequiredError(reply.error);
        }
        throw new Error(reply.error || "TONE3000 request failed.");
    }
    return reply;
}

export class Tone3000Catalog {
    private model: PiPedalModel;

    constructor(model: PiPedalModel) {
        this.model = model;
    }

    async search(query: CatalogQuery): Promise<CatalogTonesReply> {
        const body = { ...query, query: truncateUtf8(query.query.trim()) };
        return check(await this.model.t3kCatalogRequest<CatalogTonesReply>("t3kCatalogSearch", body));
    }

    /** Works signed out (no favourite state then). */
    async trending(gear: CatalogGear): Promise<CatalogTonesReply> {
        return check(await this.model.t3kCatalogRequest<CatalogTonesReply>("t3kCatalogTrending", gear));
    }

    async setFavorite(toneId: number, favorite: boolean): Promise<void> {
        check(await this.model.t3kCatalogRequest<CatalogFavoriteReply>("t3kCatalogSetFavorite", { toneId, favorite }));
    }

    async models(toneId: number, page: number, architecture: number): Promise<CatalogModelsReply> {
        return check(await this.model.t3kCatalogRequest<CatalogModelsReply>("t3kCatalogModels",
            { toneId, page, pageSize: MODELS_PAGE_SIZE, architecture }));
    }

    async tags(query: string): Promise<string[]> {
        return check(await this.model.t3kCatalogRequest<CatalogNamesReply>("t3kCatalogTags", { query, pageSize: 50 })).names;
    }

    async makes(query: string): Promise<string[]> {
        return check(await this.model.t3kCatalogRequest<CatalogNamesReply>("t3kCatalogMakes", { query, pageSize: 50 })).names;
    }
}
