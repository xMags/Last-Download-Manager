# LastDM - Modern Download Manager

A modern, feature-rich download manager built with C++ and wxWidgets.

![LastDM Banner](LastDM/resources/LDM%20Github%20Readme.png)

## Screenshot

![LastDM Interface](LastDM/resources/Interface.png)

## Features

- 🚀 **Multi-threaded Downloads** - Fast parallel downloading with multiple connections (using WinINet)
- 📊 **Real-time Speed Graph** - Visual download speed monitoring
- 📁 **Category Management** - Organize downloads by type (Documents, Videos, Music, etc.)
- ⏰ **Scheduler** - Schedule downloads for specific times
- ✅ **Checksum Verification** - MD5/SHA256 hash verification
- 🎨 **Modern UI** - Clean and intuitive interface with Dark Mode support
- 🔔 **System Tray** - Minimize to system tray with notifications
- 💾 **Persistent Downloads** - Resume interrupted downloads (XML-based storage)

## Requirements

- **Windows 10/11**
- **Visual Studio 2022** with "Desktop development with C++" workload
- **wxWidgets 3.2+**

## Dependencies

This project primarily depends on **wxWidgets**. It uses native Windows APIs (**WinINet**) for networking, so no external CURL dependency is required.

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
│   ├── core/               # Download engine (WinINet)
│   ├── ui/                 # User interface components (wxWidgets)
│   ├── database/           # XML-based data persistence
│   ├── utils/              # Utilities (settings, themes, hash)
│   └── resources/          # Icons, manifests, and assets
└── bin/                    # Compiled binaries output
```

## License

This project is open source. Feel free to use and modify.

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.