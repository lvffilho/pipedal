// Copyright (c) 2026 Robin E. R. Davies
// MIT license; see other source files in this project.

/*
 * "Sign in with phone": TONE3000 device-code sign-in (RFC 8628) driven by the PiPedal server.
 *
 * The server requests a device code and polls TONE3000; this dialog shows the QR code of
 * verification_uri_complete plus the user code, and follows status updates pushed over the
 * websocket (onT3kAuthStatusChanged). If the device endpoint refuses, the dialog offers the
 * existing popup sign-in instead.
 */

import { useEffect, useState } from "react";
import QRCode from "qrcode";
import Button from "@mui/material/Button";
import DialogTitle from "@mui/material/DialogTitle";
import DialogContent from "@mui/material/DialogContent";
import DialogActions from "@mui/material/DialogActions";
import Typography from "@mui/material/Typography";
import CircularProgress from "@mui/material/CircularProgress";
import Link from "@mui/material/Link";
import DialogEx from "./DialogEx";
import { PiPedalModel, getErrorMessage } from "./PiPedalModel";
import { Tone3000AuthStatus } from "./t3k/tone3000-auth";

// Only ever link to / encode https URLs from the server (never javascript:, data:, http:).
function safeHttpsUrl(url: string): string {
    return url.toLowerCase().startsWith("https://") ? url : "";
}

function formatRemaining(ms: number): string {
    let seconds = Math.max(0, Math.floor(ms / 1000));
    const minutes = Math.floor(seconds / 60);
    seconds = seconds % 60;
    return `${minutes}:${seconds.toString().padStart(2, "0")}`;
}

export default function Tone3000SignInDialog(props: {
    open: boolean;
    onClose: () => void;
    /** Fallback when the server can't use the device flow: the existing popup sign-in. */
    onUseBrowserSignIn?: () => void;
}) {
    const { open, onClose, onUseBrowserSignIn } = props;
    const model = PiPedalModel.getInstance();
    const [status, setStatus] = useState<Tone3000AuthStatus>(model.tone3000AuthStatus.get());
    const [qrDataUrl, setQrDataUrl] = useState<string>("");
    const [requestError, setRequestError] = useState<string>("");
    const [now, setNow] = useState<number>(Date.now());

    useEffect(() => {
        if (!open) return;
        const onChanged = (value: Tone3000AuthStatus) => { setStatus(value); };
        model.tone3000AuthStatus.addOnChangedHandler(onChanged);
        setRequestError("");
        model.t3kRefreshAuthStatus()
            .then((s) => {
                // Start straight away unless already signed in or a flow is in progress.
                if (!s.signedIn && s.deviceState !== "requesting" && s.deviceState !== "waiting") {
                    startSignIn();
                }
            })
            .catch((e) => { setRequestError(getErrorMessage(e)); });
        return () => {
            model.tone3000AuthStatus.removeOnChangedHandler(onChanged);
        };
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [open]);

    useEffect(() => {
        const uri = status.deviceState === "waiting" ? safeHttpsUrl(status.verificationUriComplete) : "";
        if (!uri) {
            setQrDataUrl("");
            return;
        }
        let cancelled = false;
        QRCode.toDataURL(uri, { margin: 1, width: 240, errorCorrectionLevel: "M" })
            .then((url) => { if (!cancelled) setQrDataUrl(url); })
            .catch(() => { if (!cancelled) setQrDataUrl(""); });
        return () => { cancelled = true; };
    }, [status.deviceState, status.verificationUriComplete]);

    useEffect(() => {
        if (!open || status.deviceState !== "waiting") return;
        const timer = setInterval(() => { setNow(Date.now()); }, 1000);
        return () => { clearInterval(timer); };
    }, [open, status.deviceState]);

    function startSignIn() {
        setRequestError("");
        model.t3kStartDeviceSignIn().catch((e) => { setRequestError(getErrorMessage(e)); });
    }

    function handleClose() {
        if (status.deviceState === "requesting" || status.deviceState === "waiting") {
            model.t3kCancelDeviceSignIn().catch(() => { });
        }
        onClose();
    }

    function handleSignOut() {
        model.t3kSignOut().catch((e) => { setRequestError(getErrorMessage(e)); });
    }

    function handleUseBrowser() {
        handleClose();
        if (onUseBrowserSignIn) onUseBrowserSignIn();
    }

    const state = status.deviceState;
    let content: React.ReactNode;
    let actions: React.ReactNode;

    if (requestError) {
        content = (<Typography variant="body2" color="error">{requestError}</Typography>);
        actions = (<Button variant="dialogPrimary" onClick={startSignIn}>Try again</Button>);
    } else if (state === "requesting") {
        content = (
            <div style={{ display: "flex", alignItems: "center", gap: 16 }}>
                <CircularProgress size={24} />
                <Typography variant="body2">Getting a sign-in code from TONE3000...</Typography>
            </div>
        );
    } else if (state === "waiting") {
        content = (
            <div style={{ display: "flex", flexFlow: "column nowrap", alignItems: "center", gap: 12 }}>
                <Typography variant="body2" style={{ textAlign: "center" }}>
                    Scan the code with your phone and approve the sign-in.
                </Typography>
                <div style={{
                    background: "#FFFFFF", padding: 8, borderRadius: 4, width: 240, height: 240,
                    maxWidth: "70vw", maxHeight: "70vw", boxSizing: "content-box",
                    display: "flex", alignItems: "center", justifyContent: "center"
                }}>
                    {qrDataUrl ? (
                        <img src={qrDataUrl} alt="TONE3000 sign-in QR code" style={{ width: "100%", height: "100%" }} />
                    ) : (
                        <CircularProgress size={24} />
                    )}
                </div>
                <Typography variant="body2" style={{ textAlign: "center" }}>
                    {safeHttpsUrl(status.verificationUri) ? (
                        <>Or go to <Link href={safeHttpsUrl(status.verificationUri)} target="_blank" rel="noopener noreferrer">{status.verificationUri}</Link> and enter:</>
                    ) : (
                        <>Or enter this code on the TONE3000 website:</>
                    )}
                </Typography>
                <Typography variant="h5" style={{ fontFamily: "monospace", letterSpacing: "0.1em" }}>
                    {status.userCode}
                </Typography>
                <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
                    <CircularProgress size={14} />
                    <Typography variant="caption">
                        Waiting for approval{status.expiresAtMs ? ` (code expires in ${formatRemaining(status.expiresAtMs - now)})` : ""}
                    </Typography>
                </div>
            </div>
        );
    } else if (status.signedIn) {
        content = (<Typography variant="body2">This PiPedal server is signed in to TONE3000.</Typography>);
        actions = (<Button variant="dialogSecondary" onClick={handleSignOut}>Sign out</Button>);
    } else if (state === "unavailable") {
        content = (
            <div style={{ display: "flex", flexFlow: "column nowrap", gap: 12 }}>
                <Typography variant="body2" color="error">{status.error}</Typography>
                <Typography variant="body2">You can sign in from this browser instead.</Typography>
            </div>
        );
        if (onUseBrowserSignIn) {
            actions = (<Button variant="dialogPrimary" onClick={handleUseBrowser}>Sign in with browser</Button>);
        }
    } else if (state === "failed") {
        content = (<Typography variant="body2" color="error">{status.error}</Typography>);
        actions = (<Button variant="dialogPrimary" onClick={startSignIn}>Try again</Button>);
    } else {
        content = (<Typography variant="body2">Sign in to TONE3000 by scanning a QR code with your phone.</Typography>);
        actions = (<Button variant="dialogPrimary" onClick={startSignIn}>Sign in with phone</Button>);
    }

    return (
        <DialogEx
            tag="tone3000-signin"
            open={open}
            fullWidth={true}
            maxWidth="xs"
            onClose={handleClose}
            onEnterKey={() => { }}
        >
            <DialogTitle>TONE3000 sign-in</DialogTitle>
            <DialogContent>
                {content}
            </DialogContent>
            <DialogActions>
                {actions}
                <Button variant="dialogSecondary" onClick={handleClose}>
                    {status.signedIn && state !== "waiting" ? "Close" : "Cancel"}
                </Button>
            </DialogActions>
        </DialogEx>
    );
}
