"""Patch an XBE before it is lifted, and tell the runtime how (docs/technical/xbe-patches.md)."""

from .patcher import PatchError, apply_patches, load_spec, render_c  # noqa: F401
