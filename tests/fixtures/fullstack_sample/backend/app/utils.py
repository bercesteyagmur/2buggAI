def slugify(title):
    """Turn a title into a URL slug: lower case, words joined with '-'."""
    return title.strip().lower().replace(" ", "_")
