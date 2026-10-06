"""Anime Hair Color Finder core: color math, hair detection, Jikan client, SQLite cache."""
import colorsys
import hashlib
import io
import json
import logging
import os
import queue
import re
import sqlite3
import threading
import time
from concurrent.futures import ProcessPoolExecutor
from concurrent.futures.process import BrokenProcessPool
from pathlib import Path

import numpy as np
import requests
from PIL import Image
from skimage.color import deltaE_ciede2000, rgb2lab

log = logging.getLogger("ahf")

DATA = Path.home() / ".anime_hair_finder"  # user-local, never the project dir
VERSION = 2            # bump when detect_hair changes -> cached analyses are redone (from local thumbnails)
SIM_SCALE = 50.0       # similarity = 100 * (1 - dE / SIM_SCALE), clipped at 0
MAX_BYTES = 5_000_000  # reject bigger downloads
THUMB = (200, 280)     # cards only ever need this; saves disk and decode time
PAGE_SIZE = 25         # list page size for both APIs
HEADERS = {"User-Agent": "AnimeHairColorFinder/1.0 (personal desktop app)"}


# ---------------------------------------------------------------- color math
def parse_color(text: str) -> tuple[int, int, int]:
    """'#RRGGBB', 'RRGGBB', '#RGB', RGB or '107, 182, 217' -> (r, g, b). Raises ValueError."""
    t = text.strip().lower().removeprefix("rgb").strip("() ")
    if "," in t:
        parts = [p.strip() for p in t.split(",")]
        if len(parts) == 3 and all(re.fullmatch(r"-?\d+", p) for p in parts):
            v = tuple(int(p) for p in parts)
            if all(0 <= c <= 255 for c in v):
                return v  # type: ignore[return-value]
        raise ValueError("RGB needs three whole numbers from 0 to 255")
    t = t.lstrip("#")
    if re.fullmatch(r"[0-9a-f]{3}", t):
        t = "".join(c * 2 for c in t)
    if not re.fullmatch(r"[0-9a-f]{6}", t):
        raise ValueError("Use #RRGGBB, #RGB or 'r, g, b'")
    return tuple(int(t[i:i + 2], 16) for i in (0, 2, 4))  # type: ignore[return-value]


def to_hex(rgb) -> str:
    return "#%02X%02X%02X" % tuple(int(c) for c in rgb)


def to_hsv(rgb) -> tuple[int, int, int]:
    h, s, v = colorsys.rgb_to_hsv(*(c / 255 for c in rgb))
    return round(h * 360), round(s * 100), round(v * 100)


def to_hsl(rgb) -> tuple[int, int, int]:
    h, l, s = colorsys.rgb_to_hls(*(c / 255 for c in rgb))
    return round(h * 360), round(s * 100), round(l * 100)


def to_lab(rgb) -> np.ndarray:
    return rgb2lab(np.asarray(rgb, float).reshape(1, 1, 3) / 255.0)[0, 0]


def delta_e(lab1, lab2, method: str = "CIEDE2000"):
    """Perceptual distance between Lab colors (arrays broadcast). method: CIEDE2000 | CIE76."""
    a, b = np.broadcast_arrays(np.asarray(lab1, float), np.asarray(lab2, float))
    if method == "CIE76":
        return np.linalg.norm(a - b, axis=-1)
    return deltaE_ciede2000(a, b)


def similarity(de: float, scale: float = SIM_SCALE) -> float:
    """Friendly 0-100 score: linear in dE, 0 once dE >= scale. NOT a pixel-match percentage."""
    return 100.0 * max(0.0, 1.0 - float(de) / scale)


# ------------------------------------------------------- color names, text facts
def base_color(rgb) -> str:
    """Coarse human color family of an RGB value (black, white, gray, red, orange, ...)."""
    h, s, v = colorsys.rgb_to_hsv(*(c / 255 for c in rgb))
    h *= 360
    if v < 0.22:
        return "black"
    if s < 0.14:
        return "white" if v > 0.82 else "gray"
    if h < 15 or h >= 345:
        return "pink" if s < 0.55 and v > 0.7 else "red"
    if h < 45:
        return "brown" if v < 0.6 and s > 0.3 else "orange"
    if h < 68:
        return "yellow"
    if h < 165:
        return "green"
    if h < 190:
        return "cyan"
    if h < 255:
        return "blue"
    if h < 295:
        return "purple"
    return "pink"


def color_name(rgb) -> str:
    base = base_color(rgb)
    _, s, v = colorsys.rgb_to_hsv(*(c / 255 for c in rgb))
    if base not in ("black", "white", "gray"):
        if v > 0.85 and s < 0.5:
            return "light " + base
        if v < 0.4:
            return "dark " + base
    return base


_WORDS = {"black": "black", "white": "white", "gray": "gray", "grey": "gray", "silver": "gray",
          "red": "red", "crimson": "red", "scarlet": "red", "orange": "orange", "yellow": "yellow",
          "blonde": "yellow", "blond": "yellow", "golden": "yellow", "brown": "brown",
          "chestnut": "brown", "auburn": "brown", "green": "green", "teal": "cyan", "aqua": "cyan",
          "turquoise": "cyan", "cyan": "cyan", "blue": "blue", "navy": "blue", "purple": "purple",
          "violet": "purple", "lavender": "purple", "pink": "pink", "magenta": "pink"}
_NEAR = {frozenset(p) for p in [("white", "gray"), ("black", "gray"), ("black", "brown"),
                                ("brown", "orange"), ("orange", "yellow"), ("orange", "red"),
                                ("red", "pink"), ("pink", "purple"), ("purple", "blue"),
                                ("blue", "cyan"), ("cyan", "green"), ("brown", "yellow"),
                                ("brown", "red"), ("white", "yellow"), ("white", "pink")]}


def hair_families(text: str) -> set[str]:
    return {_WORDS[w] for w in re.findall(r"[a-z]+", (text or "").lower()) if w in _WORDS}


def hair_conflict(rgb, listed: str) -> bool:
    """True if the hair color named in the character's text clearly differs from what we detected."""
    fams = hair_families(listed)
    seen = base_color(rgb)
    return bool(fams) and not any(f == seen or frozenset((f, seen)) in _NEAR for f in fams)


MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]


def clean_text(s, limit: int | None = None) -> str:
    s = re.sub(r"~!.*?!~", "", s or "", flags=re.S)       # AniList spoiler blocks
    s = re.sub(r"<br\s*/?>", "\n", s, flags=re.I)
    s = re.sub(r"<[^>]+>", " ", s)
    s = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", s)       # markdown links
    s = re.sub(r"[_*]{1,3}", "", s)
    s = re.sub(r"[ \t]+", " ", s).strip()
    return s[:limit].rstrip() + "..." if limit and len(s) > limit else s


def extract_facts(text: str) -> dict:
    """Pull 'Age:', 'Birthday:' and a hair-color phrase out of free-text descriptions."""
    out = {}
    for key, pat in (("age", r"age"), ("birthday", r"(?:birthday|birthdate|birth date|date of birth)")):
        m = re.search(rf"(?im)^\s*{pat}\s*:\s*([^\n]{{1,30}})", text)
        if m:
            out[key] = m.group(1).strip()
    m = re.search(r"(?im)^\s*hair(?:\s*colou?r)?\s*:\s*([^\n]{1,40})", text)
    if m and hair_families(m.group(1)):
        out["hair"] = m.group(1).strip().lower()
    else:
        for m in re.finditer(r"\b([a-z]+)[- ]haired\b|\b([a-z]+) hair\b", text.lower()):
            w = m.group(1) or m.group(2)
            if w in _WORDS:
                out["hair"] = w
                break
    return out


def fmt_date(d: dict | None) -> str | None:
    d = d or {}
    if d.get("month") and d.get("day"):
        return f"{MONTHS[d['month'] - 1]} {d['day']}" + (f", {d['year']}" if d.get("year") else "")
    return MONTHS[d["month"] - 1] if d.get("month") else None


# ------------------------------------------------------------ hair detection
def _kmeans(x: np.ndarray, k: int = 5, iters: int = 12) -> np.ndarray:
    c = x[np.random.default_rng(0).choice(len(x), k, replace=False)].copy()
    for _ in range(iters):
        labels = np.argmin(((x[:, None] - c[None]) ** 2).sum(2), 1)
        for i in range(k):
            if (labels == i).any():
                c[i] = x[labels == i].mean(0)
    return labels


def _skinlike(rgb) -> bool:
    h, s, v = colorsys.rgb_to_hsv(*(c / 255 for c in rgb))
    return h < 0.12 and 0.15 < s < 0.6 and v > 0.55


def detect_hair(img: Image.Image):
    """Estimate hair color -> ((r, g, b), confidence 0-1) or None.

    Heuristic, no face detector: drop border-colored background, ignore the central
    lower area (face/neck), k-means the rest in Lab, then group clusters by color family
    so a blue head of hair (highlight + mid + shadow) counts as ONE blue group. Dark
    shadow/outline clusters read as 'black', so a black/gray winner is replaced by a
    clearly present chromatic, non-skin group.
    """
    img = img.convert("RGB")
    img.thumbnail((128, 128))
    a = np.asarray(img)
    h, w, _ = a.shape
    if h < 20 or w < 20:
        return None
    lab = rgb2lab(a / 255.0)
    border = np.concatenate([lab[:3].reshape(-1, 3), lab[-3:].reshape(-1, 3),
                             lab[:, :3].reshape(-1, 3), lab[:, -3:].reshape(-1, 3)])
    bg_mask = np.zeros((h, w), bool)
    if border.std(axis=0).max() < 12:  # plain background -> mask it
        bg_mask = np.linalg.norm(lab - np.median(border, axis=0), axis=-1) < 10
    yy, xx = np.indices((h, w))
    rh = int(h * 0.55)
    region = (yy < rh) & (xx >= int(w * 0.07)) & (xx < int(w * 0.93))
    region &= ~((yy / h > 0.42) & (np.abs(xx / w - 0.5) < 0.2))  # face / neck
    keep = region & ~bg_mask
    if keep.sum() < 200:
        return None
    px, rgbpx = lab[keep], a[keep]
    ypos = np.nonzero(keep)[0] / rh
    labels = _kmeans(px)
    groups: dict[str, list] = {}  # family -> [score, pixel mask]
    for i in range(5):
        m = labels == i
        share = m.mean()
        if share < 0.03:
            continue
        rgb = np.median(rgbpx[m], axis=0)
        score = share * (1 - 0.5 * ypos[m].mean()) * (0.4 if _skinlike(rgb) else 1.0)
        g = groups.setdefault(base_color(rgb), [0.0, np.zeros(len(px), bool)])
        g[0] += score
        g[1] |= m
    if not groups:
        return None
    ranked = sorted(groups.items(), key=lambda kv: -kv[1][0])
    fam, (score, mask) = ranked[0]
    demoted = False
    if fam in ("black", "gray"):  # shadows of coloured hair look black: look for a real colour
        for f2, (s2, m2) in ranked[1:]:
            if (f2 not in ("black", "gray", "white") and s2 >= 0.5 * score
                    and not _skinlike(np.median(rgbpx[m2], axis=0))):
                fam, score, mask, demoted = f2, s2, m2, True
                break
    rgb = np.median(rgbpx[mask], axis=0)
    second = max([g[0] for f, g in ranked if f != fam] or [0.0])
    dominance = score / (score + second)
    tightness = max(0.0, 1 - px[mask].std(axis=0).mean() / 35)
    coverage = min(1.0, keep.sum() / (region.sum() * 0.3))
    conf = (0.5 * dominance + 0.3 * tightness + 0.2 * coverage) * (0.85 if demoted else 1.0)
    return tuple(int(c) for c in rgb), float(min(1.0, conf))


def detect_bytes(data: bytes):
    """Picklable entry point so detection can run in another process."""
    return detect_hair(Image.open(io.BytesIO(data)))


_pool: ProcessPoolExecutor | None = None


def get_pool() -> ProcessPoolExecutor:
    """Shared worker processes: numpy work runs on other cores and never starves the GUI (GIL)."""
    global _pool
    if _pool is None:
        _pool = ProcessPoolExecutor(max_workers=max(1, min(3, (os.cpu_count() or 2) - 1)))
    return _pool


def shutdown_pool() -> None:
    global _pool
    if _pool is not None:
        _pool.shutdown(wait=False, cancel_futures=True)
        _pool = None


# ------------------------------------------------------------- data sources
class Api:
    """Throttled HTTP with bounded retries. Subclasses set name/delay and implement characters()."""
    name, delay, timeout, has_gender = "api", 1.1, 8, False

    def __init__(self):
        self._last = 0.0
        self.notify = None  # optional callback(str) so the GUI can show retry status
        self.session = requests.Session()
        self.session.headers.update(HEADERS)

    def _say(self, msg: str) -> None:
        if self.notify:
            self.notify(msg)

    def request(self, method: str, url: str, **kw) -> dict:
        last = ""
        for attempt in range(5):  # bounded retries, exponential backoff
            time.sleep(max(0.0, self.delay - (time.monotonic() - self._last)))
            self._last = time.monotonic()
            try:
                r = self.session.request(method, url, timeout=self.timeout, **kw)
            except requests.RequestException as e:
                last = f"{type(e).__name__}: {e}"[:220]
                log.warning("%s connection error (%s): %s", self.name, attempt, last)
                self._say(f"{self.name}: connection problem, retrying ({attempt + 1}/3): {last}")
                if attempt >= 2:  # unreachable: give up fast so the fallback can take over
                    break
                time.sleep(2 ** attempt)
                continue
            if r.status_code == 429 or r.status_code >= 500:
                try:
                    wait = float(r.headers.get("Retry-After", ""))
                except ValueError:
                    wait = 2 ** attempt
                wait = min(wait, 30.0)
                log.warning("%s HTTP %s, waiting %.1fs", self.name, r.status_code, wait)
                self._say(f"{self.name} busy (HTTP {r.status_code}), retrying in {wait:.0f}s...")
                time.sleep(wait)
                continue
            r.raise_for_status()
            try:
                return r.json()
            except ValueError as e:
                raise RuntimeError(f"{self.name} returned malformed JSON") from e
        raise ConnectionError(f"Unable to connect to {self.name}. {last}")

    def characters(self, page: int = 1, query: str = "") -> list[dict]:
        raise NotImplementedError


def _favs(v):
    return int(v) if isinstance(v, (int, float)) else None


class Jikan(Api):
    """MyAnimeList via Jikan. Limits: 3 req/s, 60 req/min. Character key = MAL id (positive)."""
    name, delay, timeout = "Jikan", 1.1, 5  # 5s: fail fast, the other source keeps going
    BASE = "https://api.jikan.moe/v4"

    def get(self, path: str, **params) -> dict:
        return self.request("GET", self.BASE + path, params=params)

    def characters(self, page: int = 1, query: str = "") -> list[dict]:
        """One page of characters: by name search if query else the most-favorited list."""
        if query:
            data = self.get("/characters", q=query, page=page, limit=PAGE_SIZE,
                            order_by="favorites", sort="desc")
        else:
            data = self.get("/top/characters", page=page, limit=PAGE_SIZE)
        out = []
        for c in data.get("data") or []:
            img = ((c.get("images") or {}).get("jpg") or {}).get("image_url")
            if c.get("mal_id") and c.get("name") and img:
                about = clean_text(c.get("about"), 400)
                info = extract_facts(about)
                info["about"] = about
                info["favs"] = _favs(c.get("favorites"))
                info["nicknames"] = (c.get("nicknames") or [])[:2]
                out.append({"mal_id": int(c["mal_id"]), "name": c["name"], "gender": None,
                            "url": c.get("url") or "", "img": img, "info": info})
        return out


class AniList(Api):
    """AniList GraphQL. Key = -AniList id, so it never collides with MAL ids. Has gender."""
    name, delay, has_gender = "AniList", 1.0, True  # limit 90 req/min (sometimes degraded to 30)
    URL = "https://graphql.anilist.co"
    QUERY = """query($p:Int,$n:Int,$q:String){Page(page:$p,perPage:$n){
        characters(search:$q,sort:FAVOURITES_DESC){
          id name{full} gender age dateOfBirth{year month day} bloodType favourites
          description image{large} siteUrl}}}"""

    def characters(self, page: int = 1, query: str = "") -> list[dict]:
        data = self.request("POST", self.URL, json={"query": self.QUERY, "variables": {
            "p": page, "n": PAGE_SIZE, "q": query or None}})
        out = []
        for c in ((data.get("data") or {}).get("Page") or {}).get("characters") or []:
            name = (c.get("name") or {}).get("full")
            img = (c.get("image") or {}).get("large")
            if c.get("id") and name and img and "default.jpg" not in img:  # skip placeholder art
                about = clean_text(c.get("description"), 400)
                info = extract_facts(about)
                info["about"] = about
                info["favs"] = _favs(c.get("favourites"))
                if c.get("age"):
                    info["age"] = str(c["age"])
                if fmt_date(c.get("dateOfBirth")):
                    info["birthday"] = fmt_date(c["dateOfBirth"])
                if c.get("bloodType"):
                    info["blood"] = c["bloodType"]
                out.append({"mal_id": -int(c["id"]), "name": name, "gender": c.get("gender"),
                            "url": c.get("siteUrl") or "", "img": img, "info": info})
        return out


def run_search(db, apis, query, target, stop, progress, notify=None, girls_only=True, workers=4,
               use_pool=True):
    """Fetch from every API in parallel and analyse with a thread pool.

    `target` counts NEWLY analysed characters (cached ones are skipped quickly), so searching
    again digs deeper instead of re-showing the same top list. Characters are deduplicated by
    name across APIs. Girls-only needs a source that reports gender (AniList); Jikan does not.
    Returns (new, checked, failed, errors). progress(new, checked, failed) is called per character.
    """
    apis = [a for a in apis if a.has_gender or not girls_only]
    if not apis:
        raise ConnectionError("No usable data source selected (girls-only needs AniList)")
    q: queue.Queue = queue.Queue(maxsize=60)
    halt, prod_done, lock = threading.Event(), threading.Event(), threading.Lock()
    seen: set[str] = set()
    st = {"new": 0, "checked": 0, "failed": 0}
    errors: list[str] = []
    max_pages = max(8, target // 5)
    pool = get_pool() if use_pool else None

    def stopped() -> bool:
        return stop.is_set() or halt.is_set()

    def produce(api):
        api.notify = notify
        try:
            for page in range(1, max_pages + 1):
                if stopped():
                    return
                chars = api.characters(page, query)
                if not chars:
                    return
                for ch in chars:
                    if girls_only and ch.get("gender") != "Female":
                        continue
                    key = ch["name"].casefold()
                    with lock:
                        if key in seen:
                            continue
                        seen.add(key)
                    while not stopped():
                        try:
                            q.put(ch, timeout=0.5)
                            break
                        except queue.Full:
                            pass
        except (ConnectionError, requests.RequestException, RuntimeError) as e:
            log.warning("%s failed: %s", api.name, e)
            errors.append(f"{api.name}: {str(e)[:150]}")
            if notify:
                notify(f"{api.name} unavailable, continuing with the others...")

    def consume():
        while not stopped():
            try:
                ch = q.get(timeout=0.3)
            except queue.Empty:
                if prod_done.is_set():
                    return
                continue
            new, failed = False, False
            try:
                new = analyze(db, ch, pool)
            except Exception as e:  # one bad character never kills the run
                failed = True
                log.warning("skip %s: %s", ch["name"], e)
            with lock:
                st["checked"] += 1
                st["new"] += new
                st["failed"] += failed
                if st["new"] >= target:
                    halt.set()
                snap = (st["new"], st["checked"], st["failed"])
            progress(*snap)

    producers = [threading.Thread(target=produce, args=(a,), daemon=True) for a in apis]
    consumers = [threading.Thread(target=consume, daemon=True) for _ in range(workers)]
    for t in producers + consumers:
        t.start()
    for t in producers:
        t.join()
    prod_done.set()
    for t in consumers:
        t.join()
    if len(errors) == len(apis) and st["checked"] == 0:
        raise ConnectionError("; ".join(errors))
    return st["new"], st["checked"], st["failed"], errors


def download(url: str) -> bytes:
    """Fetch image bytes; rejects non-images and oversize files. Never executes anything."""
    with requests.get(url, timeout=8, stream=True, headers=HEADERS) as r:
        r.raise_for_status()
        if not r.headers.get("content-type", "").startswith("image/"):
            raise ValueError("not an image")
        data = bytearray()
        for chunk in r.iter_content(65536):
            data += chunk
            if len(data) > MAX_BYTES:
                raise ValueError("image too large")
    return bytes(data)


# ----------------------------------------------------------------- cache DB
class DB:
    def __init__(self, path: Path = DATA / "cache.db"):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.images = path.parent / "images"
        self.images.mkdir(exist_ok=True)
        self.lock = threading.Lock()
        self.c = sqlite3.connect(path, check_same_thread=False)
        self.c.execute("""CREATE TABLE IF NOT EXISTS chars(
            mal_id INTEGER PRIMARY KEY, name TEXT, url TEXT, img_url TEXT, img_path TEXT,
            img_hash TEXT, r INT, g INT, b INT, lab_l REAL, lab_a REAL, lab_b REAL,
            conf REAL, version INT, ts REAL, gender TEXT, info TEXT)""")  # r NULL = no hair found
        for col in ("gender TEXT", "info TEXT"):  # migrate databases from older versions
            try:
                self.c.execute(f"ALTER TABLE chars ADD COLUMN {col}")
            except sqlite3.OperationalError:
                pass
        self.c.commit()

    def get(self, mal_id: int):
        with self.lock:
            return self.c.execute("SELECT img_url, version, gender, info FROM chars WHERE mal_id=?",
                                  (mal_id,)).fetchone()

    def backfill(self, ch: dict) -> None:
        """Fill gender/info on rows cached before that metadata existed."""
        with self.lock, self.c:
            self.c.execute("UPDATE chars SET gender=COALESCE(gender,?), info=COALESCE(info,?) "
                           "WHERE mal_id=?", (ch.get("gender"), json.dumps(ch.get("info") or {}),
                                              ch["mal_id"]))

    def save(self, ch: dict, path: Path, digest: str, res) -> None:
        rgb, conf = res if res else (None, None)
        lab = [float(v) for v in to_lab(rgb)] if rgb else [None] * 3
        with self.lock, self.c:
            self.c.execute("INSERT OR REPLACE INTO chars VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                           (ch["mal_id"], ch["name"], ch["url"], ch["img"], str(path), digest,
                            *(rgb or (None,) * 3), *lab, conf, VERSION, time.time(),
                            ch.get("gender"), json.dumps(ch.get("info") or {})))

    def ranked(self, target, tol: float = 50.0, min_conf: float = 0.0,
               method: str = "CIEDE2000", query: str = "", girls_only: bool = False,
               reject_hair_conflicts: bool = True) -> list[dict]:
        """Re-rank cached analyses against `target`. No network involved.

        reject_hair_conflicts: skip characters whose listed hair color (from API text) is
        clearly a different color family than the target, whatever the image analysis says.
        """
        with self.lock:
            rows = self.c.execute(
                "SELECT mal_id,name,url,img_path,r,g,b,lab_l,lab_a,lab_b,conf,ts,gender,info "
                "FROM chars WHERE r IS NOT NULL AND version=? AND conf>=? "
                "AND lower(name) LIKE lower(?) AND (?=0 OR gender='Female')",
                (VERSION, min_conf, f"%{query}%", int(girls_only))).fetchall()
        if not rows:
            return []
        d = delta_e(np.array([r[7:10] for r in rows]), to_lab(target), method)
        out = []
        for r, de in zip(rows, d):
            info = json.loads(r[13] or "{}")
            if de > tol or (reject_hair_conflicts and info.get("hair")
                            and hair_conflict(target, info["hair"])):
                continue
            out.append(dict(id=r[0], name=r[1], url=r[2], path=r[3], rgb=tuple(r[4:7]),
                            conf=r[10], ts=r[11], gender=r[12], info=info,
                            de=float(de), sim=similarity(de)))
        return out


def analyze(db: DB, ch: dict, pool=None) -> bool:
    """Download + analyse one character unless already cached. True if work was done."""
    row = db.get(ch["mal_id"])
    path = db.images / f"{ch['mal_id']}.jpg"  # a small thumbnail; the original is not kept
    same_image = bool(row and row[0] == ch["img"])
    if same_image and row[1] == VERSION:
        if (ch.get("gender") and not row[2]) or (ch.get("info") and not row[3]):
            db.backfill(ch)
        return False
    reuse = same_image and path.exists()  # stale analysis: redo from the saved thumbnail
    data = path.read_bytes() if reuse else download(ch["img"])
    try:
        if not reuse:
            with Image.open(io.BytesIO(data)) as im:
                t = im.convert("RGB")
                t.thumbnail(THUMB)
                t.save(path, "JPEG", quality=85)
        try:
            res = pool.submit(detect_bytes, data).result() if pool else detect_bytes(data)
        except BrokenProcessPool:  # worker died: fall back to this process
            res = detect_bytes(data)
    except Exception as e:  # corrupt image: record "no hair" so we don't retry forever
        log.warning("decode failed for %s: %s", ch["name"], e)
        res = None
    db.save(ch, path, hashlib.sha256(data).hexdigest(), res)
    return True


def reanalyze_stale(db: DB, use_pool: bool = True) -> int:
    """Re-run detection on saved thumbnails whose analysis predates VERSION. No network."""
    with db.lock:
        rows = db.c.execute("SELECT mal_id, img_path FROM chars WHERE version<>?",
                            (VERSION,)).fetchall()
    if not rows:
        return 0
    pool = get_pool() if use_pool else None
    jobs = []
    for mal_id, path in rows:
        try:
            data = Path(path).read_bytes()
            jobs.append((mal_id, pool.submit(detect_bytes, data) if pool else data))
        except Exception:
            jobs.append((mal_id, None))
    for mal_id, job in jobs:
        try:
            res = (job.result() if pool else detect_bytes(job)) if job is not None else None
        except Exception:
            res = None
        rgb, conf = res if res else (None, None)
        lab = [float(v) for v in to_lab(rgb)] if rgb else [None] * 3
        with db.lock, db.c:
            db.c.execute("UPDATE chars SET r=?,g=?,b=?,lab_l=?,lab_a=?,lab_b=?,conf=?,version=? "
                         "WHERE mal_id=?", (*(rgb or (None,) * 3), *lab, conf, VERSION, mal_id))
    return len(rows)