# OmniRDP Current State

This repository contains the **OmniRDP RDP multiplexer** with a **Windows service manager** and **system tray app**.

## What is active in this checkout

- `OmniRDP/` is the active project source tree (multiplexer + service manager + tray app).
- `freerdp-3.26.0/` is a vendored upstream dependency used by the active project.
- `archive/` preserves prior plans, architecture notes, test scripts, and the earlier proxy-plugin direction.

## What the project does

### Core Multiplexer
A standalone N:1 RDP multiplexer — one backend RDP session fanned out to multiple viewers.

### Service Manager (NEW)
A Windows service (`OmniRDP-svc.exe`) that manages multiple backend instances. Each instance is a child process (`OmniRDP.exe --instance`) with its own backend connection and viewer listener.

### Tray App (NEW)
A system tray application (`OmniRDP-tray.exe`) that connects to the service via named pipe IPC. Shows instance status, provides context menu controls, and includes a status window and log viewer.

## Current build status

- **OmniRDP.exe**: Builds and links successfully with FreeRDP 3.26.0 ✅
- **OmniRDP-svc.exe**: Builds and links successfully ✅
- **OmniRDP-tray.exe**: Builds and links successfully ✅

## Known issues

- FreeRDP 3.26.0 generated headers not committed; build requires local FreeRDP build tree
- Some off-by-one JSON parsing bugs fixed (name, backend_hostname)
- WLog output captured via FILE appender in service mode; rich logs now available in `viewer.log`

## Repository Layout

```
OmniRDP/              — Active project source (29 files)
freerdp-3.26.0/       — Vendored FreeRDP dependency
archive/              — Historical plans and notes
```
