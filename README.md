# Partition + NewWindow Demo

Win32 WebView2 app demonstrating how `CustomDataPartitionId` interacts with
`NewWindowRequested` and `put_NewWindow` — reproducing the partition mismatch
bug (Bug 59916965) where blob URLs fail across parent/popup windows.

## The Problem

When a WebView2 host app calls `put_CustomDataPartitionId` on a parent WebView,
the effective storage partition depends on **when** the partition is set relative
to navigation. If the popup window ends up with a different effective partition
than the parent, blob URLs created in one partition **fail** in the other.

## 21 Scenarios

The app tests 7 partition configurations across 3 different timing patterns:

### Partition Configurations (7 per section)

| Sub # | Child Partition Set | Description |
|-------|--------------------|--------------------------------------------|
| 1     | (none)             | New WebView, no partition set               |
| 2     | `demoPartition`    | New WebView, same partition as parent       |
| 3     | `otherPartition`   | New WebView, different partition             |
| 4     | `""` (default)     | New WebView, explicitly set to default       |
| 5     | (unchanged)        | Parent WebView reused via put_NewWindow     |
| 6     | `""` (default)     | Parent reused, reset to default              |
| 7     | `otherPartition`   | Parent reused, changed to different          |

### Three Timing Sections

#### ① Partition AFTER Navigate (S1–S7)
Parent calls `Navigate(about:blank)` → waits for `NavigationCompleted` → **then**
calls `put_CustomDataPartitionId("demoPartition")`.

This is the most common pattern. The partition is set after the initial navigation
completes. The parent's effective storage remains `(default)` because the partition
was set after the page loaded.

#### ② Partition BEFORE Navigate (S15–S21)
Parent calls `put_CustomDataPartitionId("demoPartition")` **first** → **then**
calls `Navigate(about:blank)` → waits for `NavigationCompleted`.

Tests whether setting the partition before navigation causes the subsequent
`about:blank` navigation to use the custom partition.

#### ③ Implicit Navigation (S8–S14)
Parent does **NOT** call `Navigate(about:blank)`. Relies on WebView2's automatic
`about:blank` navigation that occurs during controller creation. Partition is set
directly in the creation callback, then `window.open()` is called immediately.

Tests whether the auto-navigation timing affects partition behavior.

### Full Scenario Matrix

| Scenario | Timing          | WebView     | Child Partition    |
|----------|-----------------|-------------|--------------------|
| S1       | After Nav       | New         | (none)             |
| S2       | After Nav       | New         | demoPartition      |
| S3       | After Nav       | New         | otherPartition     |
| S4       | After Nav       | New         | (default)          |
| S5       | After Nav       | Reused      | (unchanged)        |
| S6       | After Nav       | Reused      | (default)          |
| S7       | After Nav       | Reused      | otherPartition     |
| S8       | Implicit        | New         | (none)             |
| S9       | Implicit        | New         | demoPartition      |
| S10      | Implicit        | New         | otherPartition     |
| S11      | Implicit        | New         | (default)          |
| S12      | Implicit        | Reused      | (unchanged)        |
| S13      | Implicit        | Reused      | (default)          |
| S14      | Implicit        | Reused      | otherPartition     |
| S15      | Before Nav      | New         | (none)             |
| S16      | Before Nav      | New         | demoPartition      |
| S17      | Before Nav      | New         | otherPartition     |
| S18      | Before Nav      | New         | (default)          |
| S19      | Before Nav      | Reused      | (unchanged)        |
| S20      | Before Nav      | Reused      | (default)          |
| S21      | Before Nav      | Reused      | otherPartition     |

## What Each Scenario Shows

Each scenario result displays:
- **Parent effective partition** — always `(default)` since partition is set
  after/around `about:blank` navigation
- **Popup effective partition** — queried via `get_CustomDataPartitionId`
- **Match** — whether popup and parent share the same effective partition
- Color-coded verdict: ✅ same partition (blob URLs work) or ❌ different (blob URLs fail)

## Architecture

- **Launcher**: A separate WebView with clickable buttons for each scenario
- **Each scenario** creates its **own** parent WebView from scratch (fully self-contained)
- Popup is created via `window.open("about:blank")` → `NewWindowRequested` → `put_NewWindow`
- Result UI is injected via `ExecuteScript` after a 300ms timer (to allow `put_NewWindow`
  to finish swapping web contents)
- `GetDeferral()` is used in NWR handlers to create child WebViews asynchronously

## Build

### Prerequisites
- Windows 10+
- Visual Studio 2022+ with C++ Desktop workload
- WebView2 Runtime (Edge Canary recommended for experimental API support)

### Quick Start

```batch
# 1. Download WebView2 SDK
setup.bat

# 2. Build
build.bat

# 3. Run
PartitionDemo.exe
```

### Using Edge Build Headers (alternative)

If you have an Edge Chromium build, set environment variables before building:

```batch
set WV2_HEADERS=path\to\edge\out\gen\edge_embedded_browser\client\win\current
set WV2_LIB=path\to\edge\out
build.bat
```

## Project Structure

```
├── main.cpp      # Win32 app — all C++ logic (~2200 lines, 21 scenarios)
├── build.bat     # One-step build script
├── setup.bat     # Downloads WebView2 NuGet package
├── .gitignore
└── README.md
```

## Key APIs Used

- `ICoreWebView2::add_NewWindowRequested` — intercepts `window.open()` popups
- `ICoreWebView2NewWindowRequestedEventArgs::put_NewWindow` — redirects popup to existing WebView
- `put_CustomDataPartitionId` / `get_CustomDataPartitionId` — experimental partition API
- `ICoreWebView2::ExecuteScript` — injects result UI into about:blank pages
- `ICoreWebView2Deferral` — holds NWR event while creating child WebView asynchronously

## Requirements

- Windows 10+
- WebView2 Runtime (Edge Canary for experimental API support)
- `CustomDataPartitionId` requires experimental WebView2 API (`5a4d0ecf-3fe5-4456-ace5-d317cca0eff1`)
