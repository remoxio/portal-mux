# Icons

The app's icons. All are compiled in as Qt resources (`:/icons/...`, see `src/app/Icons.h`).

## material-icon-theme/: Material Icon Theme 5.40.0 (MIT)

Coloured folder icons for the connection groups, unmodified, from the VS Code extension
`PKief.material-icon-theme` 5.40.0 (https://github.com/material-extensions/vscode-material-icon-theme).
`LICENSE.txt`: MIT. The icons draw on Material Design Icons and Material Symbols
(Apache 2.0).

## lucide/: Lucide 1.62.0 (ISC; MIT for its Feather-derived icons)

Every action, protocol, view and VM-state icon, unmodified, from
https://github.com/lucide-icons/lucide/tree/1.62.0/icons. `LICENSE`: ISC, and MIT for the icons derived from Feather.
They draw with `currentColor`, which Portal Mux replaces with the theme's text colour (or a state colour) when it renders
them.

## breeze/, breeze-dark/: Breeze 6.32.0 (LGPL 3.0 or later), macOS only

The theme icons Qt's own widgets ask for (a line edit's clear button, message boxes' icons), at 16, 22 and 32 px, from KDE's breeze-icons 6.32.0 `icons/`
(https://download.kde.org/stable/frameworks/6.32/), so macOS shows the icons Linux and Windows do. `breeze-dark` is made
as breeze-icons' own `generate-symbolic-dark` does: the stylesheet's Text colour `#fcfcfc`, Background `#2a2e32`.
Each folder has a short `index.theme`; `breeze/COPYING-ICONS` is the licence.

## brands/: the Proxmox mark

`proxmox-line.svg` (the sidebar's Proxmox tab and the server rows) is drawn for Portal Mux: the logo's four chevrons
as 2 px lines on Lucide's grid, so it matches the Lucide icons beside it; `currentColor`, like the others.
Proxmox® is a registered trademark of Proxmox Server Solutions GmbH; the About dialog says so. Its media kit and
usage guidelines: https://www.proxmox.com/en/news/media-kit.
