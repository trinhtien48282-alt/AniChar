# AniChar C++ Remaster

Qt 6 desktop application for anime hair color search, local hair analysis, and SQLite caching. C++ includes Jikan and AniList character search, remote image download and analysis, metadata extraction, Python-parity RGB/Lab color math, and CIEDE2000 ranking.

## Build on Windows

Requires CMake 3.20+, Qt 6 (Core, Gui, Widgets, Network), SQLite3, and MinGW-w64. From the repository root:

```powershell
$env:PATH = "$(Split-Path (Get-Command c++.exe).Source);$env:PATH"
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Put the selected MinGW runtime first in `PATH` so another installed compiler runtime cannot shadow it.

Launch `build\AniChar.exe`. The app supports target color entry and picking, cached-result refresh, instant cache filters and sorting, online source selection, girls-only filtering, search depth, cancellation, and local image analysis. Girls-only mode uses AniList because Jikan does not provide gender. Qt image plugins determine available local formats; PNG, JPEG, WebP, BMP, GIF, and TIFF are offered when supported by installed plugins.

## API behavior

Jikan requests use the public v4 API: `/characters?q=...&page=...&limit=25&order_by=favorites&sort=desc` for name searches, and `/top/characters?page=...&limit=25` otherwise. AniList sends a GraphQL POST to `https://graphql.anilist.co`, using `Page(page, perPage: 25)` and sorting characters by favourites. Both requests are read-only and use no authentication. See the [Jikan API reference](https://docs.api.jikan.moe/) and [AniList API documentation](https://anilist.gitbook.io/anilist-apiv2-docs/).

Each selected source is paged independently, up to `max(8, target / 5)` pages. Results deduplicate by case-folded character name. Girls-only filtering uses AniList's `gender`; Jikan does not provide that field. Retries are bounded; numeric `Retry-After` seconds are capped at 30 seconds. Request timeouts are 5 seconds for Jikan, 8 seconds for AniList, and 8 seconds for image downloads. A failed source does not stop another selected source.

## Cache and image analysis

The cache reuses the Python reference location, `~/.anime_hair_finder/cache.db`, and its SQLite schema. Downloaded cards are saved under `~/.anime_hair_finder/images/`; image bytes are SHA-256 hashed and capped at 5 MB. Stale analyses are refreshed from cached thumbnails during startup, without network access. Image downloads are asynchronous; decoding and analysis use a four-worker pool. Local and remote images decode through Qt, apply EXIF orientation, convert to RGB, and use the existing detector. Character descriptions, age, birthday, hair color, favorites, nicknames, gender, and AniList blood type are shown in result cards.

## Known differences and limits

- Python fetches both sources in producer threads and analyzes with up to four consumers. C++ fetches source pages asynchronously but processes candidates sequentially within each source; image analysis is pooled. Search may take longer for large targets.
- Python displays 60 cards at a time and adds more on demand. C++ currently rebuilds all matching result cards on each refresh, so large caches can make filtering or progress refreshes slower.
- Local image analysis runs synchronously in the C++ GUI thread. Large local files can briefly delay the interface.
- Qt applies EXIF orientation; the Python Pillow path does not explicitly apply it, so some rotated images can produce different results.
- Numeric `Retry-After` values are supported; HTTP-date values are not parsed. Live Jikan connectivity was unavailable during the 2026-10-30 audit, so its live response path was not verified in that run.
- Hair detection remains a heuristic for a single portrait, with no face detector. It assumes the head is near the top of the image; multi-character or decorated images can reduce accuracy.

## Python reference and tests

`main.py` and `hairfinder.py` remain the behavioral reference. Run the Python version with dependencies in `pyproject.toml` using `uv sync` and `uv run python main.py`. CTest uses local mocked HTTP responses and does not require live API access. See [AUDIT_REPORT.md](AUDIT_REPORT.md) for the integration and regression audit results.
