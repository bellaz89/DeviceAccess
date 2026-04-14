"""Tk application for editing ChimeraTK LogicalNameMapping xlmap files."""

from __future__ import annotations

import pathlib
import tkinter as tk
from tkinter import filedialog, messagebox, simpledialog, ttk

from .constants import ADDABLE_CHILDREN, FIELD_HINTS, FIELD_LABELS, FIELD_ORDER, PLUGIN_NAMES, TYPE_NAMES
from .model import DocumentModel, EditorNode, ValueEntry, walk
from .widgets import ScrollableForm, TextPromptDialog
from .xmlio import load_document, save_document, serialize_document, validate_document


class LNMEditorApp(tk.Tk):
    """Main window with xlmap tree and node property editor."""

    def __init__(self) -> None:
        super().__init__()
        self.title("LogicalNameMapping xlmap Editor")
        self.geometry("1360x860")
        self.minsize(1080, 720)
        self.document = DocumentModel()
        self.current_file: pathlib.Path | None = None
        self.tree_to_uid: dict[str, str] = {}
        self.current_node_uid: str | None = None
        self.form_commit = None
        self.status_var = tk.StringVar(value="Ready.")
        self.validation_var = tk.StringVar(value="Validation not run yet.")

        self._build_ui()
        self.refresh_tree()

    def _build_ui(self) -> None:
        self._build_menu()

        root = ttk.Frame(self, padding=8)
        root.pack(fill="both", expand=True)
        root.columnconfigure(0, weight=1)
        root.rowconfigure(1, weight=1)

        toolbar = ttk.Frame(root)
        toolbar.grid(row=0, column=0, sticky="ew", pady=(0, 8))
        for text, command in (
            ("Open", self.open_file),
            ("Save", self.save_file),
            ("Save As", self.save_file_as),
            ("Validate", self.validate_current_document),
            ("Preview XML", self.preview_xml),
            ("Add Child", self.add_child),
            ("Delete", self.delete_selected_node),
        ):
            ttk.Button(toolbar, text=text, command=command).pack(side="left", padx=(0, 6))

        split = ttk.PanedWindow(root, orient="horizontal")
        split.grid(row=1, column=0, sticky="nsew")

        tree_frame = ttk.Frame(split, padding=(0, 0, 8, 0))
        tree_frame.columnconfigure(0, weight=1)
        tree_frame.rowconfigure(1, weight=1)
        ttk.Label(tree_frame, text="xlmap Tree").grid(row=0, column=0, sticky="w", pady=(0, 4))
        self.tree = ttk.Treeview(tree_frame, show="tree", selectmode="browse")
        self.tree.grid(row=1, column=0, sticky="nsew")
        tree_scroll = ttk.Scrollbar(tree_frame, orient="vertical", command=self.tree.yview)
        tree_scroll.grid(row=1, column=1, sticky="ns")
        self.tree.configure(yscrollcommand=tree_scroll.set)
        self.tree.bind("<<TreeviewSelect>>", self.on_tree_selection_changed)
        self.tree.bind("<Delete>", lambda _event: self.delete_selected_node())
        split.add(tree_frame, weight=2)

        details_frame = ttk.Frame(split)
        details_frame.columnconfigure(0, weight=1)
        details_frame.rowconfigure(1, weight=1)
        ttk.Label(details_frame, text="Value Editor").grid(row=0, column=0, sticky="w", pady=(0, 4))
        self.form = ScrollableForm(details_frame)
        self.form.grid(row=1, column=0, sticky="nsew")
        split.add(details_frame, weight=3)

        status = ttk.Frame(root)
        status.grid(row=2, column=0, sticky="ew", pady=(8, 0))
        status.columnconfigure(0, weight=1)
        ttk.Label(status, textvariable=self.status_var).grid(row=0, column=0, sticky="w")
        ttk.Label(status, textvariable=self.validation_var).grid(row=0, column=1, sticky="e")

    def _build_menu(self) -> None:
        menu = tk.Menu(self)
        self.configure(menu=menu)

        file_menu = tk.Menu(menu, tearoff=False)
        file_menu.add_command(label="Open...", command=self.open_file)
        file_menu.add_command(label="Save", command=self.save_file)
        file_menu.add_command(label="Save As...", command=self.save_file_as)
        file_menu.add_separator()
        file_menu.add_command(label="Validate", command=self.validate_current_document)
        file_menu.add_command(label="Preview XML", command=self.preview_xml)
        file_menu.add_separator()
        file_menu.add_command(label="Quit", command=self.destroy)
        menu.add_cascade(label="File", menu=file_menu)

        edit_menu = tk.Menu(menu, tearoff=False)
        edit_menu.add_command(label="Add Child", command=self.add_child)
        edit_menu.add_command(label="Delete Selected", command=self.delete_selected_node)
        menu.add_cascade(label="Edit", menu=edit_menu)

    def set_status(self, message: str) -> None:
        self.status_var.set(message)

    def all_nodes(self) -> dict[str, EditorNode]:
        return {node.uid: node for node in walk(self.document.root)}

    def get_node(self, uid: str | None) -> EditorNode | None:
        if uid is None:
            return None
        return self.all_nodes().get(uid)

    def refresh_tree(self) -> None:
        current_uid = self.current_node_uid
        self.tree.delete(*self.tree.get_children())
        self.tree_to_uid.clear()

        def insert_node(parent_item: str, node: EditorNode) -> None:
            item_id = self.tree.insert(parent_item, "end", text=node.display_name(), open=True)
            self.tree_to_uid[item_id] = node.uid
            for child in node.children:
                insert_node(item_id, child)

        insert_node("", self.document.root)
        if current_uid is not None:
            for item_id, uid in self.tree_to_uid.items():
                if uid == current_uid:
                    self.tree.selection_set(item_id)
                    self.tree.focus(item_id)
                    self.tree.see(item_id)
                    return
        top = self.tree.get_children("")
        if top:
            self.tree.selection_set(top[0])
            self.tree.focus(top[0])

    def on_tree_selection_changed(self, _event=None) -> None:
        if self.form_commit is not None:
            self.form_commit()
        selected = self.tree.selection()
        if not selected:
            return
        self.current_node_uid = self.tree_to_uid[selected[0]]
        node = self.get_node(self.current_node_uid)
        if node is not None:
            self.show_node_form(node)

    def clear_form(self) -> None:
        for child in self.form.inner.winfo_children():
            child.destroy()
        self.form_commit = None

    def _make_text_widget(self, parent: ttk.Frame, initial: str, height: int = 3) -> tk.Text:
        widget = tk.Text(parent, height=height, wrap="word", undo=True)
        widget.insert("1.0", initial)
        return widget

    def show_node_form(self, node: EditorNode) -> None:
        self.clear_form()
        frame = self.form.inner
        frame.columnconfigure(1, weight=1)
        row = 0

        ttk.Label(frame, text=f"Node type: {node.tag}", font=("", 11, "bold")).grid(
            row=row, column=0, columnspan=2, sticky="w", pady=(0, 10)
        )
        row += 1

        widgets: dict[str, object] = {}

        def add_entry(label: str, key: str, initial: str, choices: tuple[str, ...] | None = None) -> None:
            nonlocal row
            ttk.Label(frame, text=label).grid(row=row, column=0, sticky="nw", padx=(0, 8), pady=4)
            if choices is None:
                var = tk.StringVar(value=initial)
                entry = ttk.Entry(frame, textvariable=var)
                entry.grid(row=row, column=1, sticky="ew", pady=4)
                widgets[key] = var
            else:
                var = tk.StringVar(value=initial)
                combo = ttk.Combobox(frame, textvariable=var, values=choices)
                combo.grid(row=row, column=1, sticky="ew", pady=4)
                widgets[key] = var
            row += 1

        def add_text(label: str, key: str, initial: str, height: int = 3, hint: str | None = None) -> None:
            nonlocal row
            ttk.Label(frame, text=label).grid(row=row, column=0, sticky="nw", padx=(0, 8), pady=4)
            text = self._make_text_widget(frame, initial, height=height)
            text.grid(row=row, column=1, sticky="nsew", pady=4)
            widgets[key] = text
            row += 1
            if hint:
                ttk.Label(frame, text=hint, foreground="#666666").grid(
                    row=row, column=1, sticky="w", pady=(0, 4)
                )
                row += 1

        if node.tag in ("module", "redirectedRegister", "redirectedChannel", "redirectedBit", "constant", "variable"):
            add_entry("Name", "attr:name", node.attributes.get("name", ""))
        elif node.tag == "plugin":
            add_entry("Plugin name", "attr:name", node.attributes.get("name", ""), PLUGIN_NAMES)
        elif node.tag == "parameter":
            add_entry("Parameter name", "attr:name", node.attributes.get("name", ""))

        if node.tag in ("redirectedRegister", "redirectedChannel", "redirectedBit", "constant", "variable", "parameter"):
            for field_name in FIELD_ORDER.get(node.tag, ()):
                initial = node.fields.get(field_name, "")
                label = FIELD_LABELS.get(field_name, field_name)
                hint = FIELD_HINTS.get(field_name)
                if field_name == "type":
                    add_entry(label, f"field:{field_name}", initial, TYPE_NAMES)
                else:
                    add_text(label, f"field:{field_name}", initial, height=3 if field_name != "content" else 6, hint=hint)

        if node.tag in ("constant", "variable"):
            value_lines = []
            for entry in node.values:
                if entry.index.strip():
                    value_lines.append(f"index={entry.index.strip()} :: {entry.value}")
                else:
                    value_lines.append(entry.value)
            add_text(
                "Values",
                "values",
                "\n".join(value_lines),
                height=max(4, min(10, len(value_lines) + 1)),
                hint="One <value> element per line. Prefix with 'index=N :: ' for indexed values.",
            )

        actions = ttk.Frame(frame)
        actions.grid(row=row, column=0, columnspan=2, sticky="ew", pady=(12, 0))
        ttk.Button(actions, text="Apply", command=lambda: self.apply_form(node, widgets)).pack(side="left")
        ttk.Button(actions, text="Add Child", command=self.add_child).pack(side="left", padx=(6, 0))
        if node.tag != "logicalNameMap":
            ttk.Button(actions, text="Delete", command=self.delete_selected_node).pack(side="left", padx=(6, 0))

        self.form_commit = lambda: self.apply_form(node, widgets, quiet=True)

    def apply_form(self, node: EditorNode, widgets: dict[str, object], quiet: bool = False) -> None:
        try:
            for key, widget in widgets.items():
                if isinstance(widget, tk.StringVar):
                    value = widget.get().strip()
                else:
                    assert isinstance(widget, tk.Text)
                    value = widget.get("1.0", "end").strip()
                if key.startswith("attr:"):
                    node.attributes[key.split(":", 1)[1]] = value
                elif key.startswith("field:"):
                    node.fields[key.split(":", 1)[1]] = value
                elif key == "values":
                    node.values = self.parse_values_text(value)
            self.refresh_tree()
            if not quiet:
                self.set_status(f"Updated {node.display_name()}")
        except ValueError as exc:
            if not quiet:
                messagebox.showerror("Invalid value", str(exc), parent=self)
            raise

    def parse_values_text(self, text: str) -> list[ValueEntry]:
        """Parse the values text area into ``ValueEntry`` records."""
        entries: list[ValueEntry] = []
        for raw_line in text.splitlines():
            line = raw_line.strip()
            if not line:
                continue
            if line.startswith("index=") and "::" in line:
                left, value = line.split("::", 1)
                index = left.split("=", 1)[1].strip()
                if not index:
                    raise ValueError(f"Missing index in line '{line}'.")
                entries.append(ValueEntry(value=value.strip(), index=index))
            else:
                entries.append(ValueEntry(value=line))
        return entries

    def find_parent(self, child_uid: str) -> tuple[EditorNode | None, int | None]:
        """Return the parent node and index of a child UID."""
        for parent in walk(self.document.root):
            for index, child in enumerate(parent.children):
                if child.uid == child_uid:
                    return parent, index
        return None, None

    def add_child(self) -> None:
        current = self.get_node(self.current_node_uid)
        if current is None:
            return
        allowed = ADDABLE_CHILDREN.get(current.tag, ())
        if not allowed:
            messagebox.showinfo("No children allowed", f"Nodes of type '{current.tag}' cannot have children.", parent=self)
            return
        child_tag = self.choose_child_tag(allowed)
        if child_tag is None:
            return
        new_node = self.make_default_node(child_tag)
        current.children.append(new_node)
        self.current_node_uid = new_node.uid
        self.refresh_tree()
        self.set_status(f"Added {new_node.display_name()}")

    def choose_child_tag(self, allowed: tuple[str, ...]) -> str | None:
        if len(allowed) == 1:
            return allowed[0]
        dialog = simpledialog.askstring(
            "Add child",
            "Enter child type:\n" + "\n".join(f"- {item}" for item in allowed),
            parent=self,
        )
        if dialog is None:
            return None
        dialog = dialog.strip()
        if dialog not in allowed:
            messagebox.showerror("Invalid child type", f"Allowed child types: {', '.join(allowed)}", parent=self)
            return None
        return dialog

    def make_default_node(self, tag: str) -> EditorNode:
        """Create a new node with sensible defaults for the selected type."""
        node = EditorNode(tag=tag)
        if tag == "module":
            node.attributes["name"] = "newModule"
        elif tag in ("redirectedRegister", "redirectedChannel", "redirectedBit", "constant", "variable"):
            node.attributes["name"] = f"new{tag[0].upper()}{tag[1:]}"
        elif tag == "plugin":
            node.attributes["name"] = "multiply"
        elif tag == "parameter":
            node.attributes["name"] = "name"
            node.fields["content"] = ""

        if tag in ("redirectedRegister", "redirectedChannel", "redirectedBit"):
            for field_name in FIELD_ORDER[tag]:
                node.fields[field_name] = ""
        if tag in ("constant", "variable"):
            node.fields["type"] = "int32"
            node.fields["numberOfElements"] = ""
            node.values = [ValueEntry(value="0")]
        return node

    def delete_selected_node(self) -> None:
        current_uid = self.current_node_uid
        current = self.get_node(current_uid)
        if current is None or current.tag == "logicalNameMap":
            return
        parent, index = self.find_parent(current_uid)
        if parent is None or index is None:
            return
        if not messagebox.askyesno("Delete node", f"Delete '{current.display_name()}'?", parent=self):
            return
        del parent.children[index]
        self.current_node_uid = parent.uid
        self.refresh_tree()
        self.set_status(f"Deleted {current.display_name()}")

    def open_file(self) -> None:
        path = filedialog.askopenfilename(
            title="Open xlmap file",
            filetypes=[("LogicalNameMapping files", "*.xlmap *.xml"), ("All files", "*.*")],
            parent=self,
        )
        if not path:
            return
        try:
            self.document = load_document(path)
            self.current_file = pathlib.Path(path)
            self.current_node_uid = self.document.root.uid
            self.refresh_tree()
            self.set_status(f"Loaded {path}")
            self.validation_var.set("Validation not run yet.")
        except Exception as exc:  # pragma: no cover - GUI error handling
            messagebox.showerror("Open failed", str(exc), parent=self)

    def save_file(self) -> None:
        if self.current_file is None:
            self.save_file_as()
            return
        if self.form_commit is not None:
            self.form_commit()
        try:
            save_document(self.document, self.current_file)
            self.set_status(f"Saved {self.current_file}")
        except Exception as exc:  # pragma: no cover - GUI error handling
            messagebox.showerror("Save failed", str(exc), parent=self)

    def save_file_as(self) -> None:
        path = filedialog.asksaveasfilename(
            title="Save xlmap file",
            defaultextension=".xlmap",
            filetypes=[("LogicalNameMapping files", "*.xlmap"), ("XML files", "*.xml"), ("All files", "*.*")],
            parent=self,
        )
        if not path:
            return
        self.current_file = pathlib.Path(path)
        self.save_file()

    def validate_current_document(self) -> None:
        if self.form_commit is not None:
            self.form_commit()
        ok, message = validate_document(self.document)
        if ok:
            self.validation_var.set("Validation: OK")
            self.set_status(message)
            messagebox.showinfo("Validation", message, parent=self)
        else:
            self.validation_var.set("Validation: FAILED")
            self.set_status("Schema validation failed.")
            messagebox.showerror("Validation failed", message, parent=self)

    def preview_xml(self) -> None:
        if self.form_commit is not None:
            self.form_commit()
        window = tk.Toplevel(self)
        window.title("XML Preview")
        window.geometry("900x700")
        text = tk.Text(window, wrap="none", undo=False)
        text.pack(fill="both", expand=True)
        text.insert("1.0", serialize_document(self.document))
        text.configure(state="disabled")


def main() -> int:
    """Run the Tk editor application."""
    app = LNMEditorApp()
    app.mainloop()
    return 0
