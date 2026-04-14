"""In-memory model used by the LNM editor."""

from __future__ import annotations

import copy
import itertools
from dataclasses import dataclass, field

from .constants import FIELD_ORDER

_UID_COUNTER = itertools.count(1)


def new_uid() -> str:
    """Return a stable unique node identifier."""
    return f"node-{next(_UID_COUNTER)}"


@dataclass
class ValueEntry:
    """One ``<value>`` element for constants and variables."""

    value: str = ""
    index: str = ""


@dataclass
class EditorNode:
    """Generic tree node for LNM entries, plugins, and parameters."""

    tag: str
    uid: str = field(default_factory=new_uid)
    attributes: dict[str, str] = field(default_factory=dict)
    fields: dict[str, str] = field(default_factory=dict)
    values: list[ValueEntry] = field(default_factory=list)
    children: list["EditorNode"] = field(default_factory=list)

    def display_name(self) -> str:
        """Return the user-visible label shown in the tree."""
        if self.tag == "logicalNameMap":
            return "logicalNameMap"
        if self.tag == "module":
            return f"module: {self.attributes.get('name', '<unnamed>')}"
        if self.tag == "plugin":
            return f"plugin: {self.attributes.get('name', '<unnamed>')}"
        if self.tag == "parameter":
            return f"parameter: {self.attributes.get('name', '<unnamed>')}"
        if self.tag in FIELD_ORDER:
            return f"{self.tag}: {self.attributes.get('name', '<unnamed>')}"
        return self.tag


@dataclass
class DocumentModel:
    """Root document wrapper."""

    root: EditorNode = field(default_factory=lambda: EditorNode(tag="logicalNameMap"))

    def clone(self) -> "DocumentModel":
        """Return a deep copy for undo/redo snapshots."""
        return copy.deepcopy(self)


def walk(node: EditorNode):
    """Yield the node and all descendants in pre-order."""
    yield node
    for child in node.children:
        yield from walk(child)
