#!/usr/bin/env python3
"""Prototype main window for mate-mpv controls layout."""

from __future__ import annotations

import tkinter as tk
from tkinter import ttk


class MateMpvWindow:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("mate-mpv")
        self.root.geometry("1100x700")
        self.root.minsize(900, 500)

        self.is_fullscreen = False
        self.is_playing = False
        self.position = tk.DoubleVar(value=0.0)
        self.volume = tk.DoubleVar(value=70.0)
        self.playback_state = tk.StringVar(value="State: Stopped")

        self._build_ui()

    def _build_ui(self) -> None:
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)

        container = ttk.Frame(self.root, padding=12)
        container.grid(sticky="nsew")
        container.columnconfigure(0, weight=1)
        container.rowconfigure(0, weight=1)

        video_surface = ttk.Frame(container, relief="sunken", borderwidth=2)
        video_surface.grid(row=0, column=0, sticky="nsew")
        video_surface.columnconfigure(0, weight=1)
        video_surface.rowconfigure(0, weight=1)

        placeholder = ttk.Label(
            video_surface,
            text="Video Area",
            anchor="center",
            font=("Sans", 24, "bold"),
            foreground="#666666",
        )
        placeholder.grid(sticky="nsew")

        controls = ttk.Frame(container, padding=(0, 10, 0, 0))
        controls.grid(row=1, column=0, sticky="ew")
        controls.columnconfigure(1, weight=1)

        left_buttons = ttk.Frame(controls)
        left_buttons.grid(row=0, column=0, sticky="w", padx=(0, 16))

        ttk.Button(left_buttons, text="⏮ Return to Start", command=self.return_to_start).grid(
            row=0, column=0, padx=(0, 6)
        )
        ttk.Button(left_buttons, text="⏪ Rewind", command=self.rewind).grid(
            row=0, column=1, padx=6
        )

        self.play_pause_btn = ttk.Button(left_buttons, text="▶ Play", command=self.toggle_play_pause)
        self.play_pause_btn.grid(row=0, column=2, padx=6)

        ttk.Button(left_buttons, text="⏹ Stop", command=self.stop).grid(row=0, column=3, padx=6)
        ttk.Button(left_buttons, text="⏩ Fast Forward", command=self.fast_forward).grid(
            row=0, column=4, padx=(6, 0)
        )

        center_panel = ttk.Frame(controls)
        center_panel.grid(row=0, column=1, sticky="ew")
        center_panel.columnconfigure(0, weight=1)

        seekbar = ttk.Scale(
            center_panel,
            orient="horizontal",
            from_=0,
            to=100,
            variable=self.position,
            command=self.on_seek,
        )
        seekbar.grid(row=0, column=0, sticky="ew", padx=8)

        state_label = ttk.Label(center_panel, textvariable=self.playback_state, anchor="center")
        state_label.grid(row=1, column=0, pady=(4, 0), sticky="ew")

        right_panel = ttk.Frame(controls)
        right_panel.grid(row=0, column=2, sticky="e", padx=(16, 0))

        ttk.Label(right_panel, text="Volume").grid(row=0, column=0, padx=(0, 8))
        ttk.Scale(
            right_panel,
            orient="horizontal",
            from_=0,
            to=100,
            variable=self.volume,
            length=160,
            command=self.on_volume_change,
        ).grid(row=0, column=1, padx=(0, 10))

        self.fullscreen_btn = ttk.Button(
            right_panel,
            text="Enter Fullscreen",
            command=self.toggle_fullscreen,
        )
        self.fullscreen_btn.grid(row=0, column=2)

    def return_to_start(self) -> None:
        self.position.set(0)
        self.playback_state.set("State: Returned to start")

    def rewind(self) -> None:
        self.position.set(max(0, self.position.get() - 5))
        self.playback_state.set(f"State: Rewinding ({self.position.get():.0f}%)")

    def toggle_play_pause(self) -> None:
        self.is_playing = not self.is_playing
        if self.is_playing:
            self.play_pause_btn.configure(text="⏸ Pause")
            self.playback_state.set("State: Playing")
        else:
            self.play_pause_btn.configure(text="▶ Play")
            self.playback_state.set("State: Paused")

    def stop(self) -> None:
        self.is_playing = False
        self.play_pause_btn.configure(text="▶ Play")
        self.position.set(0)
        self.playback_state.set("State: Stopped")

    def fast_forward(self) -> None:
        self.position.set(min(100, self.position.get() + 5))
        self.playback_state.set(f"State: Fast forwarding ({self.position.get():.0f}%)")

    def on_seek(self, _value: str) -> None:
        status = "Playing" if self.is_playing else "Paused"
        self.playback_state.set(f"State: {status} ({self.position.get():.0f}%)")

    def on_volume_change(self, _value: str) -> None:
        status = "Playing" if self.is_playing else "Paused"
        self.playback_state.set(
            f"State: {status} ({self.position.get():.0f}%) • Volume: {self.volume.get():.0f}%"
        )

    def toggle_fullscreen(self) -> None:
        self.is_fullscreen = not self.is_fullscreen
        self.root.attributes("-fullscreen", self.is_fullscreen)
        if self.is_fullscreen:
            self.fullscreen_btn.configure(text="Exit Fullscreen")
        else:
            self.fullscreen_btn.configure(text="Enter Fullscreen")


def main() -> None:
    root = tk.Tk()
    MateMpvWindow(root)
    root.mainloop()


if __name__ == "__main__":
    main()
