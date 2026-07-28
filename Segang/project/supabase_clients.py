class OAuthPkceStorage:
    def __init__(self):
        self._values = {}

    def get_item(self, key):
        return self._values.get(key)

    def set_item(self, key, value):
        self._values[key] = value

    def remove_item(self, key):
        self._values.pop(key, None)

    def code_verifier(self):
        for key, value in self._values.items():
            if key.endswith("-code-verifier"):
                return value
        return None


def create_machine_client(url, key, factory=None):
    if factory is None:
        from supabase import create_client
        factory = create_client
    client_factory = factory
    return client_factory(url, key)


def create_oauth_client(
    url,
    key,
    factory=None,
    storage=None,
    options_factory=None,
):
    if factory is None:
        from supabase import ClientOptions, create_client
        factory = create_client
        if options_factory is None:
            options_factory = ClientOptions
    if storage is None:
        return factory(url, key)
    if options_factory is None:
        raise ValueError(
            "options_factory is required with injected factory and storage"
        )
    return factory(url, key, options=options_factory(storage=storage))
