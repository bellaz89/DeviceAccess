"""Constants shared by the LNM editor modules."""

from __future__ import annotations

import pathlib

PACKAGE_ROOT = pathlib.Path(__file__).resolve().parent
SCHEMA_PATH = PACKAGE_ROOT / "xmlschema" / "logicalNameMap.xsd"

PLUGIN_NAMES = (
    "multiply",
    "math",
    "monostableTrigger",
    "forceReadOnly",
    "forcePollingRead",
    "typeHintModifier",
    "doubleBuffer",
    "bitRange",
    "tagModifier",
    "isStatusOutput",
    "hasReverseRecovery",
    "fanOut",
)

TYPE_NAMES = (
    "int8",
    "uint8",
    "int16",
    "uint16",
    "int32",
    "uint32",
    "int64",
    "uint64",
    "float32",
    "float64",
    "string",
    "Boolean",
    "Void",
    "integer",
)

ROOT_CHILD_TAGS = (
    "module",
    "redirectedRegister",
    "redirectedChannel",
    "redirectedBit",
    "constant",
    "variable",
)

REGISTER_TAGS = (
    "redirectedRegister",
    "redirectedChannel",
    "redirectedBit",
    "constant",
    "variable",
)

FIELD_ORDER = {
    "redirectedRegister": ("targetDevice", "targetRegister", "targetStartIndex", "numberOfElements"),
    "redirectedChannel": ("targetDevice", "targetRegister", "targetChannel", "targetStartIndex", "numberOfElements"),
    "redirectedBit": ("targetDevice", "targetRegister", "targetBit"),
    "constant": ("type", "numberOfElements"),
    "variable": ("type", "numberOfElements"),
    "parameter": ("content",),
}

FIELD_LABELS = {
    "targetDevice": "Target device",
    "targetRegister": "Target register",
    "targetStartIndex": "Target start index",
    "numberOfElements": "Number of elements",
    "targetChannel": "Target channel",
    "targetBit": "Target bit",
    "type": "Type",
    "content": "Value",
}

FIELD_HINTS = {
    "targetDevice": "Supports raw text and mixed XML fragments such as <par>...</par>.",
    "targetRegister": "Supports raw text and mixed XML fragments such as <ref>...</ref>.",
    "targetStartIndex": "Optional numeric expression or mixed XML fragment.",
    "numberOfElements": "Optional numeric expression or mixed XML fragment.",
    "targetChannel": "Numeric expression or mixed XML fragment.",
    "targetBit": "Numeric expression or mixed XML fragment.",
    "type": "One of the DeviceAccess type names accepted by DataType.",
    "content": "Raw text or XML fragment. Parameters support <ref> and <par> content.",
}

ADDABLE_CHILDREN = {
    "logicalNameMap": ROOT_CHILD_TAGS,
    "module": ROOT_CHILD_TAGS,
    "redirectedRegister": ("plugin",),
    "redirectedChannel": ("plugin",),
    "redirectedBit": ("plugin",),
    "constant": ("plugin",),
    "variable": ("plugin",),
    "plugin": ("parameter",),
}
