"""Route one Kif House Telegram topic through the local Qwen profile."""

from pathlib import Path

ROUTE_NAME = "telegram-qwen"


def route_from_house(house):
    """Return the Qwen topic route, or None when that topic is undeclared."""
    if not isinstance(house, dict):
        return None
    topics = house.get("topics") or {}
    if "Qwen" not in topics or "group_id" not in house:
        return None
    return {
        "name": ROUTE_NAME,
        "platform": "telegram",
        "chat_id": str(house["group_id"]),
        "thread_id": str(topics["Qwen"]),
        "profile": "qwen",
        "enabled": True,
    }


def _same_route(route, wanted):
    return (
        isinstance(route, dict)
        and route.get("name") == wanted["name"]
        and route.get("platform") == wanted["platform"]
        and str(route.get("chat_id") or "") == wanted["chat_id"]
        and str(route.get("thread_id") or "") == wanted["thread_id"]
        and route.get("profile") == wanted["profile"]
        and route.get("enabled", True)
    )


def merge_routes(config, house=None):
    """Return (config, changed) with multiplex routing for the Qwen topic."""
    if not isinstance(config, dict):
        raise ValueError("config must be a mapping")
    gateway = config.get("gateway")
    if gateway is None:
        gateway = {}
        config["gateway"] = gateway
    if not isinstance(gateway, dict):
        raise ValueError("gateway must be a mapping")
    wanted = route_from_house(house)
    routes = list(config.get("profile_routes") or [])
    if wanted is None:
        kept = [
            route
            for route in routes
            if not (isinstance(route, dict) and route.get("name") == ROUTE_NAME)
        ]
        if kept == routes:
            return config, False
        config["profile_routes"] = kept
        gateway["profile_routes"] = [dict(route) for route in kept]
        return config, True
    if (
        config.get("multiplex_profiles") is True
        and gateway.get("multiplex_profiles") is True
        and any(_same_route(route, wanted) for route in routes)
    ):
        return config, False
    config["multiplex_profiles"] = True
    gateway["multiplex_profiles"] = True
    routes = [
        route
        for route in routes
        if not (isinstance(route, dict) and route.get("name") == ROUTE_NAME)
    ]
    routes.append(dict(wanted))
    config["profile_routes"] = routes
    gateway["profile_routes"] = [dict(route) for route in routes]
    return config, True


def merge_into_config_file(path, house=None):
    """Idempotently merge routing into an existing Hermes config.yaml."""
    import yaml
    from hermes_declaration import atomic_write

    path = Path(path)
    if not path.is_file():
        return False
    loaded = yaml.safe_load(path.read_bytes())
    config, changed = merge_routes(
        {} if loaded is None else loaded, house=house
    )
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
