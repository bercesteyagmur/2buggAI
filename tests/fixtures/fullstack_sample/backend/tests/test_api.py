from app.main import create_app


def test_list_users_returns_full_name():
    client = create_app().test_client()
    response = client.get("/api/users")
    assert response.status_code == 200
    assert response.get_json()[0]["full_name"] == "Ada Lovelace"
