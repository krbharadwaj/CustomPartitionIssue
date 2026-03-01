# Partition + NewWindow Demo

Win32 WebView2 app demonstrating how `CustomDataPartitionId` interacts with
`NewWindowRequested` and `put_NewWindow` — reproducing the partition mismatch
bug where blob URLs fail across parent/popup windows.

## The Problem

1. Parent WebView navigates to `about:blank`
2. `put_CustomDataPartitionId("demoPartition")` is called **after** navigation completes
3. The partition does **not** apply to the parent's own storage (remains `(default)`)
4. But it **propagates** to child frames and NewWindows created from this WebView
5. `window.open()` → popup may get a different effective partition
6. Blob URLs created in parent (`(default)` partition) **fail** in popup (`demoPartition`)

## 8 Scenarios

| # | window.open URL | WebView Instance | Child Partition | Partition Match? |
|---|----------------|-----------------|----------------|-----------------|
| 1 | about:blank    | New             | demoPartition  | ❌ Mismatch      |
| 2 | about:blank    | New             | (default)      | ✅ Match         |
| 3 | about:blank    | Parent (reuse)  | inherits       | ✅ Match         |
| 4 | Actual URL     | New             | demoPartition  | ❌ Mismatch      |
| 5 | Actual URL     | New             | (default)      | ✅ Match         |
| 6 | Actual URL     | Parent (reuse)  | inherits       | ✅ Match         |
| 7 | about:blank    | New             | otherPartition | ❌ Mismatch      |
| 8 | Actual URL     | New             | otherPartition | ❌ Mismatch      |

All scenarios use the full `window.open()` → `NewWindowRequested` → `put_NewWindow` flow.

## What Each Window Shows

- **Parent partition** vs **child partition** comparison
- Match/mismatch verdict with color coding
- Whether blob URLs would work or fail
- Explanation of the specific scenario configuration

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
├── main.cpp      # Win32 app — all C++ logic (~750 lines)
├── build.bat     # One-step build script
├── setup.bat     # Downloads WebView2 NuGet package
├── .gitignore
└── README.md
```

## Key APIs Used

- `ICoreWebView2::add_NewWindowRequested` — intercepts `window.open()` popups
- `ICoreWebView2NewWindowRequestedEventArgs::put_NewWindow` — redirects popup to existing WebView
- `put_CustomDataPartitionId` / `get_CustomDataPartitionId` — experimental partition API (`ICoreWebView2Experimental20`)
- `ICoreWebView2::ExecuteScript` — injects UI into about:blank pages

## Requirements

- Windows 10+
- WebView2 Runtime (Edge Canary for experimental API support)
- `CustomDataPartitionId` requires experimental WebView2 API (`5a4d0ecf-3fe5-4456-ace5-d317cca0eff1`)
