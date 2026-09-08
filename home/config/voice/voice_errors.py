"""Shared errors for local voice transports."""


class DeliveryUncertain(RuntimeError):
    """Input might have arrived; an automatic retry could duplicate it."""
