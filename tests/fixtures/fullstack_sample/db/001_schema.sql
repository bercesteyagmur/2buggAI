-- Initial schema (SQLite).
CREATE TABLE users (
    id INTEGER PRIMARY KEY,
    full_name TEXT NOT NULL,
    email TEXT NOT NULL UNIQUE
);
