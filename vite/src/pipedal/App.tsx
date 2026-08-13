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

import React from 'react';

import { ThemeProvider, createTheme, StyledEngineProvider } from '@mui/material/styles';
import { type Theme } from '@mui/material/styles';
import CssBaseline from '@mui/material/CssBaseline';

import VirtualKeyboardHandler from './VirtualKeyboardHandler';
import AppThemed from "./AppThemed";
import { isDarkMode } from './DarkMode';
import { appBarBackgroundImage, hairlineColor } from './ThemeSurfaces';
import Tone3000AuthComplete from './Tone3000AuthComplete';
import FontTest from './FontTest';

import IconTest from './IconTest';

declare module '@mui/material/styles' {
    interface Theme {
        mainBackground: React.CSSProperties['color'];
        toolbarColor: React.CSSProperties['color'];
    }
    interface ThemeOptions {
        mainBackground?: React.CSSProperties['color'];
        toolbarColor?: React.CSSProperties['color'];
    }
    interface Palette {
        actionBar: Palette['primary'];
    }
    interface PaletteOptions {
        actionBar: PaletteOptions['primary'];
    }

}

declare module '@mui/material/Button' {
    interface ButtonPropsVariantOverrides {
        dialogPrimary: true;
        dialogSecondary: true;
    }
}


// declare module '@mui/styles/defaultTheme' {
//     // eslint-disable-next-line @typescript-eslint/no-empty-interface
//     interface DefaultTheme extends Theme { }
// }




// `any`: MUI v6 requires `variants: []` on every styleOverrides entry once any entry
// sets `variants`, which buys nothing here.
const sharedComponents = (isDark: boolean): any => ({
    MuiCssBaseline: {
        styleOverrides: {
            html: {
                WebkitFontSmoothing: 'antialiased',
                MozOsxFontSmoothing: 'grayscale',
            },
            body: {
                textRendering: 'optimizeLegibility',
            },
            img: {
                outline: `1px solid ${hairlineColor(isDark)}`,
                outlineOffset: '-1px',
            },
        },
    },
    MuiAppBar: {
        styleOverrides: {
            root: {
                backgroundImage: appBarBackgroundImage(isDark),
                backdropFilter: 'blur(10px)',
                WebkitBackdropFilter: 'blur(10px)',
                boxShadow: isDark
                    ? '0 1px 0 0 rgba(255,255,255,0.04), 0 4px 16px rgba(0,0,0,0.35)'
                    : '0 1px 0 0 rgba(0,0,0,0.04), 0 4px 16px rgba(103,80,164,0.08)',
                color: 'inherit',
            },
        },
    },
    MuiDrawer: {
        styleOverrides: {
            paper: {
                backgroundImage: isDark
                    ? 'linear-gradient(180deg, #221f2c 0%, #1a1822 100%)'
                    : 'linear-gradient(180deg, #ffffff 0%, #faf8fd 100%)',
                borderRight: `1px solid ${hairlineColor(isDark)}`,
                boxShadow: isDark
                    ? '0 0 40px rgba(0,0,0,0.5)'
                    : '0 0 40px rgba(103,80,164,0.12)',
            },
        },
    },
    MuiPaper: {
        styleOverrides: {
            root: {
                backgroundImage: 'none',
            },
            rounded: {
                borderRadius: 12,
            },
            elevation1: { boxShadow: isDark ? '0 1px 2px rgba(0,0,0,0.4)' : '0 1px 2px rgba(0,0,0,0.06)' },
            elevation2: { boxShadow: isDark ? '0 2px 8px rgba(0,0,0,0.4)' : '0 2px 8px rgba(0,0,0,0.08)' },
            elevation4: { boxShadow: isDark ? '0 4px 16px rgba(0,0,0,0.5)' : '0 4px 16px rgba(0,0,0,0.10)' },
        },
    },
    MuiDialog: {
        styleOverrides: {
            paper: {
                borderRadius: 16,
                boxShadow: isDark
                    ? '0 24px 64px rgba(0,0,0,0.6), 0 0 0 1px rgba(255,255,255,0.04)'
                    : '0 24px 64px rgba(103,80,164,0.18), 0 0 0 1px rgba(0,0,0,0.04)',
            },
        },
    },
    MuiButton: {
        styleOverrides: {
            root: {
                '& .MuiTouchRipple-root': { borderRadius: 'inherit' },
                '& .MuiTouchRipple-ripple': { transform: 'scale(1.9)' },
            },
            containedPrimary: {
                borderRadius: '9999px',
                paddingLeft: '16px', paddingRight: '16px',
                textTransform: 'none',
                fontWeight: 600,
                boxShadow: 'none',
                '&:hover': {
                    boxShadow: isDark ? '0 4px 12px rgba(167,112,228,0.35)' : '0 4px 12px rgba(103,80,164,0.25)',
                },
            },
            containedSecondary: {
                borderRadius: '9999px',
                paddingLeft: '16px', paddingRight: '16px',
                textTransform: 'none',
                fontWeight: 600,
            },
        },
        variants: [
            {
                props: { variant: 'dialogPrimary' },
                style: { color: isDark ? '#FFFFFF' : 'rgb(0,0,0,0.87)' },
            },
            {
                props: { variant: 'dialogSecondary' },
                style: { color: isDark ? 'rgba(255,255,255,0.7)' : 'rgba(0,0,0,0.6)' },
            },
        ],
    },
    MuiIconButton: {
        styleOverrides: {
            root: {
                padding: 8,
                borderRadius: 10,
                transition: 'background-color 150ms ease-out, color 150ms ease-out, transform 120ms ease-out',
                '&:active': { transform: 'scale(0.94)' },
            },
        },
    },
    MuiSwitch: {
        styleOverrides: {
            root: {
                // Thumb centring depends on switchBase.padding + thumb/2 === root.height/2 (by default
                // 9 + 20/2 === 38/2), so root width/height/padding and switchBase padding must not be
                // overridden piecemeal here. Non-geometric polish only.
                '& .MuiSwitch-thumb': {
                    boxShadow: '0 1px 3px rgba(0,0,0,0.3)',
                },
                '& .MuiSwitch-track': {
                    borderRadius: 999,
                    opacity: isDark ? 0.45 : 0.6,
                },
            },
        },
    },
    MuiListItemButton: {
        styleOverrides: {
            root: ({ theme }: { theme: Theme }) => ({
                transition: 'background-color 150ms ease-out, color 150ms ease-out',
                borderRadius: 8,
                margin: '2px 8px',
                padding: '8px 12px',
                '&.Mui-selected': {
                    backgroundColor: isDark ? 'rgba(167,112,228,0.18)' : 'rgba(103,80,164,0.12)',
                    '&:hover': {
                        backgroundColor: isDark ? 'rgba(167,112,228,0.24)' : 'rgba(103,80,164,0.16)',
                    },
                },
                '&:hover': {
                    backgroundColor: isDark ? 'rgba(255,255,255,0.05)' : 'rgba(0,0,0,0.04)',
                },
            }),
        },
    },
    MuiToolbar: {
        styleOverrides: {
            dense: {
                minHeight: 56,
                paddingLeft: 8,
                paddingRight: 8,
            },
        },
    },
    MuiDivider: {
        styleOverrides: {
            root: {
                borderColor: hairlineColor(isDark),
                margin: '4px 0',
            },
        },
    },
    MuiTooltip: {
        styleOverrides: {
            tooltip: {
                backgroundColor: isDark ? 'rgba(40,36,52,0.96)' : 'rgba(40,36,52,0.96)',
                color: '#FFFFFF',
                borderRadius: 8,
                padding: '8px 12px',
                fontSize: '0.75rem',
                boxShadow: '0 4px 16px rgba(0,0,0,0.3)',
            },
            arrow: {
                color: 'rgba(40,36,52,0.96)',
            },
        },
    },
});

const theme = createTheme(
    isDarkMode() ?
        {
            cssVariables: true,
            shape: { borderRadius: 10 },
            typography: {
                fontFamily: '"Roboto", "Inter", "SF Pro Text", -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif',
                h6: { fontWeight: 600, letterSpacing: '-0.01em' },
                subtitle1: { fontWeight: 500 },
                subtitle2: { fontWeight: 500, letterSpacing: '0.01em' },
                button: { fontWeight: 600, letterSpacing: '0.01em' },
                caption: { letterSpacing: '0.02em' },
            },
            components: sharedComponents(true),
            palette: {
                mode: 'dark',
                primary: { main: '#A770E4' },
                secondary: { main: '#FF6060' },
                background: {
                    default: '#16141d',
                    paper: '#221f2c',
                },
                text: {
                    primary: 'rgba(255,255,255,0.92)',
                    secondary: 'rgba(255,255,255,0.60)',
                    disabled: 'rgba(255,255,255,0.35)',
                },
                divider: 'rgba(255,255,255,0.08)',
                action: {
                    hover: 'rgba(167,112,228,0.10)',
                    selected: 'rgba(167,112,228,0.16)',
                    focus: 'rgba(167,112,228,0.12)',
                },
                actionBar: {
                    main: '#130b22ff',
                    contrastText: '#FFFFFF',
                },
            },
            mainBackground: '#16141d',
            toolbarColor: '#16141d',
        }
        :
        {
            cssVariables: true,
            shape: { borderRadius: 10 },
            typography: {
                fontFamily: '"Roboto", "Inter", "SF Pro Text", -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif',
                h6: { fontWeight: 600, letterSpacing: '-0.01em' },
                subtitle1: { fontWeight: 500 },
                subtitle2: { fontWeight: 500, letterSpacing: '0.01em' },
                button: { fontWeight: 600, letterSpacing: '0.01em' },
                caption: { letterSpacing: '0.02em' },
            },
            components: sharedComponents(false),
            palette: {
                mode: 'light',
                primary: { main: '#6750A4' },
                secondary: { main: '#FF6060' },
                background: {
                    default: '#FAFAFA',
                    paper: '#FFFFFF',
                },
                text: {
                    primary: 'rgba(0,0,0,0.87)',
                    secondary: 'rgba(0,0,0,0.55)',
                    disabled: 'rgba(0,0,0,0.30)',
                },
                divider: 'rgba(0,0,0,0.08)',
                action: {
                    hover: 'rgba(103,80,164,0.08)',
                    selected: 'rgba(103,80,164,0.12)',
                    focus: 'rgba(103,80,164,0.10)',
                },
                actionBar: {
                    main: '#130b22ff',
                    contrastText: '#FFFFFF',
                },
            },
            // Must equal palette.background.default: panels fill with mainBackground while the
            // page behind them uses background.default, so any difference shows as banding.
            mainBackground: '#FAFAFA',
            toolbarColor: '#FAFAFA',
        }
);



type AppThemeProps = {

};


function isTone3000Auth() {
    let url = new URL(window.location.href);
    let param = url.searchParams.get("api_key");
    return (param !== null && param !== "")
}
function isFontTest() {
    let url = new URL(window.location.href);
    let param = url.searchParams.get("fontTest");
    return (param !== null)
}
function isIconTest() {
    let url = new URL(window.location.href);
    let param = url.searchParams.get("iconTest");
    return (param !== null)
}

const App = (class extends React.Component {
    // Before the component mounts, we initialise our state

    constructor(props: AppThemeProps) {
        super(props);
        this.state = {
        };
        if (!App.virtualKeyboardHandler) {
            App.virtualKeyboardHandler = new VirtualKeyboardHandler();
        }
    }

    static virtualKeyboardHandler?: VirtualKeyboardHandler;

    render() {
        return (
            <StyledEngineProvider injectFirst>
                <ThemeProvider theme={theme}>
                    <CssBaseline />
                    {
                        isTone3000Auth() && (<Tone3000AuthComplete />)
                        || isFontTest() && (<FontTest />)
                        || isIconTest() && (<IconTest />)
                        || (<AppThemed />)
                    }
                </ThemeProvider>
            </StyledEngineProvider>
        );
    }
}
);

export default App;
