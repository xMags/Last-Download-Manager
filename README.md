# LastDM - Modern Download Manager

A modern, feature-rich download manager built with C++ and wxWidgets.

![LastDM Banner](LastDM/resources/LDM%20Github%20Readme.png)

## Screenshot

![LastDM Interface](LastDM/resources/Interface.png)

## Features

- **Multi-Segment Downloads** - Accelerated downloading with parallel connections using WinINet
- **Auto-Retry & Resume** - Automatic retry on network failures with exponential backoff; interrupted downloads resume from where they left off
- **Real-time Speed Graph** - Visual download speed monitoring with gradient visualization
- **Category Management** - Automatic file categorization (Documents, Videos, Music, Images, Programs, Compressed)
- **Scheduler** - Schedule downloads to start/stop at specific times
- **Checksum Verification** - MD5 and SHA256 hash verification for downloaded files
- **Modern UI** - Clean interface with Dark Mode support
- **System Tray Integration** - Minimize to system tray with notification support
- **Crash Recovery** - Periodic database saves ensure progress is preserved even after unexpected shutdowns
- **Detailed Error Reporting** - Human-readable error messages for network failures

## Requirements

- **Windows 10/11**
- **Visual Studio 2022** with "Desktop development with C++" workload
- **wxWidgets 3.2+**

## Dependencies

This project uses **wxWidgets** for the UI and native Windows APIs (**WinINet**) for networking. No external libcurl dependency is required.

### Setting up wxWidgets

1. Download and build wxWidgets from [wxwidgets.org](https://www.wxwidgets.org/downloads/).
2. Set the `WXWIN` environment variable to your wxWidgets installation directory.
3. The project is configured to look for libraries in `$(WXWIN)\lib\vc_x64_lib`.

## Building

### Using Visual Studio

1. Open `LastDM.sln` in Visual Studio 2022.
2. Select the **Debug** or **Release** configuration and **x64** platform.
3. Build the solution (**Ctrl+Shift+B**).

## Project Structure

```
LastDM-Download-Manager/
├── LastDM.sln              # Visual Studio Solution
├── LastDM/                 # Main project directory
│   ├── main.cpp            # Application entry point
│   ├── core/               # Download engine (WinINet-based)
│   │   ├── Download.cpp/h        # Download data model
│   │   ├── DownloadEngine.cpp/h  # Network operations & retry logic
│   │   └── DownloadManager.cpp/h # Queue management
│   ├── ui/                 # User interface components (wxWidgets)
│   │   ├── MainWindow.cpp/h      # Main application window
│   │   ├── DownloadsTable.cpp/h  # Download list view
│   │   ├── CategoriesPanel.cpp/h # Category sidebar
│   │   └── SpeedGraphPanel.cpp/h # Speed visualization
│   ├── database/           # SQLite-based data persistence
│   ├── utils/              # Utilities (settings, themes, hash)
│   └── resources/          # Icons, manifests, and assets
└── bin/                    # Compiled binaries output
```

## Technical Details

### Retry Strategy

LastDM implements a two-level retry system:

1. **Chunk-Level Retry** - Each download segment retries up to 3 times with 500ms base delay
2. **Download-Level Retry** - The entire download retries up to 5 times with exponential backoff (2-32 seconds)

### Resume Capability

- Part files (`.part0`, `.part1`, etc.) are preserved on failure
- Downloads can be resumed manually even after all automatic retries are exhausted
- Content-Range headers are validated to prevent data corruption

### Thread Safety

- Atomic operations for status, sizes, and speed values
- Mutex protection for metadata, chunks, and database operations
- RAII-style session management for WinINet handles

## License

This project is open source. Feel free to use and modify.

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.
