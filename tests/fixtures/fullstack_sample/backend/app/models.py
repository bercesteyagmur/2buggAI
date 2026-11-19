"""Data model of the sample backend. Mirrors the tables in db/001_schema.sql."""
from dataclasses import dataclass


@dataclass
class User:
    id: int
    full_name: str   # column users.full_name (renamed from "name" in the API v2)
    email: str

    def to_dict(self):
        return {"id": self.id, "full_name": self.full_name, "email": self.email}


USERS = [
    User(1, "Ada Lovelace", "ada@example.com"),
    User(2, "Alan Turing", "alan@example.com"),
]
