add_library(ldm_ui STATIC
    CategoryPane.cpp
    CategoryPane.h
    CommandState.cpp
    CommandState.h
    Dialog.cpp
    Dialog.h
    Dpi.cpp
    Dpi.h
    DownloadListView.cpp
    DownloadListView.h
    FileDialogs.cpp
    FileDialogs.h
    Format.cpp
    Format.h
    GlyphIcons.cpp
    GlyphIcons.h
    Handles.h
    History.cpp
    History.h
    ListModel.cpp
    ListModel.h
    MainController.cpp
    MainController.h
    MainWindow.cpp
    MainWindow.h
    ShellActions.h
    StatusBar.cpp
    StatusBar.h
    Theme.cpp
    Theme.h
    Toolbar.cpp
    Toolbar.h
    TrayIcon.cpp
    TrayIcon.h
    UiBridge.cpp
    UiBridge.h
    dialogs/AddUrlDialog.cpp
    dialogs/AddUrlDialog.h
    dialogs/ConfirmDialogs.cpp
    dialogs/ConfirmDialogs.h
    dialogs/DownloadInfoDialog.cpp
    dialogs/DownloadInfoDialog.h)
target_link_libraries(ldm_ui
    PUBLIC ldm_core
    PRIVATE comctl32 uxtheme dwmapi shell32 comdlg32)
target_include_directories(ldm_ui PRIVATE ${PROJECT_SOURCE_DIR}/resources)
target_compile_definitions(ldm_ui PRIVATE "LDM_VERSION_WIDE=L\"${PROJECT_VERSION}\"")
