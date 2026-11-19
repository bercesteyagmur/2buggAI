from app.utils import slugify


def test_slugify_joins_words_with_dash():
    assert slugify("Hello World") == "hello-world"
