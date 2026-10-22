find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_library(ldm_storage STATIC
    Migrations.cpp
    Migrations.h
    Sqlite.cpp
    Sqlite.h
    SqliteStore.cpp
    SqliteStore.h)
target_link_libraries(ldm_storage
    PUBLIC ldm_core
    PRIVATE unofficial::sqlite3::sqlite3)
