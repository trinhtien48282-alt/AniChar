"""Anime Hair Color Finder - GUI. Run: python main.py"""
import logging
import multiprocessing
import queue
import threading
import time
import webbrowser
from tkinter import colorchooser, filedialog

import customtkinter as ctk
from PIL import Image

import hairfinder as hf

DEPTHS = {"Small (30 new)": 30, "Medium (80 new)": 80, "Large (200 new)": 200}
SORTS = {"Best match": lambda r: r["de"], "Highest confidence": lambda r: -r["conf"],
         "Name": lambda r: r["name"].lower(), "Newest analysis": lambda r: -r["ts"]}
PAGE_CARDS = 60   # cards drawn at once; 'Show more' adds another page (Tk gets slow past ~100)
DRAW_EVERY = 1.5  # seconds between grid refreshes while a search is running
CARD_W = 220
CARD_H = 500      # fixed height: variable-height cards glitch while scrolling in Tk
LOW_CONF = 0.5
BG, PANEL, CARD, BORDER, ACCENT, MUTED = "#0f131b", "#161b26", "#1d2433", "#2b3550", "#5b8cff", "#8b95ad"


def match_color(sim: float) -> str:
    return "#3ecf8e" if sim >= 80 else "#f2b94b" if sim >= 50 else "#d9645f"


class App(ctk.CTk):
    def __init__(self):
        super().__init__(fg_color=BG)
        self.title("Anime Hair Color Finder")
        self.geometry("1200x820")
        self.minsize(1000, 640)
        ctk.set_appearance_mode("dark")
        self.db = hf.DB()
        self.rgb = (107, 182, 217)
        self.stop = threading.Event()
        self.events: queue.Queue = queue.Queue()
        self.worker: threading.Thread | None = None
        self.rows: list[dict] = []
        self.all_rows: list[dict] = []
        self.cards: dict = {}   # id -> card widget, reused between refreshes
        self.pos: dict = {}
        self.limit = PAGE_CARDS
        self.cols = 0
        self._dirty = False
        self._last_draw = 0.0
        self._refresh_job = None
        self.last_error = ""
        self._resize_job = None
        self._build()
        self.set_color(self.rgb)
        self.bind("<Configure>", self._on_resize)
        self.bind_all("<Control-f>", lambda e: self.query.focus_set())
        self.bind_all("<Control-r>", lambda e: self.find())
        self.bind_all("<Escape>", lambda e: self.stop.set())
        self.protocol("WM_DELETE_WINDOW", self._close)
        threading.Thread(target=self._reanalyze, daemon=True).start()
        self.after(100, self._poll)

    def _reanalyze(self):
        try:
            n = hf.reanalyze_stale(self.db)
        except Exception:
            logging.exception("reanalyze failed")
            return
        if n:
            self.events.put(("status", f"Updated {n} cached analyses with the improved detector."))
            self.events.put(("redraw",))

    def _close(self):
        self.stop.set()
        hf.shutdown_pool()
        self.destroy()

    # ------------------------------------------------------------------ layout
    def _section(self, parent, text):
        ctk.CTkLabel(parent, text=text, text_color=MUTED, anchor="w",
                     font=ctk.CTkFont(size=11, weight="bold")).pack(fill="x", padx=16, pady=(16, 4))

    def _build(self):
        side = ctk.CTkScrollableFrame(self, width=290, fg_color=PANEL, corner_radius=0)
        side.pack(side="left", fill="y")
        ctk.CTkLabel(side, text="Anime Hair\nColor Finder", justify="left", anchor="w",
                     font=ctk.CTkFont(size=22, weight="bold")).pack(fill="x", padx=16, pady=(16, 0))

        self._section(side, "TARGET COLOR")
        self.preview = ctk.CTkButton(side, height=84, corner_radius=14, command=self.pick,
                                     font=ctk.CTkFont(size=20, weight="bold"))
        self.preview.pack(fill="x", padx=16)
        self.hex = ctk.CTkEntry(side, placeholder_text="#RRGGBB  or  r, g, b")
        self.hex.pack(fill="x", padx=16, pady=(8, 0))
        self.hex.bind("<Return>", self._from_entry)
        self.hex.bind("<FocusOut>", self._from_entry)
        self.info = ctk.CTkLabel(side, justify="left", anchor="w", text_color=MUTED,
                                 font=ctk.CTkFont(size=12))
        self.info.pack(fill="x", padx=16, pady=(6, 0))

        self._section(side, "SEARCH")
        self.query = ctk.CTkEntry(side, placeholder_text="Character name (optional)")
        self.query.pack(fill="x", padx=16)
        self.girls = ctk.CTkCheckBox(side, text="Girls only", command=self.schedule_refresh)
        self.girls.select()
        self.girls.pack(anchor="w", padx=16, pady=(10, 0))
        self.use_jikan = ctk.CTkCheckBox(side, text="Source: Jikan (MyAnimeList)")
        self.use_jikan.select()
        self.use_jikan.pack(anchor="w", padx=16, pady=(8, 0))
        self.use_anilist = ctk.CTkCheckBox(side, text="Source: AniList")
        self.use_anilist.select()
        self.use_anilist.pack(anchor="w", padx=16, pady=(8, 0))
        self.mode = ctk.CTkSegmentedButton(side, values=["Cache only", "Online"])
        self.mode.set("Online")
        self.mode.pack(fill="x", padx=16, pady=(12, 0))
        self.depth = ctk.CTkOptionMenu(side, values=list(DEPTHS))
        self.depth.set("Small (30 new)")
        self.depth.pack(fill="x", padx=16, pady=(8, 0))
        self.find_btn = ctk.CTkButton(side, text="Find characters", height=38, fg_color=ACCENT,
                                      font=ctk.CTkFont(weight="bold"), command=self.find)
        self.find_btn.pack(fill="x", padx=16, pady=(14, 0))
        self.stop_btn = ctk.CTkButton(side, text="Stop", fg_color="#8a3b3b", hover_color="#a34747",
                                      command=self.stop.set, state="disabled")
        self.stop_btn.pack(fill="x", padx=16, pady=(8, 0))

        self._section(side, "FILTERS  (instant, no network)")
        self.tol = self._slider(side, "Max color distance", 3, 50, 25)
        self.minconf = self._slider(side, "Min confidence %", 0, 90, 0)
        self.sort = ctk.CTkOptionMenu(side, values=list(SORTS), command=lambda _: self.schedule_refresh())
        self.sort.pack(fill="x", padx=16, pady=(10, 0))
        self.reject = ctk.CTkCheckBox(side, text="Hide listed-hair conflicts", command=self.schedule_refresh)
        self.reject.select()
        self.reject.pack(anchor="w", padx=16, pady=(10, 0))
        ctk.CTkLabel(side, text="Skips characters whose description names a different hair color "
                                "than your target (image analysis is only a heuristic).",
                     text_color=MUTED, justify="left", wraplength=250, anchor="w",
                     font=ctk.CTkFont(size=10)).pack(fill="x", padx=16, pady=(2, 0))
        ctk.CTkButton(side, text="Analyze a local image", fg_color="transparent", border_width=1,
                      border_color=BORDER, command=self.local_image).pack(fill="x", padx=16, pady=(14, 16))

        main = ctk.CTkFrame(self, fg_color="transparent")
        main.pack(side="left", fill="both", expand=True)
        self.status = ctk.CTkLabel(main, anchor="w", justify="left", wraplength=800, text=(
            "Pick a color and press Find. The local cache grows as you search."))
        self.status.pack(fill="x", padx=18, pady=(14, 2))
        self.bar = ctk.CTkProgressBar(main, progress_color=ACCENT)
        self.bar.set(0)
        self.bar.pack(fill="x", padx=18, pady=(0, 8))
        self.grid_frame = ctk.CTkScrollableFrame(main, fg_color="transparent")
        self.grid_frame.pack(fill="both", expand=True, padx=8, pady=(0, 8))
        self.empty = ctk.CTkLabel(self.grid_frame, justify="left", text_color=MUTED, text=(
            "No close color matches found.  (；一_一)\n\nTry:\n"
            "  - raising the max color distance\n  - searching online\n"
            "  - selecting a slightly different color\n  - unticking Girls only"))
        self.more = ctk.CTkButton(self.grid_frame, text="Show more", command=self._show_more)

    def _slider(self, parent, label, lo, hi, val):
        text = ctk.CTkLabel(parent, text=f"{label}: {val}", anchor="w")
        text.pack(fill="x", padx=16, pady=(8, 0))
        s = ctk.CTkSlider(parent, from_=lo, to=hi, number_of_steps=hi - lo)
        s.set(val)
        s.configure(command=lambda v: (text.configure(text=f"{label}: {int(v)}"), self.schedule_refresh()))
        s.pack(fill="x", padx=16)
        return s

    # ------------------------------------------------------------------- color
    def set_color(self, rgb):
        self.rgb = tuple(rgb)
        self._clear_cards()  # card contents depend on the target color
        hx = hf.to_hex(rgb)
        self.preview.configure(fg_color=hx, hover_color=hx, text=hx,
                               text_color="#000000" if sum(rgb) / 3 > 140 else "#ffffff")
        self.hex.delete(0, "end")
        self.hex.insert(0, hx)
        h, s, v = hf.to_hsv(rgb)
        hh, ss, ll = hf.to_hsl(rgb)
        self.info.configure(text=f"RGB  {rgb[0]}, {rgb[1]}, {rgb[2]}\nHSV  {h}°, {s}%, {v}%\n"
                                 f"HSL  {hh}°, {ss}%, {ll}%")
        self.refresh()

    def pick(self):
        rgb, _ = colorchooser.askcolor(color=hf.to_hex(self.rgb), title="Target hair color")
        if rgb:
            self.set_color([int(c) for c in rgb])

    def _from_entry(self, _=None):
        try:
            rgb = hf.parse_color(self.hex.get())
        except ValueError as e:
            self.status.configure(text=f"Invalid color: {e}")
            return False
        if rgb != self.rgb:
            self.set_color(rgb)
        return True

    # ------------------------------------------------------------------ search
    def find(self):
        if self.worker and self.worker.is_alive():
            return
        if not self._from_entry():
            return
        self.stop.clear()
        self.last_error = ""
        girls = bool(self.girls.get())
        if self.mode.get() == "Cache only":
            self.refresh()
            self.status.configure(text=f"Local cache: {len(self.rows)} matches." if self.rows else
                                       "No cached matches yet. Switch to Online to build the cache.")
            return
        apis = [a for a, on in ((hf.Jikan(), self.use_jikan.get()), (hf.AniList(), self.use_anilist.get()))
                if on]
        if not apis:
            self.status.configure(text="Tick at least one source.")
            return
        note = ""
        if girls and any(not a.has_gender for a in apis):
            note = "  (Jikan skipped: it has no gender data, untick Girls only to use it)"
        self.find_btn.configure(state="disabled")
        self.stop_btn.configure(state="normal")
        self.bar.set(0)
        self.status.configure(text="Contacting " + " + ".join(a.name for a in apis if a.has_gender or not girls)
                                   + "..." + note)
        self.worker = threading.Thread(target=self._work, daemon=True, args=(
            apis, self.query.get().strip(), DEPTHS[self.depth.get()], girls))
        self.worker.start()

    def _work(self, apis, query, target, girls):
        new = checked = failed = 0
        errors: list[str] = []
        try:
            new, checked, failed, errors = hf.run_search(
                self.db, apis, query, target, self.stop,
                progress=lambda n, c, f: self.events.put(("progress", n, target, c, f)),
                notify=lambda m: self.events.put(("status", m)), girls_only=girls)
        except ConnectionError as e:
            self.events.put(("error", f"{e}\nCached results are still available. If this is a DNS or "
                                      "SSL error, try DNS 1.1.1.1 / 8.8.8.8 or a VPN."))
        except Exception as e:
            logging.exception("worker crashed")
            self.events.put(("error", f"Unexpected error: {e}"))
        self.events.put(("done", new, checked, failed, errors))

    def _poll(self):
        try:
            latest = None  # many progress events per tick -> apply only the newest
            while True:
                try:
                    ev = self.events.get_nowait()
                except queue.Empty:
                    break
                if ev[0] == "progress":
                    latest = ev
                    continue
                if latest:
                    self._safe(latest)
                    latest = None
                self._safe(ev)
            if latest:
                self._safe(latest)
            if self._dirty and time.monotonic() - self._last_draw > DRAW_EVERY:
                self._safe(("redraw",))
        finally:
            self.after(100, self._poll)

    def _safe(self, ev):
        try:
            self._handle(ev)
        except Exception as e:  # a display bug must not kill the poll loop
            logging.exception("event %s failed", ev[0])
            self.status.configure(text=f"Display error: {e} (see app.log)")
            if ev[0] == "done":
                self._finish()

    def _handle(self, ev):
        if ev[0] == "redraw":
            self.refresh()
        elif ev[0] == "status":
            self.status.configure(text=ev[1])
        elif ev[0] == "progress":
            _, n, target, checked, failed = ev
            self.bar.set(min(1, n / target))
            self.status.configure(text=f"New: {n} / {target}    Checked: {checked}    Failed: {failed}")
            self._dirty = True  # progressive results, throttled in _poll
        elif ev[0] == "error":
            self.last_error = ev[1]
        elif ev[0] == "done":
            _, new, checked, failed, errors = ev
            self._finish()
            self.refresh()
            if self.last_error:
                msg = self.last_error
            elif self.stop.is_set():
                msg = "Search stopped. Analyzed results are saved to the cache."
            elif checked == 0:
                msg = "No characters came back for that search."
            else:
                msg = f"Complete. {new} new, {checked} checked, {failed} failed."
                if errors:
                    msg += "   Unavailable: " + "; ".join(e.split(":")[0] for e in errors)
            self.last_error = ""
            self.bar.set(1 if not self.stop.is_set() else self.bar.get())
            self.status.configure(text=msg)

    def _finish(self):
        self.find_btn.configure(state="normal")
        self.stop_btn.configure(state="disabled")

    # ----------------------------------------------------------------- results
    def schedule_refresh(self, ms: int = 150):
        """Debounce: dragging a slider fires many events, only the last one redraws."""
        if self._refresh_job:
            self.after_cancel(self._refresh_job)
        self._refresh_job = self.after(ms, self.refresh)

    def refresh(self):
        self._dirty = False
        self._last_draw = time.monotonic()
        self.all_rows = sorted(
            self.db.ranked(self.rgb, tol=self.tol.get(), min_conf=self.minconf.get() / 100,
                           query=self.query.get().strip(), girls_only=bool(self.girls.get()),
                           reject_hair_conflicts=bool(self.reject.get())),
            key=SORTS[self.sort.get()])
        self.rows = self.all_rows[:self.limit]
        self.draw()

    def _show_more(self):
        self.limit += PAGE_CARDS
        self.refresh()

    def _clear_cards(self):
        for card in self.cards.values():
            card.destroy()
        self.cards, self.pos, self.limit = {}, {}, PAGE_CARDS

    def _on_resize(self, e):
        if e.widget is self:
            if self._resize_job:
                self.after_cancel(self._resize_job)
            self._resize_job = self.after(200, lambda: self.draw() if self._cols() != self.cols else None)

    def _cols(self) -> int:
        return max(1, self.grid_frame.winfo_width() // (CARD_W + 16))

    def draw(self):
        """Incremental: reuse existing cards, create only new ones, move only those that shifted."""
        cols = self.cols = self._cols()
        keep = {r["id"] for r in self.rows}
        for k in [k for k in self.cards if k not in keep]:
            self.cards.pop(k).destroy()
            self.pos.pop(k, None)
        if not self.rows:
            self.more.grid_remove()
            self.empty.grid(row=0, column=0, pady=60, padx=20)
            return
        self.empty.grid_remove()
        for i, r in enumerate(self.rows):
            card = self.cards.get(r["id"])
            if card is None:
                card = self.cards[r["id"]] = self._card(r)
            pos = (i // cols, i % cols)
            if self.pos.get(r["id"]) != pos:
                card.grid(row=pos[0], column=pos[1], padx=8, pady=8, sticky="n")
                self.pos[r["id"]] = pos
        left = len(self.all_rows) - len(self.rows)
        if left > 0:
            self.more.configure(text=f"Show more ({left} left)")
            self.more.grid(row=(len(self.rows) - 1) // cols + 1, column=0, columnspan=cols, pady=10)
        else:
            self.more.grid_remove()

    def _thumb(self, r):
        try:
            im = Image.open(r["path"]).convert("RGB")
            im.thumbnail((CARD_W - 24, 270))
            return ctk.CTkImage(im, size=im.size)
        except Exception:
            return None  # "Image unavailable"

    def _card(self, r):
        """Fixed-size card. The button is packed first (side=bottom) so it can never be clipped."""
        info = r.get("info") or {}
        card = ctk.CTkFrame(self.grid_frame, fg_color=CARD, corner_radius=16, width=CARD_W,
                            height=CARD_H, border_width=1, border_color=BORDER)
        card.pack_propagate(False)
        ctk.CTkButton(card, text="Open source page", height=26, fg_color="transparent",
                      border_width=1, border_color=BORDER, command=lambda u=r["url"]: u and webbrowser.open(u)
                      ).pack(side="bottom", fill="x", padx=14, pady=(6, 12))
        ctk.CTkFrame(card, height=8, corner_radius=4, fg_color=hf.to_hex(r["rgb"])
                     ).pack(fill="x", padx=12, pady=(12, 0))
        img = self._thumb(r)
        pic = ctk.CTkLabel(card, image=img, text="" if img else "Image unavailable")
        pic.pack(padx=12, pady=(8, 4))
        card._img = img  # keep a reference or Tk drops the picture while scrolling
        pic.bind("<Button-1>", lambda e, u=r["url"]: u and webbrowser.open(u))
        ctk.CTkLabel(card, text=r["name"], font=ctk.CTkFont(size=15, weight="bold"),
                     wraplength=CARD_W - 24).pack(padx=12)
        ctk.CTkLabel(card, text=f"{r['sim']:.1f}% match", text_color=match_color(r["sim"]),
                     font=ctk.CTkFont(size=16, weight="bold")).pack(pady=(2, 0))

        lines, bits = [], []
        if info.get("age"):
            bits.append(f"Age {info['age']}")
        if info.get("birthday"):
            bits.append(f"Birthday {info['birthday']}")
        if bits:
            lines.append("   ".join(bits))
        bits = [b for b in (r.get("gender"), f"Blood {info['blood']}" if info.get("blood") else None) if b]
        if isinstance(info.get("favs"), int):
            bits.append(f"{info['favs']:,} favorites")
        if bits:
            lines.append("   ".join(bits))
        if info.get("nicknames"):
            lines.append("aka " + ", ".join(info["nicknames"]))
        hair = f"App sees hair as: {hf.color_name(r['rgb'])}  ({hf.to_hex(r['rgb'])})"
        if info.get("hair"):
            hair += f"\nListed hair: {info['hair']}" + ("  (!) differs" if hf.hair_conflict(r["rgb"], info["hair"]) else "")
        lines.append(hair)
        low = "  (!) low" if r["conf"] < LOW_CONF else ""
        lines.append(f"target {hf.to_hex(self.rgb)}   dE {r['de']:.1f}\nanalysis confidence {r['conf'] * 100:.0f}%{low}")
        ctk.CTkLabel(card, text="\n".join(lines), text_color=MUTED, justify="left", anchor="nw",
                     font=ctk.CTkFont(size=11), wraplength=CARD_W - 24
                     ).pack(fill="both", expand=True, padx=12, pady=(6, 0))
        return card

    # ------------------------------------------------------------- local image
    def local_image(self):
        path = filedialog.askopenfilename(filetypes=[("Images", "*.png *.jpg *.jpeg *.webp *.bmp")])
        if not path:
            return
        try:
            res = hf.detect_hair(Image.open(path))
        except Exception as e:
            self.status.configure(text=f"Could not read image: {e}")
            return
        if not res:
            self.status.configure(text="Hair not detected in that image.")
            return
        rgb, conf = res
        de = float(hf.delta_e(hf.to_lab(rgb), hf.to_lab(self.rgb)))
        self.status.configure(text=f"Local image hair {hf.to_hex(rgb)} vs target {hf.to_hex(self.rgb)}: "
                                   f"match {hf.similarity(de):.1f}%, dE {de:.1f}, "
                                   f"analysis confidence {conf * 100:.0f}%")


if __name__ == "__main__":
    multiprocessing.freeze_support()  # needed on Windows for the worker processes
    hf.DATA.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(filename=hf.DATA / "app.log", level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    App().mainloop()