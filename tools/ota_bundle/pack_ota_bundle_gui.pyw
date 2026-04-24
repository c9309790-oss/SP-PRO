#!/usr/bin/env python3
from __future__ import annotations

import json
import pathlib
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import pack_ota_bundle as bundle_tool


def sanitize_name_part(value: str, fallback: str) -> str:
    text = (value or "").strip()
    if not text:
        return fallback
    safe = "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in text).strip("_")
    return safe or fallback


def default_bundle_path(ctr_version: str, esp32_version: str) -> str:
    script_dir = pathlib.Path(__file__).resolve().parent
    tools_dir = script_dir.parent
    out_dir = tools_dir / "after_package"
    out_dir.mkdir(parents=True, exist_ok=True)
    name_parts = ["ota"]
    if ctr_version.strip():
        name_parts.append("ctr")
        name_parts.append(sanitize_name_part(ctr_version, "version"))
    if esp32_version.strip():
        name_parts.append("hmi")
        name_parts.append(sanitize_name_part(esp32_version, "version"))
    if len(name_parts) == 1:
        name_parts.append("bundle")
    stamp = bundle_tool.datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    return str(out_dir / f"{'_'.join(name_parts)}_{stamp}.bin")


class LabeledEntry(ttk.Frame):
    def __init__(self, master, label: str, width: int = 60, browse: bool = False, save: bool = False, directory: bool = False):
        super().__init__(master)
        self.var = tk.StringVar()
        self.directory = directory
        self.save = save
        ttk.Label(self, text=label, width=18).pack(side=tk.LEFT, padx=(0, 6))
        ttk.Entry(self, textvariable=self.var, width=width).pack(side=tk.LEFT, fill=tk.X, expand=True)
        if browse:
            ttk.Button(self, text="Browse", command=self._browse).pack(side=tk.LEFT, padx=(6, 0))

    def _browse(self) -> None:
        initial = self.var.get() or str(pathlib.Path.cwd())
        if self.directory:
            selected = filedialog.askdirectory(initialdir=initial or None)
        elif self.save:
            selected = filedialog.asksaveasfilename(initialdir=str(pathlib.Path(initial).parent) if initial else None)
        else:
            selected = filedialog.askopenfilename(initialdir=str(pathlib.Path(initial).parent) if initial else None)
        if selected:
            self.var.set(selected)

    def get(self) -> str:
        return self.var.get().strip()

    def set(self, value: str) -> None:
        self.var.set(value)


class BundleGui(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("OTA Bundle Tool")
        self.geometry("980x760")
        self.minsize(860, 660)

        notebook = ttk.Notebook(self)
        notebook.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)

        self.pack_tab = ttk.Frame(notebook)
        self.inspect_tab = ttk.Frame(notebook)
        self.extract_tab = ttk.Frame(notebook)
        notebook.add(self.pack_tab, text="Pack")
        notebook.add(self.inspect_tab, text="Inspect")
        notebook.add(self.extract_tab, text="Extract")

        self._build_pack_tab()
        self._build_inspect_tab()
        self._build_extract_tab()

    def _build_pack_tab(self) -> None:
        frame = self.pack_tab
        self.ctr_entry = LabeledEntry(frame, "CTR bin", browse=True)
        self.ctr_entry.pack(fill=tk.X, padx=10, pady=(10, 4))
        self.ctr_version_entry = LabeledEntry(frame, "CTR version", width=24)
        self.ctr_version_entry.pack(fill=tk.X, padx=10, pady=4)
        self.ctr_name_entry = LabeledEntry(frame, "CTR name", width=24)
        self.ctr_name_entry.pack(fill=tk.X, padx=10, pady=4)

        self.esp_entry = LabeledEntry(frame, "ESP32 bin", browse=True)
        self.esp_entry.pack(fill=tk.X, padx=10, pady=(10, 4))
        self.esp_version_entry = LabeledEntry(frame, "ESP32 version", width=24)
        self.esp_version_entry.pack(fill=tk.X, padx=10, pady=4)
        self.esp_name_entry = LabeledEntry(frame, "ESP32 name", width=24)
        self.esp_name_entry.pack(fill=tk.X, padx=10, pady=4)

        self.pkg_version_entry = LabeledEntry(frame, "Package version", width=24)
        self.pkg_version_entry.pack(fill=tk.X, padx=10, pady=(10, 4))
        self.output_entry = LabeledEntry(frame, "Output bundle", browse=True, save=True)
        self.output_entry.pack(fill=tk.X, padx=10, pady=4)
        self.info_json_entry = LabeledEntry(frame, "Info json", browse=True, save=True)
        self.info_json_entry.pack(fill=tk.X, padx=10, pady=4)
        self._output_user_edited = False
        self.output_entry.var.trace_add("write", self._on_output_changed)
        self.ctr_entry.var.trace_add("write", self._update_default_output_path)
        self.ctr_version_entry.var.trace_add("write", self._update_default_output_path)
        self.esp_entry.var.trace_add("write", self._update_default_output_path)
        self.esp_version_entry.var.trace_add("write", self._update_default_output_path)

        align_row = ttk.Frame(frame)
        align_row.pack(fill=tk.X, padx=10, pady=(10, 4))
        ttk.Label(align_row, text="Alignment", width=18).pack(side=tk.LEFT, padx=(0, 6))
        self.align_var = tk.StringVar(value=str(bundle_tool.ALIGNMENT))
        ttk.Entry(align_row, textvariable=self.align_var, width=12).pack(side=tk.LEFT)
        ttk.Button(align_row, text="Pack Bundle", command=self._run_pack).pack(side=tk.RIGHT)

        self.pack_output = tk.Text(frame, wrap=tk.NONE, height=20)
        self.pack_output.pack(fill=tk.BOTH, expand=True, padx=10, pady=(10, 10))
        self._update_default_output_path()

    def _on_output_changed(self, *_args) -> None:
        if getattr(self, "_updating_output_path", False):
            return
        self._output_user_edited = True

    def _update_default_output_path(self, *_args) -> None:
        if getattr(self, "_output_user_edited", False):
            return
        self._updating_output_path = True
        try:
            self.output_entry.set(
                default_bundle_path(
                    self.ctr_version_entry.get() if self.ctr_entry.get() else "",
                    self.esp_version_entry.get() if self.esp_entry.get() else "",
                )
            )
        finally:
            self._updating_output_path = False

    def _build_inspect_tab(self) -> None:
        frame = self.inspect_tab
        self.inspect_entry = LabeledEntry(frame, "Bundle file", browse=True)
        self.inspect_entry.pack(fill=tk.X, padx=10, pady=(10, 4))
        ttk.Button(frame, text="Inspect Bundle", command=self._run_inspect).pack(anchor=tk.E, padx=10, pady=(4, 4))
        self.inspect_output = tk.Text(frame, wrap=tk.NONE)
        self.inspect_output.pack(fill=tk.BOTH, expand=True, padx=10, pady=(6, 10))

    def _build_extract_tab(self) -> None:
        frame = self.extract_tab
        self.extract_bundle_entry = LabeledEntry(frame, "Bundle file", browse=True)
        self.extract_bundle_entry.pack(fill=tk.X, padx=10, pady=(10, 4))
        self.extract_dir_entry = LabeledEntry(frame, "Output dir", browse=True, directory=True)
        self.extract_dir_entry.pack(fill=tk.X, padx=10, pady=4)
        ttk.Button(frame, text="Extract Payloads", command=self._run_extract).pack(anchor=tk.E, padx=10, pady=(4, 4))
        self.extract_output = tk.Text(frame, wrap=tk.NONE)
        self.extract_output.pack(fill=tk.BOTH, expand=True, padx=10, pady=(6, 10))

    def _run_worker(self, text_widget: tk.Text, action, success_title: str) -> None:
        def worker() -> None:
            try:
                result = action()
                self.after(0, lambda: self._show_result(text_widget, result, success_title))
            except Exception as exc:  # noqa: BLE001
                self.after(0, lambda: self._show_error(text_widget, exc))

        threading.Thread(target=worker, daemon=True).start()

    def _show_result(self, text_widget: tk.Text, result, success_title: str) -> None:
        if isinstance(result, (dict, list)):
            text = json.dumps(result, indent=2, ensure_ascii=False)
        else:
            text = str(result)
        text_widget.delete("1.0", tk.END)
        text_widget.insert(tk.END, text)
        messagebox.showinfo("OTA Bundle Tool", success_title)

    def _show_error(self, text_widget: tk.Text, exc: Exception) -> None:
        text_widget.delete("1.0", tk.END)
        text_widget.insert(tk.END, str(exc))
        messagebox.showerror("OTA Bundle Tool", str(exc))

    def _run_pack(self) -> None:
        ctr = self.ctr_entry.get() or None
        esp32 = self.esp_entry.get() or None
        out = self.output_entry.get()
        if not out:
            messagebox.showerror("OTA Bundle Tool", "Output bundle is required.")
            return

        def action():
            align = int(self.align_var.get().strip())
            return bundle_tool.pack_bundle_to_manifest(
                out=out,
                ctr=ctr,
                esp32=esp32,
                ctr_version=self.ctr_version_entry.get(),
                esp32_version=self.esp_version_entry.get(),
                ctr_name=self.ctr_name_entry.get(),
                esp32_name=self.esp_name_entry.get(),
                package_version=self.pkg_version_entry.get(),
                align=align,
                info_json=self.info_json_entry.get() or None,
            )

        self._run_worker(self.pack_output, action, "Bundle packed.")

    def _run_inspect(self) -> None:
        bundle = self.inspect_entry.get()
        if not bundle:
            messagebox.showerror("OTA Bundle Tool", "Bundle file is required.")
            return
        self._run_worker(
            self.inspect_output,
            lambda: bundle_tool.inspect_bundle_to_info(bundle),
            "Bundle inspected.",
        )

    def _run_extract(self) -> None:
        bundle = self.extract_bundle_entry.get()
        out_dir = self.extract_dir_entry.get()
        if not bundle or not out_dir:
            messagebox.showerror("OTA Bundle Tool", "Bundle file and output dir are required.")
            return
        self._run_worker(
            self.extract_output,
            lambda: {"extracted": bundle_tool.extract_bundle_entries(bundle, out_dir)},
            "Payloads extracted.",
        )


def main() -> None:
    app = BundleGui()
    app.mainloop()


if __name__ == "__main__":
    main()
