# LDM - Modern Download Manager

A modern, feature-rich download manager built with C++ and wxWidgets.

![LDM Banner](LDM/resources/LDM%20Github%20Readme.png)

## Screenshot

![LDM Interface](LDM/resources/Interface.png)

## Features

### Core Download Engine
- **Multi-Segment Downloads** - Accelerated downloading with parallel connections using WinINet
- **Auto-Retry & Resume** - Automatic retry on network failures with exponential backoff; interrupted downloads resume from where they left off
- **Checksum Verification** - MD5 and SHA256 hash verification for downloaded files
- **Crash Recovery** - Periodic database saves ensure progress is preserved even after unexpected shutdowns

### Video Site Support
- **yt-dlp Integration** - Download videos from YouTube, Vimeo, and 1000+ supported sites
- **Automatic Tool Management** - Auto-downloads yt-dlp, ffmpeg, and Deno runtime as needed
- **Video Quality Selection** - Choose your preferred resolution and format before downloading
- **Protected Video Support** - Referer header propagation for CDN-authenticated video streams

### User Interface
- **Real-time Speed Graph** - Visual download speed monitoring with gradient visualization
- **Category Management** - Automatic file categorization (Documents, Videos, Music, Images, Programs, Compressed)
- **Scheduler** - Schedule downloads to start/stop at specific times
- **Modern UI** - Clean interface with Dark Mode support
- **System Tray Integration** - Minimize to system tray with notification support
- **Detailed Error Reporting** - Human-readable error messages for network failures

### Browser Integration
- **Chrome/Edge/Firefox Extension** - Seamlessly intercept and send downloads to LDM
- **Context Menu Integration** - Right-click any link, image, or video to download
- **Batch Downloads** - Download all links or media from a page with one click
- **Video Detection** - Automatic detection of video elements on web pages
- **Protected Downloads** - Passes referer headers for sites requiring authentication

## Browser Extension

LDM includes a browser extension that automatically intercepts downloads and sends them to LDM for accelerated downloading.

### Installation

1. **Make sure LDM is running** - The extension communicates with LDM via a local HTTP server
2. Open your browser's extensions page:
   - **Chrome**: `chrome://extensions`
   - **Edge**: `edge://extensions`
   - **Brave**: `brave://extensions`
   - **Firefox**: `about:debugging#/runtime/this-firefox`
3. Enable **Developer mode** (toggle in top-right corner)
4. Click **Load unpacked** (Chrome/Edge) or **Load Temporary Add-on** (Firefox)
5. Select the `BrowserExtension` folder from your LDM installation

### Extension Features

- **Auto-intercept** - Automatically catches browser downloads and sends them to LDM
- **Context Menu** - Right-click any link → "Download with LDM"
- **Video Tab** - Scan pages for video elements and download with quality selection
- **Batch Downloads** - "Download All Links" and "Download All Media" options
- **Manual Download** - Paste URLs directly in the extension popup
- **Connection Status** - Real-time indicator showing LDM connection state
- **Keyboard Shortcuts** - Configurable hotkeys for quick actions
- **Configurable Filters** - Exclude file types, domains, or minimum file sizes

## Requirements

- **Windows 10/11**
- **Visual Studio 2022** with "Desktop development with C++" workload
- **wxWidgets 3.2+**

## Dependencies

This project uses **wxWidgets** for the UI and native Windows APIs (**WinINet**) for networking. No external libcurl dependency is required.

### Video Download Dependencies (Auto-managed)

LDM automatically downloads and manages these tools when needed:
- **yt-dlp** - Video extraction and downloading
- **ffmpeg** - Audio/video processing and merging
- **Deno** - JavaScript runtime for certain extractors (YouTube)

Tools are stored in `%APPDATA%\LDM\tools\` and updated automatically.

### Setting up wxWidgets

1. Download and build wxWidgets from [wxwidgets.org](https://www.wxwidgets.org/downloads/).
2. Set the `WXWIN` environment variable to your wxWidgets installation directory.
3. The project is configured to look for libraries in `$(WXWIN)\lib\vc_x64_lib`.

## Building

### Using Visual Studio

1. Open `LDM.sln` in Visual Studio 2022.
2. Select the **Debug** or **Release** configuration and **x64** platform.
3. Build the solution (**Ctrl+Shift+B**).

### Debug Mode

To enable debug console output:
- Run with `--debug` command line flag, or
- Create an empty `debug.txt` file in the application directory

## Project Structure

```
Last-Download-Manager/
├── LDM.sln                  # Visual Studio Solution
├── BrowserExtension/        # Chrome/Firefox extension
│   ├── manifest.json            # Extension manifest (v3)
│   ├── background.js            # Service worker
│   ├── content.js               # Content script for video detection
│   ├── popup/                   # Extension popup UI
│   └── options/                 # Extension settings page
├── LDM/                     # Main application
│   ├── main.cpp                 # Application entry point
│   ├── core/                    # Download engine
│   │   ├── Download.cpp/h           # Download data model
│   │   ├── DownloadEngine.cpp/h     # Network operations & retry logic
│   │   ├── DownloadManager.cpp/h    # Queue management
│   │   └── YtDlpManager.cpp/h       # Video site integration
│   ├── ui/                      # User interface (wxWidgets)
│   │   ├── MainWindow.cpp/h         # Main application window
│   │   ├── DownloadsTable.cpp/h     # Download list view
│   │   ├── CategoriesPanel.cpp/h    # Category sidebar
│   │   ├── VideoQualityDialog.cpp/h # Quality selection dialog
│   │   └── SpeedGraphPanel.cpp/h    # Speed visualization
│   ├── database/                # XML-based data persistence
│   ├── utils/                   # Utilities (settings, themes, HTTP server)
│   └── resources/               # Icons, manifests, and assets
└── bin/                         # Compiled binaries output
```

## Technical Details

### Retry Strategy

LDM implements a two-level retry system:

1. **Chunk-Level Retry** - Each download segment retries up to 3 times with 500ms base delay
2. **Download-Level Retry** - The entire download retries up to 5 times with exponential backoff (2-32 seconds)

### Resume Capability

- Part files (`.part0`, `.part1`, etc.) are preserved on failure
- Downloads can be resumed manually even after all automatic retries are exhausted
- Content-Range headers are validated to prevent data corruption
- Existing file size is checked on application restart for accurate resume

### Thread Safety

- Atomic operations for status, sizes, and speed values
- Mutex protection for metadata, chunks, and database operations
- RAII-style session management for WinINet handles
- Tracked async tasks with proper lifecycle management

### Browser Extension Communication

- Local HTTP server on port 45678
- Token-based authentication for security
- JSON API for download requests and status queries
- Referer header propagation for protected content

## License

This project is open source. Feel free to use and modify.

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.
