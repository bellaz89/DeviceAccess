"""Load, save, and validate ChimeraTK LogicalNameMapping xlmap files."""

from __future__ import annotations

import pathlib
import xml.etree.ElementTree as ET

import xmlschema

from .constants import FIELD_ORDER, ROOT_CHILD_TAGS, SCHEMA_PATH
from .model import DocumentModel, EditorNode, ValueEntry


def inner_xml(elem: ET.Element) -> str:
    """Return the mixed-content payload of an element."""
    parts: list[str] = []
    if elem.text:
        parts.append(elem.text)
    for child in list(elem):
        parts.append(ET.tostring(child, encoding="unicode"))
    return "".join(parts).strip()


def set_inner_xml(elem: ET.Element, raw: str) -> None:
    """Set mixed content using a raw XML fragment string."""
    raw = raw.strip()
    elem.text = None
    for child in list(elem):
        elem.remove(child)
    if not raw:
        return
    wrapper = ET.fromstring(f"<wrapper>{raw}</wrapper>")
    elem.text = wrapper.text
    for child in list(wrapper):
        wrapper.remove(child)
        elem.append(child)


def parse_entry(elem: ET.Element) -> EditorNode:
    """Parse one LNM node recursively."""
    tag = elem.tag
    node = EditorNode(tag=tag, attributes=dict(elem.attrib))
    if tag in ("module", "logicalNameMap"):
        node.children = [parse_entry(child) for child in list(elem) if child.tag in ROOT_CHILD_TAGS]
        return node
    if tag in ("redirectedRegister", "redirectedChannel", "redirectedBit"):
        for field_name in FIELD_ORDER[tag]:
            child = elem.find(field_name)
            node.fields[field_name] = inner_xml(child) if child is not None else ""
        node.children = [parse_entry(child) for child in list(elem) if child.tag == "plugin"]
        return node
    if tag in ("constant", "variable"):
        node.fields["type"] = inner_xml(elem.find("type")) if elem.find("type") is not None else ""
        node.fields["numberOfElements"] = (
            inner_xml(elem.find("numberOfElements")) if elem.find("numberOfElements") is not None else ""
        )
        for child in list(elem):
            if child.tag == "value":
                node.values.append(ValueEntry(value=inner_xml(child), index=child.get("index", "")))
        node.children = [parse_entry(child) for child in list(elem) if child.tag == "plugin"]
        return node
    if tag == "plugin":
        node.children = [parse_entry(child) for child in list(elem) if child.tag == "parameter"]
        return node
    if tag == "parameter":
        node.fields["content"] = inner_xml(elem)
        return node
    raise ValueError(f"Unsupported tag '{tag}'")


def load_document(path: str | pathlib.Path) -> DocumentModel:
    """Load an xlmap file into the editor model."""
    tree = ET.parse(path)
    root = tree.getroot()
    if root.tag != "logicalNameMap":
        raise ValueError(f"Expected root tag 'logicalNameMap', got '{root.tag}'")
    document = DocumentModel(root=parse_entry(root))
    return document


def emit_entry(node: EditorNode) -> ET.Element:
    """Serialize one node recursively."""
    elem = ET.Element(node.tag, {key: value for key, value in node.attributes.items() if value.strip()})
    if node.tag in ("logicalNameMap", "module"):
        for child in node.children:
            elem.append(emit_entry(child))
        return elem
    if node.tag in ("redirectedRegister", "redirectedChannel", "redirectedBit"):
        for field_name in FIELD_ORDER[node.tag]:
            value = node.fields.get(field_name, "").strip()
            if not value:
                continue
            child = ET.SubElement(elem, field_name)
            set_inner_xml(child, value)
        for child_node in node.children:
            elem.append(emit_entry(child_node))
        return elem
    if node.tag in ("constant", "variable"):
        type_text = node.fields.get("type", "").strip()
        if type_text:
            type_elem = ET.SubElement(elem, "type")
            set_inner_xml(type_elem, type_text)
        for entry in node.values:
            if not entry.value.strip() and not entry.index.strip():
                continue
            value_elem = ET.SubElement(elem, "value")
            if entry.index.strip():
                value_elem.set("index", entry.index.strip())
            set_inner_xml(value_elem, entry.value)
        number_of_elements = node.fields.get("numberOfElements", "").strip()
        if number_of_elements:
            number_elem = ET.SubElement(elem, "numberOfElements")
            set_inner_xml(number_elem, number_of_elements)
        for child_node in node.children:
            elem.append(emit_entry(child_node))
        return elem
    if node.tag == "plugin":
        for child_node in node.children:
            elem.append(emit_entry(child_node))
        return elem
    if node.tag == "parameter":
        set_inner_xml(elem, node.fields.get("content", ""))
        return elem
    raise ValueError(f"Unsupported tag '{node.tag}'")


def indent(elem: ET.Element, level: int = 0) -> None:
    """Pretty-print an XML element tree in place."""
    indent_text = "\n" + level * "  "
    child_indent = "\n" + (level + 1) * "  "
    children = list(elem)
    if children:
        if not elem.text or not elem.text.strip():
            elem.text = child_indent
        for child in children:
            indent(child, level + 1)
        if not children[-1].tail or not children[-1].tail.strip():
            children[-1].tail = indent_text
    if level and (not elem.tail or not elem.tail.strip()):
        elem.tail = indent_text


def document_to_element(document: DocumentModel) -> ET.Element:
    """Convert a document into an XML root element."""
    root = emit_entry(document.root)
    indent(root)
    return root


def serialize_document(document: DocumentModel) -> str:
    """Serialize a document to a Unicode XML string."""
    root = document_to_element(document)
    xml_text = ET.tostring(root, encoding="unicode")
    return '<?xml version="1.0" encoding="UTF-8"?>\n' + xml_text + "\n"


def save_document(document: DocumentModel, path: str | pathlib.Path) -> None:
    """Save a document to disk."""
    pathlib.Path(path).write_text(serialize_document(document), encoding="utf-8")


def validate_document(document: DocumentModel) -> tuple[bool, str]:
    """Validate a document model against the bundled XSD."""
    schema = xmlschema.XMLSchema(str(SCHEMA_PATH))
    xml_text = serialize_document(document)
    try:
        schema.validate(xml_text)
    except xmlschema.XMLSchemaException as exc:
        return False, str(exc)
    return True, "Schema validation succeeded."
