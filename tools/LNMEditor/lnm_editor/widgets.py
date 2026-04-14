"""Reusable Tk widgets used by the LNM editor."""

from __future__ import annotations

import tkinter as tk
from tkinter import messagebox, simpledialog, ttk


class ScrollableForm(ttk.Frame):
    """Scrollable container for the node property editor."""

    def __init__(self, master: tk.Misc) -> None:
        super().__init__(master)
        self.canvas = tk.Canvas(self, borderwidth=0, highlightthickness=0)
        self.scrollbar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.inner = ttk.Frame(self.canvas)
        self.window = self.canvas.create_window((0, 0), window=self.inner, anchor="nw")
        self.canvas.configure(yscrollcommand=self.scrollbar.set)
        self.canvas.grid(row=0, column=0, sticky="nsew")
        self.scrollbar.grid(row=0, column=1, sticky="ns")
        self.columnconfigure(0, weight=1)
        self.rowconfigure(0, weight=1)
        self.inner.bind("<Configure>", self._on_inner_configure)
        self.canvas.bind("<Configure>", self._on_canvas_configure)

    def _on_inner_configure(self, _event: tk.Event) -> None:
        self.canvas.configure(scrollregion=self.canvas.bbox("all"))

    def _on_canvas_configure(self, event: tk.Event) -> None:
        self.canvas.itemconfigure(self.window, width=event.width)


class TextPromptDialog(simpledialog.Dialog):
    """Prompt for a free-text value such as a new node name."""

    def __init__(self, parent: tk.Misc, title: str, prompt: str, initial: str = "") -> None:
        self.prompt = prompt
        self.initial = initial
        self.value = initial
        super().__init__(parent, title)

    def body(self, master: ttk.Frame):
        ttk.Label(master, text=self.prompt).grid(row=0, column=0, sticky="w", pady=(0, 6))
        self.var = tk.StringVar(value=self.initial)
        entry = ttk.Entry(master, textvariable=self.var, width=48)
        entry.grid(row=1, column=0, sticky="ew")
        master.columnconfigure(0, weight=1)
        return entry

    def validate(self) -> bool:
        if not self.var.get().strip():
            messagebox.showerror("Missing value", "A value is required.", parent=self)
            return False
        return True

    def apply(self) -> None:
        self.value = self.var.get().strip()
