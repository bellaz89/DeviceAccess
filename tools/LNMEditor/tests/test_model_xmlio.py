from __future__ import annotations

from pathlib import Path

from lnm_editor.xmlio import load_document, serialize_document, validate_document


def test_load_and_validate_repo_sample() -> None:
    sample = Path(__file__).resolve().parents[3] / "tests" / "valid.xlmap"
    document = load_document(sample)
    ok, message = validate_document(document)
    assert ok, message


def test_serialized_document_contains_root_tag() -> None:
    sample = Path(__file__).resolve().parents[3] / "tests" / "valid.xlmap"
    document = load_document(sample)
    xml_text = serialize_document(document)
    assert xml_text.startswith('<?xml version="1.0" encoding="UTF-8"?>')
    assert "<logicalNameMap>" in xml_text
