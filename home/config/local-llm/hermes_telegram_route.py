"""Route Hermes Telegram through the local Qwen profile."""

from pathlib import Path

ROUTE = {
    "name": "telegram-qwen",
    "platform": "telegram",
    "profile": "qwen",
    "enabled": True,
}


def _has_route(routes):
    return any(
        isinstance(route, dict)
        and route.get("name") == ROUTE["name"]
        and route.get("platform") == ROUTE["platform"]
        and route.get("profile") == ROUTE["profile"]
        and route.get("enabled", True)
        for route in routes
    )


def merge_routes(config):
    """Return (config, changed) with multiplex Telegram routing enabled."""
    if not isinstance(config, dict):
        raise ValueError("config must be a mapping")
    gateway = config.get("gateway")
    if gateway is None:
        gateway = {}
        config["gateway"] = gateway
    if not isinstance(gateway, dict):
        raise ValueError("gateway must be a mapping")
    routes = list(config.get("profile_routes") or [])
    if (
        config.get("multiplex_profiles") is True
        and gateway.get("multiplex_profiles") is True
        and _has_route(routes)
    ):
        return config, False
    config["multiplex_profiles"] = True
    gateway["multiplex_profiles"] = True
    routes = [
        route
        for route in routes
        if not (isinstance(route, dict) and route.get("name") == ROUTE["name"])
    ]
    routes.append(dict(ROUTE))
    config["profile_routes"] = routes
    gateway["profile_routes"] = [dict(route) for route in routes]
    return config, True


def merge_into_config_file(path):
    """Idempotently merge routing into an existing Hermes config.yaml."""
    import yaml
    from hermes_declaration import atomic_write

    path = Path(path)
    if not path.is_file():
        return False
    loaded = yaml.safe_load(path.read_bytes())
    config, changed = merge_routes({} if loaded is None else loaded)
    if not changed:
        return False
    atomic_write(
        path,
        yaml.safe_dump(
            config,
            sort_keys=False,
            default_flow_style=False,
            allow_unicode=True,
        ).encode(),
    )
    return True
