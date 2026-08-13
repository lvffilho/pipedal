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

import React, { Component } from 'react';
import WithStyles, { withTheme } from './WithStyles';
import { withStyles } from "tss-react/mui";
import {createStyles} from './WithStyles';


import IconButtonEx from './IconButtonEx';
import Drawer from '@mui/material/Drawer';
import ArrowBackIcon from '@mui/icons-material/ArrowBack';
import { Theme } from '@mui/material/styles';
import {isDarkMode} from './DarkMode';

const drawerStyles = (theme: Theme) => {
    return createStyles({
    list: {
        width: 250,
    },
    fullList: {
        width: 'auto',
    },

    drawer_header: {
        color: theme.palette.primary.main,
        background: (isDarkMode()? 'linear-gradient(180deg, rgba(167,112,228,0.10) 0%, rgba(167,112,228,0) 100%)' : 'linear-gradient(180deg, rgba(103,80,164,0.08) 0%, rgba(103,80,164,0) 100%)'),
        borderBottom: `1px solid ${isDarkMode() ? 'rgba(255,255,255,0.06)' : 'rgba(0,0,0,0.06)'}`,
        padding: '8px 4px',
    },
})};

type Anchor = 'top' | 'left' | 'bottom' | 'right';

type CloseEventHandler = () => void;



interface DrawerProps extends WithStyles<typeof drawerStyles> {
    title?: string;
    position: Anchor;
    is_open: boolean;
    onClose?: CloseEventHandler;
    children?: React.ReactNode;
    theme: Theme;
}
type DrawerState = {
    is_open: boolean;
}

export const TemporaryDrawer = withTheme(withStyles(
    class extends Component<DrawerProps, DrawerState>
    {
        constructor(props: DrawerProps) {
            super(props);
            this.state = { is_open: props.is_open };

        }

        toggleDrawer(anchor: Anchor, open: boolean): void {
            this.setState({ is_open: open });
        };


        fireClose() {
            let handler = this.props.onClose;
            if (handler != null) {
                handler();
            }
        }

        render() {
            const classes  = withStyles.getClasses(this.props);
            const theme = this.props.theme;

            return (
                <div>
                    <React.Fragment>
                        <Drawer anchor={this.props.position} open={this.props.is_open} onClose={() => { this.fireClose(); }} >
                            <div 
                                className={this.props.position === 'top' || this.props.position === 'bottom' ? classes.fullList : classes.list}
                                role="presentation"
                                onClick={() => { this.fireClose(); }}
                                onKeyDown={() => { this.fireClose(); }}
                            >
                                <div className={classes.drawer_header} style={{ display: "flex", flexFlow: "row nowrap", justifyContent: "flex-start", alignItems: "center", width: "100%", gap: 4}}>

                                    <IconButtonEx tooltip="Back"
                                         style={{ flex: "0 0 auto", color: theme.palette.text.secondary }} >
                                        <ArrowBackIcon />
                                    </IconButtonEx>
                                    <div style={{ flex: "1 1 auto", display: "flex", justifyContent: "center", minWidth: 0 }}>
                                        <img src="img/Pi-Logo-3.png" alt="PiPedal" style={{height: 32, maxWidth: "100%", objectFit: "contain", filter: isDarkMode() ? 'drop-shadow(0 0 8px rgba(167,112,228,0.35))' : 'none'}} />
                                    </div>
                                    <div style={{ flex: "0 0 40px" }} />
                                </div>
                                {this.props.children}
                            </div>
                        </Drawer>
                    </React.Fragment>
                </div >
            );
        }
    },
    drawerStyles
));
