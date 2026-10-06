# Anime Hair Color Finder

Pick a color, get anime characters (from Jikan / MyAnimeList, with AniList as automatic fallback) whose estimated hair color is closest.

## Run (Windows)
```
python -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
python main.py
python test_hairfinder.py      # tests
```

## Sources and speed
Jikan and AniList are queried **in parallel**, characters are deduplicated by name, and images are
downloaded by 4 threads and analysed in separate worker processes (all CPU cores, GUI never starved).
The grid is drawn incrementally (60 cards, 'Show more' for the rest), refreshed at most every 1.5 s during a search.
Only small thumbnails are kept on disk. Only AniList reports gender, so **Girls only** (on by default)
uses AniList; untick it to include Jikan too. Jikan timeout is 5 s; a dead source is skipped, not fatal.

## Hair color detection (heuristic)
Clusters are grouped by color family, so highlight + shadow of blue hair count as one blue group; a
black/gray winner is replaced by a clearly present colored group (dark shading reads as black).
Each card shows what the app thinks the hair is (`App sees hair as: blue`) next to the hair color
listed in the API text, and 'Hide listed-hair conflicts' drops characters whose description names a
different color than your target. Old cached analyses are redone offline from saved thumbnails.

## How it works
1. Jikan, or AniList if Jikan is unreachable (`/top/characters`, or `/characters?q=` when a name is typed) lists characters, throttled to
   ~1 request/sec with bounded exponential backoff and `Retry-After` handling on 429.
2. Each thumbnail is downloaded once (content-type and 5 MB checks), SHA-256 hashed, and analysed.
3. Hair detection: mask plain border-colored background, k-means (Lab) on the top 45% of the image,
   pick the cluster by area x height, penalising skin-like clusters.
4. Result (RGB, Lab, confidence, version) goes into SQLite at `~/.anime_hair_finder/cache.db`.
   Changing the target color, tolerance, sort or filters only re-ranks cached rows: no network.
5. Distance is CIEDE2000 in Lab (CIE76 available via `delta_e(..., "CIE76")`).
   Similarity = `100 * (1 - dE / 50)`, floored at 0. It is a color-distance score, not a pixel-match percent.

Confidence is an internal heuristic (cluster dominance, tightness, coverage), not a measured accuracy.

## Limitations
No face detector: assumes the head is in the top ~45% of the frame. One image per character
(the list response's image). Multi-character or heavily decorated images lower confidence, or fool it.

## Cache / data
Everything lives in `~/.anime_hair_finder/` (`cache.db`, `images/`, `app.log`). Delete the folder to clear it.
Bump `VERSION` in `hairfinder.py` after changing the detector to re-analyse.

## Attribution
Data via Jikan (unofficial MyAnimeList API) and AniList (GraphQL; ids stored negative to avoid clashes). Character images belong to their rights holders;
this tool only caches them locally for analysis and claims no ownership.