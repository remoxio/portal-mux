# Icons

The app's icons; Material Icon Theme and Tabler are the same sets as the author's other apps (spark-commander,
Water Data Studio). All are compiled in as Qt resources (`:/icons/...`, see `src/app/Icons.h`).

## material-icon-theme/: Material Icon Theme 5.40.0 (MIT)

Coloured folder icons for the connection groups, unmodified, from the VS Code extension
`PKief.material-icon-theme` 5.40.0 (https://github.com/material-extensions/vscode-material-icon-theme), as
bundled in spark-commander. `LICENSE.txt`: MIT. The icons draw on Material Design Icons and Material Symbols
(Apache 2.0).

## tabler/: Tabler Icons 3.52.0 (MIT)

Monochrome line icons for actions, protocols and VM states, unmodified, from
https://github.com/tabler/tabler-icons/tree/v3.52.0/icons/outline. `LICENSE`: MIT. They draw with
`currentColor`, which Portal Mux replaces with the theme's text colour (or a state colour) when it renders them.

## lucide/: Lucide 1.62.0 (ISC; MIT for the chevrons and the lock)

`network.svg`, `monitor-cog.svg` and `panels-top-left.svg` (the Connections, KVM Manager and Sessions views), `server.svg`
(Settings → SSH Servers), `lock.svg` (Settings → Passwords) and
`chevron-down.svg`, `chevron-up.svg` (folding the Proxmox view's panels), unmodified, from
https://github.com/lucide-icons/lucide/tree/1.62.0/icons. `LICENSE`: ISC, and MIT for its Feather-derived icons,
the chevrons and the lock among them. Drawn with `currentColor` like the Tabler icons.

## breeze/, breeze-dark/: Breeze 6.32.0 (LGPL 3.0 or later), macOS only

The theme icons the app asks for (`QIcon::fromTheme`), at 16, 22 and 32 px, from KDE's breeze-icons 6.32.0 `icons/`
(https://download.kde.org/stable/frameworks/6.32/), so macOS shows the icons Linux and Windows do. `breeze-dark` is made
as breeze-icons' own `generate-symbolic-dark` does: the stylesheet's Text colour `#fcfcfc`, Background `#2a2e32`.
Each folder has a short `index.theme`; `breeze/COPYING-ICONS` is the licence.

## brands/: the Proxmox mark

`proxmox-line.svg` (the sidebar's Proxmox tab and the server rows) is drawn for Portal Mux: the logo's four chevrons
as 2 px lines on Lucide's grid, so it matches the Lucide icons beside it; `currentColor`, like the others.
Proxmox® is a registered trademark of Proxmox Server Solutions GmbH; the About dialog says so. Its media kit and
usage guidelines: https://www.proxmox.com/en/news/media-kit.
