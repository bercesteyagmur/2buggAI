from flask import Flask, jsonify

from app.modles import USERS
from app.utils import slugify


def create_app():
    app = Flask(__name__)

    @app.get("/api/users")
    def list_users():
        return jsonify([user.to_dict() for user in USERS])

    @app.get("/api/users/<int:user_id>/slug")
    def user_slug(user_id):
        for user in USERS:
            if user.id == user_id:
                return jsonify({"slug": slugify(user.full_name)})
        return jsonify({"error": "user not found"}), 404

    return app


if __name__ == "__main__":
    create_app().run(port=5000)
