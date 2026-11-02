# AE App for XWindows OS

AE App is a proposed first application port for XWindows OS. The initial goal is an EA account, owned-game library and download client inside the Xbox desktop. It is not yet available to install, and this port would not establish PC game compatibility.

## Source assessment

The assessment uses [`slaveitgr/AE-app-for-Linux` at commit `5189d910fcdd29ea622c6197058c135e0d8f99d8`](https://github.com/slaveitgr/AE-app-for-Linux/tree/5189d910fcdd29ea622c6197058c135e0d8f99d8).

The existing client has a Rust egui/eframe desktop interface with OpenGL rendering and Linux desktop integrations. Its Linux archive contains Linux programs, not a UWP application; the ELF executables cannot run unchanged inside XWindows OS. The existing Windows code also assumes desktop facilities that the Xbox UWP host does not provide. Source reuse requires a port. See the [UI dependencies and platform integrations](https://github.com/slaveitgr/AE-app-for-Linux/blob/5189d910fcdd29ea622c6197058c135e0d8f99d8/aeapp-ui/Cargo.toml).

The useful starting point is its account, library, download and queue logic. The interface already communicates with the backend through explicit requests and responses, which provides a useful boundary for adapting the application to our desktop.

## First working scope

1. Sign in to an EA account through an adapted browser flow.
2. Display the games owned by that account, with accurate loading and error states.
3. Download authorized game content into app-owned storage, with progress, pause/resume and integrity checks supported by the content format.

Login currently opens an external browser and waits for a local TCP callback. The Xbox port needs a tested UWP authentication flow and protected token storage. It must also replace the current desktop filesystem paths, hardware queries and free-space checks with Xbox-compatible access. No credentials or tokens should enter the UI bundle, app catalog or diagnostic reports. See the [existing authentication flow](https://github.com/slaveitgr/AE-app-for-Linux/blob/5189d910fcdd29ea622c6197058c135e0d8f99d8/aeapp-lib/src/core/auth/login.rs#L85).

Downloading content and running it are separate capabilities. The current launcher creates a bootstrap child process and delegates Linux execution to Proton/umu. That launch path cannot be reused unchanged in UWP. A Launch action remains unavailable until our runtime has demonstrated support for the particular program; successful login, download or package installation does not meet that gate. See the [existing game launch path](https://github.com/slaveitgr/AE-app-for-Linux/blob/5189d910fcdd29ea622c6197058c135e0d8f99d8/aeapp-lib/src/core/launch.rs#L342).

## Planned installation experience

The desired flow is **Apps → AE App → Install → Start shortcut**. The Apps catalog and installer do not currently exist. This describes intended behavior, not an implemented or tested feature.

A proposed hosted application bundle would contain the UI, resources and a manifest identifying the app, version, required host version and file digests. Installation would validate that bundle before creating its shortcut. A native backend would ship only after UWP adaptation, a successful Xbox-targeted build and appropriate testing; it may require an XWindows OS host package update. The downloader would not turn arbitrary Linux binaries into executable Xbox apps.

The first installation milestone is a real AE client that can sign in and show the account's library. Downloads follow once storage, cancellation, recovery and integrity handling have been validated. Game execution remains a separate runtime milestone.

## Credits and status

AE App derives from Maxima and is distributed under GPL-3.0. A port must preserve upstream authorship, copyright and license notices, provide corresponding source as required, and review the licenses of any reused resources. See the [AE App credits](https://github.com/slaveitgr/AE-app-for-Linux/blob/5189d910fcdd29ea622c6197058c135e0d8f99d8/CREDITS.md).

This document records a source assessment and proposed scope. No AE App UWP build, installation, account login, download or game launch on Xbox has been demonstrated by this assessment.
