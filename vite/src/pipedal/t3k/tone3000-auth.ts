/**
 * TONE3000 account session held by the PiPedal server (device-code sign-in, RFC 8628).
 *
 * The server owns the tokens (0600 file in its private storage). The browser shows the QR
 * code / user code from Tone3000AuthStatus, and asks the server for a short-lived access
 * token (t3kAuthGetAccessToken) when it downloads model files itself. The refresh token
 * never reaches the browser.
 */

export type Tone3000DeviceState =
    "idle"
    | "requesting"
    | "waiting"      // showing the QR code; the server is polling.
    | "succeeded"
    | "failed"       // expired / declined / network: offer retry.
    | "unavailable"; // the device endpoint refused: fall back to the popup sign-in.

export class Tone3000AuthStatus {
    signedIn: boolean = false;
    deviceState: Tone3000DeviceState = "idle";
    userCode: string = "";
    verificationUri: string = "";
    verificationUriComplete: string = "";
    expiresAtMs: number = 0;
    error: string = "";

    deserialize(input: any): Tone3000AuthStatus {
        this.signedIn = !!input.signedIn;
        this.deviceState = (input.deviceState ?? "idle") as Tone3000DeviceState;
        this.userCode = input.userCode ?? "";
        this.verificationUri = input.verificationUri ?? "";
        this.verificationUriComplete = input.verificationUriComplete ?? "";
        this.expiresAtMs = input.expiresAtMs ?? 0;
        this.error = input.error ?? "";
        return this;
    }
}

export interface Tone3000AccessTokenReply {
    ok: boolean;
    accessToken: string;
    expiresAtMs: number;
    error: string;
}

/** Supplies a Bearer token. forceRefresh is set when retrying after a 401, with the token that was rejected. */
export type Tone3000TokenProvider = (forceRefresh: boolean, rejectedAccessToken?: string) => Promise<string>;
