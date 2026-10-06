// Copyright (c) 2026 Robin E.R. Davies
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

// In-app TONE3000 catalog browser: search, filter, favourite and download tones without
// leaving PiPedal. All catalog calls go through the PiPedal server (which holds the TONE3000
// session); downloads use the server-session download path into the directory being browsed.

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { FixedSizeList, ListChildComponentProps } from "react-window";
import Autocomplete from "@mui/material/Autocomplete";
import Button from "@mui/material/Button";
import Checkbox from "@mui/material/Checkbox";
import Chip from "@mui/material/Chip";
import CircularProgress from "@mui/material/CircularProgress";
import DialogActions from "@mui/material/DialogActions";
import DialogContent from "@mui/material/DialogContent";
import DialogTitle from "@mui/material/DialogTitle";
import FormControlLabel from "@mui/material/FormControlLabel";
import IconButton from "@mui/material/IconButton";
import InputAdornment from "@mui/material/InputAdornment";
import MenuItem from "@mui/material/MenuItem";
import Select from "@mui/material/Select";
import Switch from "@mui/material/Switch";
import TextField from "@mui/material/TextField";
import ToggleButton from "@mui/material/ToggleButton";
import ToggleButtonGroup from "@mui/material/ToggleButtonGroup";
import Typography from "@mui/material/Typography";
import useMediaQuery from "@mui/material/useMediaQuery";
import ArrowBackIcon from "@mui/icons-material/ArrowBack";
import ClearIcon from "@mui/icons-material/Clear";
import DownloadIcon from "@mui/icons-material/Download";
import FavoriteIcon from "@mui/icons-material/Favorite";
import FavoriteBorderIcon from "@mui/icons-material/FavoriteBorder";
import FilterListIcon from "@mui/icons-material/FilterList";
import PhoneIphoneIcon from "@mui/icons-material/PhoneIphone";
import SearchIcon from "@mui/icons-material/Search";
import VerifiedIcon from "@mui/icons-material/Verified";

import DialogEx from "../DialogEx";
import LazyBoundary, { lazyWithRetry } from "../LazyBoundary";
import { PiPedalModel, getErrorMessage } from "../PiPedalModel";
import Tone3000DownloadType from "../Tone3000DownloadType";
import { Tone3000AuthStatus } from "./tone3000-auth";
import {
    CatalogFormat, CatalogGear, CatalogKind, CatalogModel, CatalogQuery, CatalogSignInRequiredError, CatalogSort,
    CatalogTone, FORMAT_OPTIONS, GEAR_OPTIONS, SORT_OPTIONS, Tone3000Catalog, defaultSort, isIrOnlyGear, truncateUtf8,
    emptyCatalogQuery
} from "./tone3000-catalog";

const Tone3000SignInDialog = lazyWithRetry(() => import("../Tone3000SignInDialog"));

const ROW_HEIGHT = 84;
const SEARCH_DEBOUNCE_MS = 400;
const LOAD_MORE_THRESHOLD = 5; // rows from the end.

function formatCount(n: number): string {
    if (n >= 1_000_000) return (n / 1_000_000).toFixed(1).replace(/\.0$/, "") + "M";
    if (n >= 1_000) return (n / 1_000).toFixed(1).replace(/\.0$/, "") + "k";
    return n.toString();
}

function toneFormat(downloadType: Tone3000DownloadType): CatalogFormat {
    return downloadType === Tone3000DownloadType.CabIr ? "ir" : "nam";
}

// The models PiPedal loads for this tone (same rule as the downloader): A2 only for NAM tones
// that have them, when the A2 preference is on.
function modelArchitecture(model: PiPedalModel, tone: CatalogTone): number {
    return tone.format === "nam" && tone.a2ModelsCount > 0 && model.tone3000_A2_models ? 2 : 0;
}

function downloadableModelCount(model: PiPedalModel, tone: CatalogTone): number {
    return modelArchitecture(model, tone) === 2 ? tone.a2ModelsCount : tone.modelsCount;
}

// Only https images and links from the server (it filters too).
function safeHttps(url: string): string {
    return url.toLowerCase().startsWith("https://") ? url : "";
}

interface RowData {
    tones: CatalogTone[];
    showFooter: boolean;
    loading: boolean;
    canDownload: (tone: CatalogTone) => boolean;
    onToggleFavorite: (tone: CatalogTone) => void;
    onDownload: (tone: CatalogTone) => void;
}

function ToneRow(props: ListChildComponentProps<RowData>) {
    const { index, style, data } = props;
    if (index >= data.tones.length) {
        return (
            <div style={{ ...style, display: "flex", alignItems: "center", justifyContent: "center" }}>
                {data.loading && (<CircularProgress size={24} />)}
            </div>
        );
    }
    const tone = data.tones[index];
    const thumbnail = safeHttps(tone.thumbnail);
    const details = [tone.userName, tone.gear, tone.format.toUpperCase()].filter((s) => !!s).join(" · ");
    const canDownload = data.canDownload(tone);
    return (
        <div style={{
            ...style, display: "flex", flexFlow: "row nowrap", alignItems: "center", gap: 8,
            boxSizing: "border-box", paddingLeft: 8, paddingRight: 4, borderBottom: "1px solid rgba(128,128,128,0.2)"
        }}>
            <div style={{
                width: 56, height: 56, flex: "0 0 auto", borderRadius: 4, overflow: "hidden",
                background: "rgba(128,128,128,0.2)"
            }}>
                {thumbnail && (
                    <img src={thumbnail} alt="" loading="lazy" referrerPolicy="no-referrer"
                        style={{ width: "100%", height: "100%", objectFit: "cover" }} />
                )}
            </div>
            <div style={{ flex: "1 1 auto", minWidth: 0 }}>
                <Typography variant="body2" noWrap style={{ fontWeight: 500 }} title={tone.title}>
                    {tone.title}
                </Typography>
                <div style={{ display: "flex", alignItems: "center", gap: 4, minWidth: 0 }}>
                    <Typography variant="caption" noWrap color="text.secondary">{details}</Typography>
                    {tone.userVerified && (<VerifiedIcon style={{ fontSize: 14, opacity: 0.7 }} />)}
                </div>
                <Typography variant="caption" noWrap color="text.secondary" component="div">
                    {downloadModelsLabel(PiPedalModel.getInstance(), tone)} · {formatCount(tone.downloadsCount)} downloads · {formatCount(tone.favoritesCount)} ♥
                </Typography>
            </div>
            <IconButton aria-label={tone.isFavorite ? "remove from favourites" : "add to favourites"}
                onClick={() => data.onToggleFavorite(tone)} size="large"
            >
                {tone.isFavorite ? (<FavoriteIcon color="error" />) : (<FavoriteBorderIcon />)}
            </IconButton>
            <IconButton aria-label="download" onClick={() => data.onDownload(tone)} size="large"
                disabled={!canDownload}
            >
                <DownloadIcon />
            </IconButton>
        </div>
    );
}

function downloadModelsLabel(model: PiPedalModel, tone: CatalogTone): string {
    const n = downloadableModelCount(model, tone);
    return n === 1 ? "1 model" : `${n} models`;
}

// Measures the element it is given (for the virtualised list's height).
function useElementSize(): [(element: HTMLDivElement | null) => void, { width: number; height: number }] {
    const [size, setSize] = useState({ width: 0, height: 0 });
    const observer = useRef<ResizeObserver | null>(null);
    const ref = useCallback((element: HTMLDivElement | null) => {
        observer.current?.disconnect();
        observer.current = null;
        if (element) {
            observer.current = new ResizeObserver((entries) => {
                const r = entries[0].contentRect;
                setSize({ width: Math.floor(r.width), height: Math.floor(r.height) });
            });
            observer.current.observe(element);
        }
    }, []);
    return [ref, size];
}

function ModelPickerDialog(props: {
    tone: CatalogTone;
    catalog: Tone3000Catalog;
    fullScreen: boolean;
    onClose: () => void;
    onDownload: (modelIds: number[] | undefined) => void;
    onSignInRequired: () => void;
}) {
    const { tone, catalog, fullScreen, onClose, onDownload, onSignInRequired } = props;
    const model = PiPedalModel.getInstance();
    const architecture = modelArchitecture(model, tone);
    const [models, setModels] = useState<CatalogModel[]>([]);
    const [page, setPage] = useState(0);
    const [totalPages, setTotalPages] = useState(1);
    const [total, setTotal] = useState(0);
    const [loading, setLoading] = useState(false);
    const [error, setError] = useState("");
    // Everything (including pages not loaded yet) until the user unticks something.
    const [allSelected, setAllSelected] = useState(true);
    const [selected, setSelected] = useState<Set<number>>(new Set());
    const cancelled = useRef(false);

    function loadPage(p: number) {
        setLoading(true);
        setError("");
        catalog.models(tone.id, p, architecture)
            .then((reply) => {
                if (cancelled.current) return;
                setModels((prev) => {
                    const seen = new Set(prev.map((m) => m.id));
                    return prev.concat(reply.models.filter((m) => !seen.has(m.id)));
                });
                setPage(reply.page);
                setTotalPages(reply.totalPages);
                setTotal(reply.total);
                setLoading(false);
            })
            .catch((e) => {
                if (cancelled.current) return;
                setLoading(false);
                if (e instanceof CatalogSignInRequiredError) {
                    onSignInRequired();
                }
                setError(getErrorMessage(e));
            });
    }

    useEffect(() => {
        cancelled.current = false;
        loadPage(1);
        return () => { cancelled.current = true; };
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [tone.id]);

    function isChecked(id: number) {
        return allSelected || selected.has(id);
    }
    function toggle(id: number) {
        const next = new Set<number>(allSelected ? models.map((m) => m.id) : selected);
        if (next.has(id)) next.delete(id); else next.add(id);
        setAllSelected(false);
        setSelected(next);
    }
    function toggleAll() {
        if (allSelected) {
            setAllSelected(false);
            setSelected(new Set());
        } else {
            setAllSelected(true);
        }
    }
    const selectedCount = allSelected ? Math.max(total, models.length) : selected.size;

    return (
        <DialogEx tag="tone3000-models" open={true} onClose={onClose} fullWidth={true} maxWidth="sm"
            fullScreen={fullScreen} onEnterKey={() => { }}
        >
            <DialogTitle style={{ display: "flex", alignItems: "center", gap: 8, paddingLeft: 8 }}>
                <IconButton aria-label="back" onClick={onClose}><ArrowBackIcon /></IconButton>
                <span style={{ overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>{tone.title}</span>
            </DialogTitle>
            <DialogContent dividers style={{ minHeight: 200 }}>
                {tone.description && (
                    <Typography variant="body2" color="text.secondary" style={{ marginBottom: 8, whiteSpace: "pre-wrap", maxHeight: 96, overflow: "auto" }}>
                        {tone.description}
                    </Typography>
                )}
                {(tone.makes.length > 0 || tone.tags.length > 0) && (
                    <div style={{ display: "flex", flexWrap: "wrap", gap: 4, marginBottom: 8 }}>
                        {tone.makes.map((m) => (<Chip key={"m" + m} label={m} size="small" />))}
                        {tone.tags.map((t) => (<Chip key={"t" + t} label={t} size="small" variant="outlined" />))}
                    </div>
                )}
                <FormControlLabel
                    control={<Checkbox checked={allSelected} indeterminate={!allSelected && selected.size > 0} onChange={toggleAll} />}
                    label={`All models${total ? ` (${total})` : ""}`}
                />
                {models.map((m) => (
                    <div key={m.id} style={{ display: "flex", alignItems: "center" }}>
                        <Checkbox checked={isChecked(m.id)} onChange={() => toggle(m.id)} inputProps={{ "aria-label": m.name }} />
                        <Typography variant="body2" style={{ flex: "1 1 auto", minWidth: 0, overflowWrap: "anywhere" }}>{m.name}</Typography>
                        {m.size && (<Typography variant="caption" color="text.secondary" style={{ marginLeft: 8 }}>{m.size}</Typography>)}
                    </div>
                ))}
                {loading && (<div style={{ display: "flex", justifyContent: "center", padding: 8 }}><CircularProgress size={24} /></div>)}
                {error && (<Typography variant="body2" color="error">{error}</Typography>)}
                {!loading && page < totalPages && (
                    <Button onClick={() => loadPage(page + 1)}>Load more models</Button>
                )}
                {safeHttps(tone.url) && (
                    <Typography variant="caption" component="div" style={{ marginTop: 8 }}>
                        <a href={safeHttps(tone.url)} target="_blank" rel="noopener noreferrer">View on TONE3000</a>
                    </Typography>
                )}
            </DialogContent>
            <DialogActions>
                <Button variant="dialogSecondary" onClick={onClose}>Cancel</Button>
                <Button variant="dialogPrimary" disabled={selectedCount === 0 || (models.length === 0 && loading)}
                    onClick={() => { onDownload(allSelected ? undefined : Array.from(selected)); }}
                >
                    {`Download${selectedCount ? ` (${selectedCount})` : ""}`}
                </Button>
            </DialogActions>
        </DialogEx>
    );
}

export default function Tone3000CatalogDialog(props: {
    open: boolean;
    onClose: () => void;
    /** What the file browser holds: decides the initial format and what can be downloaded here. */
    downloadType: Tone3000DownloadType;
    /** The directory being browsed: downloads go here. */
    downloadPath: string;
    /** Fallback sign-in (the browser popup) when the server can't do the phone sign-in. */
    onUseBrowserSignIn?: () => void;
}) {
    const { open, onClose, downloadType, downloadPath, onUseBrowserSignIn } = props;
    const model = PiPedalModel.getInstance();
    const catalog = useMemo(() => new Tone3000Catalog(model), [model]);
    const fullScreen = useMediaQuery("(max-width: 600px)");
    const wantFormat = toneFormat(downloadType);

    const [authStatus, setAuthStatus] = useState<Tone3000AuthStatus>(model.tone3000AuthStatus.get());
    const signedIn = authStatus.signedIn;
    const [showSignIn, setShowSignIn] = useState(false);

    const [text, setText] = useState("");
    const [debouncedText, setDebouncedText] = useState("");
    const [kind, setKind] = useState<CatalogKind>("search");
    const [gear, setGear] = useState<CatalogGear>(downloadType === Tone3000DownloadType.CabIr ? "ir" : "");
    const [format, setFormat] = useState<CatalogFormat>(wantFormat);
    const [sort, setSort] = useState<CatalogSort>("");
    const [showFilters, setShowFilters] = useState(false);
    const [tags, setTags] = useState<string[]>([]);
    const [makes, setMakes] = useState<string[]>([]);
    const [calibrated, setCalibrated] = useState(false);
    const [verified, setVerified] = useState(false);
    const [tagOptions, setTagOptions] = useState<string[]>([]);
    const [makeOptions, setMakeOptions] = useState<string[]>([]);
    const [tagInput, setTagInput] = useState("");
    const [makeInput, setMakeInput] = useState("");

    const [tones, setTones] = useState<CatalogTone[]>([]);
    const [page, setPage] = useState(0);
    const [totalPages, setTotalPages] = useState(1);
    const [loading, setLoading] = useState(false);
    const [error, setError] = useState("");
    const [needsSignIn, setNeedsSignIn] = useState(false);
    const [pickerTone, setPickerTone] = useState<CatalogTone | null>(null);

    const generation = useRef(0);
    const loadingRef = useRef(false);
    const favoritesInFlight = useRef<Set<number>>(new Set());
    const listRef = useRef<FixedSizeList | null>(null);
    const [listBoxRef, listSize] = useElementSize();

    // Auth status.
    useEffect(() => {
        if (!open) return;
        const onChanged = (value: Tone3000AuthStatus) => { setAuthStatus(value); };
        model.tone3000AuthStatus.addOnChangedHandler(onChanged);
        model.t3kRefreshAuthStatus().then((s) => setAuthStatus(s)).catch(() => { });
        return () => { model.tone3000AuthStatus.removeOnChangedHandler(onChanged); };
    }, [open, model]);

    useEffect(() => {
        if (signedIn) setNeedsSignIn(false);
    }, [signedIn]);

    // Debounced search text.
    useEffect(() => {
        const timer = setTimeout(() => { setDebouncedText(text.trim()); }, SEARCH_DEBOUNCE_MS);
        return () => { clearTimeout(timer); };
    }, [text]);

    const query: CatalogQuery = useMemo(() => ({
        ...emptyCatalogQuery(),
        kind, query: debouncedText, sort, gear, format, tags, makes, calibrated, verified,
    }), [kind, debouncedText, sort, gear, format, tags, makes, calibrated, verified]);

    const loadPage = useCallback((p: number) => {
        const gen = generation.current;
        loadingRef.current = true;
        setLoading(true);
        setError("");
        // Signed out: the public trending list is all there is.
        const request = signedIn ? catalog.search({ ...query, page: p }) : catalog.trending(gear);
        request
            .then((reply) => {
                if (gen !== generation.current) return;
                setTones((prev) => {
                    if (p === 1) return reply.tones;
                    const seen = new Set(prev.map((t) => t.id));
                    return prev.concat(reply.tones.filter((t) => !seen.has(t.id)));
                });
                setPage(reply.page);
                setTotalPages(signedIn ? reply.totalPages : 1);
                loadingRef.current = false;
                setLoading(false);
            })
            .catch((e) => {
                if (gen !== generation.current) return;
                loadingRef.current = false;
                setLoading(false);
                if (e instanceof CatalogSignInRequiredError) {
                    setNeedsSignIn(true);
                }
                setError(getErrorMessage(e));
            });
    }, [catalog, query, signedIn, gear]);

    // New query: start again from page 1.
    useEffect(() => {
        if (!open) return;
        ++generation.current;
        setTones([]);
        setPage(0);
        setTotalPages(1);
        listRef.current?.scrollTo(0);
        loadPage(1);
    }, [open, loadPage]);

    // Tag / make suggestions (signed in only).
    useEffect(() => {
        if (!open || !signedIn || !showFilters) return;
        let cancelled = false;
        const timer = setTimeout(() => {
            catalog.tags(tagInput.trim()).then((names) => { if (!cancelled) setTagOptions(names.filter((n) => !n.includes("_"))); }).catch(() => { });
        }, SEARCH_DEBOUNCE_MS);
        return () => { cancelled = true; clearTimeout(timer); };
    }, [open, signedIn, showFilters, tagInput, catalog]);
    useEffect(() => {
        if (!open || !signedIn || !showFilters) return;
        let cancelled = false;
        const timer = setTimeout(() => {
            catalog.makes(makeInput.trim()).then((names) => { if (!cancelled) setMakeOptions(names.filter((n) => !n.includes("_"))); }).catch(() => { });
        }, SEARCH_DEBOUNCE_MS);
        return () => { cancelled = true; clearTimeout(timer); };
    }, [open, signedIn, showFilters, makeInput, catalog]);

    function requireSignIn(): boolean {
        if (signedIn) return true;
        setShowSignIn(true);
        return false;
    }

    function patchTone(id: number, patch: (t: CatalogTone) => CatalogTone) {
        setTones((prev) => prev.map((t) => (t.id === id ? patch(t) : t)));
        setPickerTone((prev) => (prev && prev.id === id ? patch(prev) : prev));
    }

    // Optimistic: flip now, roll back if the server says no.
    const handleToggleFavorite = useCallback((tone: CatalogTone) => {
        if (!requireSignIn()) return;
        // One request per tone at a time: ignore taps while one is in flight.
        if (favoritesInFlight.current.has(tone.id)) return;
        favoritesInFlight.current.add(tone.id);
        const favorite = !tone.isFavorite;
        const apply = (fav: boolean) => (t: CatalogTone): CatalogTone => {
            if (t.isFavorite === fav) return t;
            return { ...t, isFavorite: fav, hasFavoriteState: true, favoritesCount: Math.max(0, t.favoritesCount + (fav ? 1 : -1)) };
        };
        patchTone(tone.id, apply(favorite));
        catalog.setFavorite(tone.id, favorite).finally(() => {
            favoritesInFlight.current.delete(tone.id);
        }).catch((e) => {
            patchTone(tone.id, apply(!favorite));
            if (e instanceof CatalogSignInRequiredError) {
                setShowSignIn(true);
            } else {
                model.showAlert(getErrorMessage(e));
            }
        });
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [catalog, signedIn, model]);

    const canDownload = useCallback((tone: CatalogTone) => tone.format === wantFormat, [wantFormat]);

    // modelIds undefined means every model: say so explicitly so the downloader doesn't show its own picker.
    function startDownload(tone: CatalogTone, modelIds: number[] | undefined) {
        model.downloadTone3000ToneWithServerAuth(tone.id, downloadType, downloadPath, modelIds, modelIds === undefined)
            .catch((e) => { model.showAlert(getErrorMessage(e)); });
    }

    const handleDownload = useCallback((tone: CatalogTone) => {
        if (!requireSignIn()) return;
        if (downloadableModelCount(model, tone) <= 1) {
            startDownload(tone, undefined);
        } else {
            setPickerTone(tone);
        }
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [signedIn, model, downloadType, downloadPath]);

    const hasMore = signedIn && page < totalPages;
    const rowData: RowData = useMemo(() => ({
        tones, loading, showFooter: hasMore || loading,
        canDownload, onToggleFavorite: handleToggleFavorite, onDownload: handleDownload
    }), [tones, loading, hasMore, canDownload, handleToggleFavorite, handleDownload]);
    const itemCount = tones.length + (rowData.showFooter ? 1 : 0);

    function handleItemsRendered({ visibleStopIndex }: { visibleStopIndex: number }) {
        // After a failed page the Retry button is the only way to try again (no retry-on-scroll loop).
        if (hasMore && !loadingRef.current && !error && visibleStopIndex >= tones.length - LOAD_MORE_THRESHOLD) {
            loadPage(page + 1);
        }
    }

    const ownList = kind !== "search";
    const effectiveSort: CatalogSort = sort || defaultSort(debouncedText);
    const activeFilterCount = tags.length + makes.length + (calibrated ? 1 : 0) + (verified ? 1 : 0);

    function setSortChoice(value: CatalogSort) {
        // The default is stored as "no choice", so it follows the search text.
        setSort(value === defaultSort(debouncedText) ? "" : value);
    }

    let emptyMessage = "";
    if (!loading && !error && tones.length === 0 && page > 0) {
        emptyMessage = kind === "favorited" ? "No favourites yet."
            : kind === "downloaded" ? "Nothing downloaded yet."
                : "No tones found.";
    }

    return (
        <DialogEx tag="tone3000-catalog" open={open} onClose={onClose} fullWidth={true} maxWidth="md"
            fullScreen={fullScreen} onEnterKey={() => { }}
            PaperProps={{ style: fullScreen ? undefined : { height: "85vh" } }}
        >
            <DialogTitle style={{ display: "flex", alignItems: "center", gap: 8, paddingLeft: 8, paddingBottom: 8 }}>
                <IconButton aria-label="close" onClick={onClose}><ArrowBackIcon /></IconButton>
                <span style={{ flex: "1 1 auto" }}>TONE3000</span>
                {!signedIn && (
                    <Button size="small" startIcon={<PhoneIphoneIcon />} onClick={() => setShowSignIn(true)}>Sign in</Button>
                )}
            </DialogTitle>
            <DialogContent style={{ display: "flex", flexFlow: "column nowrap", paddingLeft: 8, paddingRight: 8, paddingBottom: 0, overflow: "hidden" }}>
                {(!signedIn || needsSignIn) && (
                    <div style={{
                        display: "flex", flexWrap: "wrap", alignItems: "center", justifyContent: "flex-end", gap: 8,
                        padding: 8, marginBottom: 8, borderRadius: 4, background: "rgba(128,128,128,0.12)", flex: "0 0 auto"
                    }}>
                        <Typography variant="body2" style={{ flex: "1 1 200px" }}>
                            Showing trending tones. Sign in with your phone to search, keep favourites and download.
                        </Typography>
                        <Button variant="dialogPrimary" size="small" onClick={() => setShowSignIn(true)}>Sign in with phone</Button>
                    </div>
                )}
                {signedIn && (
                    <ToggleButtonGroup size="small" exclusive value={kind} style={{ marginBottom: 8, flex: "0 0 auto" }}
                        onChange={(_e, value) => { if (value) setKind(value as CatalogKind); }}
                    >
                        <ToggleButton value="search">Browse</ToggleButton>
                        <ToggleButton value="favorited">Favourites</ToggleButton>
                        <ToggleButton value="downloaded">Downloaded</ToggleButton>
                    </ToggleButtonGroup>
                )}
                {signedIn && (
                    <div style={{ display: "flex", alignItems: "center", gap: 4, flex: "0 0 auto" }}>
                        <TextField size="small" fullWidth placeholder={ownList ? "Filter by title" : "Search TONE3000"}
                            value={text}
                            onChange={(e) => setText(truncateUtf8(e.target.value))}
                            slotProps={{
                                input: {
                                    startAdornment: (<InputAdornment position="start"><SearchIcon /></InputAdornment>),
                                    endAdornment: text ? (
                                        <InputAdornment position="end">
                                            <IconButton size="small" aria-label="clear search" onClick={() => setText("")}><ClearIcon /></IconButton>
                                        </InputAdornment>
                                    ) : undefined,
                                },
                                htmlInput: { enterKeyHint: "search", "aria-label": "search TONE3000" }
                            }}
                        />
                        {!ownList && (
                            <IconButton aria-label="more filters" onClick={() => setShowFilters(!showFilters)}
                                color={showFilters || activeFilterCount > 0 ? "primary" : "default"}
                            >
                                <FilterListIcon />
                            </IconButton>
                        )}
                    </div>
                )}
                <div style={{ display: "flex", flexFlow: "row nowrap", gap: 6, overflowX: "auto", padding: "8px 0", flex: "0 0 auto" }}>
                    {GEAR_OPTIONS.map((g) => (
                        <Chip key={g.value} label={g.label} size="small" clickable
                            color={gear === g.value ? "primary" : "default"}
                            variant={gear === g.value ? "filled" : "outlined"}
                            onClick={() => setGear(gear === g.value ? "" : g.value)}
                        />
                    ))}
                    {signedIn && !ownList && (<div style={{ borderLeft: "1px solid rgba(128,128,128,0.4)", margin: "0 2px" }} />)}
                    {signedIn && !ownList && FORMAT_OPTIONS.map((f) => (
                        <Chip key={f.value} label={f.label} size="small" clickable
                            color={format === f.value ? "primary" : "default"}
                            variant={format === f.value ? "filled" : "outlined"}
                            onClick={() => setFormat(format === f.value ? "" : f.value)}
                        />
                    ))}
                </div>
                {signedIn && !ownList && (
                    <div style={{ display: "flex", alignItems: "center", gap: 8, flex: "0 0 auto", marginBottom: 4 }}>
                        <Typography variant="caption" color="text.secondary">Sort</Typography>
                        <Select size="small" variant="standard" value={effectiveSort}
                            onChange={(e) => setSortChoice(e.target.value as CatalogSort)}
                        >
                            {SORT_OPTIONS.filter((o) => o.value !== "bestMatch" || debouncedText !== "").map((o) => (
                                <MenuItem key={o.value} value={o.value}>{o.label}</MenuItem>
                            ))}
                        </Select>
                    </div>
                )}
                {signedIn && !ownList && showFilters && (
                    <div style={{ display: "flex", flexFlow: "column nowrap", gap: 8, marginBottom: 8, flex: "0 1 auto", minHeight: 0, maxHeight: "40vh", overflowY: "auto" }}>
                        <Autocomplete multiple size="small" options={tagOptions} value={tags}
                            filterOptions={(x) => x} filterSelectedOptions
                            onChange={(_e, value) => setTags(value.slice(0, 10))}
                            onInputChange={(_e, value) => setTagInput(value)}
                            renderInput={(params) => (<TextField {...params} placeholder="Tags" />)}
                        />
                        <Autocomplete multiple size="small" options={makeOptions} value={makes}
                            filterOptions={(x) => x} filterSelectedOptions
                            onChange={(_e, value) => setMakes(value.slice(0, 10))}
                            onInputChange={(_e, value) => setMakeInput(value)}
                            renderInput={(params) => (<TextField {...params} placeholder="Makes" />)}
                        />
                        <div style={{ display: "flex", flexWrap: "wrap", gap: 8 }}>
                            <FormControlLabel control={<Switch checked={calibrated} disabled={isIrOnlyGear(gear) || format === "ir"}
                                onChange={(e) => setCalibrated(e.target.checked)} />} label="Calibrated" />
                            <FormControlLabel control={<Switch checked={verified}
                                onChange={(e) => setVerified(e.target.checked)} />} label="Verified creators" />
                        </div>
                    </div>
                )}
                {error && !needsSignIn && (
                    <div style={{ display: "flex", alignItems: "center", gap: 8, marginBottom: 4 }}>
                        <Typography variant="body2" color="error" style={{ flex: "1 1 auto" }}>{error}</Typography>
                        <Button size="small" onClick={() => loadPage(tones.length > 0 ? page + 1 : 1)}>Retry</Button>
                    </div>
                )}
                {format && format !== wantFormat && signedIn && !ownList && (
                    <Typography variant="caption" color="text.secondary" style={{ marginBottom: 4 }}>
                        {wantFormat === "nam"
                            ? "This folder holds NAM models: open TONE3000 from an IR file browser to download IRs."
                            : "This folder holds IRs: open TONE3000 from a NAM model file browser to download models."}
                    </Typography>
                )}
                <div ref={listBoxRef} style={{ flex: "1 1 auto", minHeight: 120, position: "relative" }}>
                    {emptyMessage && (
                        <Typography variant="body2" color="text.secondary" style={{ textAlign: "center", padding: 24 }}>{emptyMessage}</Typography>
                    )}
                    {listSize.height > 0 && (
                        <FixedSizeList
                            ref={listRef}
                            height={listSize.height}
                            width={listSize.width}
                            itemCount={itemCount}
                            itemSize={ROW_HEIGHT}
                            itemData={rowData}
                            overscanCount={4}
                            onItemsRendered={handleItemsRendered}
                            style={{ position: "absolute", left: 0, top: 0 }}
                        >
                            {ToneRow}
                        </FixedSizeList>
                    )}
                </div>
            </DialogContent>
            {pickerTone && (
                <ModelPickerDialog tone={pickerTone} catalog={catalog} fullScreen={fullScreen}
                    onClose={() => setPickerTone(null)}
                    onSignInRequired={() => setShowSignIn(true)}
                    onDownload={(modelIds) => {
                        const tone = pickerTone;
                        setPickerTone(null);
                        startDownload(tone, modelIds);
                    }}
                />
            )}
            {showSignIn && (
                <LazyBoundary onLoadFailed={() => setShowSignIn(false)}>
                    <Tone3000SignInDialog open={showSignIn} onClose={() => setShowSignIn(false)}
                        onUseBrowserSignIn={onUseBrowserSignIn}
                    />
                </LazyBoundary>
            )}
        </DialogEx>
    );
}
