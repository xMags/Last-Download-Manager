# Last Download Manager

A native Windows download manager written in C++ with plain Win32. It aims to be a full replacement for the reference download manager: the same window layout and workflows, multi-connection downloads, browser capture, media capture, queues, scheduling and a site grabber, with its own branding and artwork.

**Status: complete rewrite in progress.** The previous wxWidgets version has been retired. Work is delivered in milestones:

| Milestone | Scope | State |
|---|---|---|
| 1 | Reference study and parity checklist | Done |
| 2 | Native shell and reliable downloads | Done |
| 3 | Browser integration and media capture | Planned |
| 4 | Queues, scheduler, organization and Site Grabber | Planned |
| 5 | Packaging and installer | Planned |

Design documents:

- [the reference parity checklist](docs/rewrite/parity-checklist.md): what "replacement" means, item by item.
- [Milestone 2 technical design](docs/rewrite/m2-technical-design.md): modules, threads, database and build.
- [Milestone 3 technical design](docs/rewrite/m3-technical-design.md): browser extensions, native messaging host and media capture.

## Requirements

- Windows 10 or 11, x64
- Visual Studio 2026 (version 18) with the "Desktop development with C++" workload, which includes MSVC, CMake, Ninja and vcpkg
- Node.js 22 or newer, only for the integration tests

## Building

Open a "Developer PowerShell for VS" (or "x64 Native Tools Command Prompt") so the compiler, CMake, Ninja and vcpkg are on `PATH`, then run from the repository root:

```
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

Dependencies (libcurl, SQLite, nlohmann-json, GoogleTest) are fetched and built by vcpkg on the first configure, which takes several minutes. The program is written to `build/windows-release/src/app/ldm.exe`, with the browser integration host `ldm-host.exe` beside it.

Use the `windows-debug` preset for a debug build.

## License

GPL-3.0. See [LICENSE](LICENSE).
