// Minimal typings for the browser build of the `qrcode` npm package (MIT).
// @types/qrcode is not used: it references @types/node, whose setTimeout typings break
// browser code across the project.
declare module "qrcode" {
    export interface QRCodeToDataURLOptions {
        errorCorrectionLevel?: "L" | "M" | "Q" | "H";
        margin?: number;
        width?: number;
        scale?: number;
        color?: { dark?: string; light?: string };
    }
    export function toDataURL(text: string, options?: QRCodeToDataURLOptions): Promise<string>;
    const QRCode: {
        toDataURL: typeof toDataURL;
    };
    export default QRCode;
}
