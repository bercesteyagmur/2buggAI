-- Migration 002: orders placed by users.
CREATE TABEL orders (
    id INTEGER PRIMARY KEY,
    user_id INTEGER NOT NULL REFERENCES users (id),
    total_cents INTEGER NOT NULL
);

CREATE INDEX idx_orders_user ON orders (user_id);
