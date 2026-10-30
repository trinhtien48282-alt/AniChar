# AniChar C++ remaster integration and regression audit

Audit date: 2026-10-30

## Scope and conclusion

Reviewed the C++ application, tests, build configuration, README, and Python reference (`main.py`, `hairfinder.py`). No full-parity claim: the C++ implementation retains several UI, scheduling, and image-orientation differences listed below. One rate-limit bug in the image download path was reproduced with a mock and fixed.

## Verified behavior

| Area | Evidence / result |
|---|---|
| Jikan request contract | Mock tests verify name search `/v4/characters` with `q`, `page`, `limit=25`, `order_by=favorites`, `sort=desc`; blank query uses `/v4/top/characters`. Pagination, 429 retry, connection retry, malformed JSON handling, missing images, and source fallback covered. Live Jikan was unreachable from the audit environment. |
| AniList request contract | Mock tests verify unauthenticated GraphQL POST, variables, response mapping, pagination, gender filtering, metadata, and negative cache IDs. A controlled live POST to `https://graphql.anilist.co` on 2026-10-30 returned Rem's character data and image URL with no authentication. |
| Image integration | Mock HTTP tests fetch PNG bytes, reject wrong content types and files above 5 MB, decode and analyze valid images, store SHA-256 and thumbnail, and persist metadata/results. New regression test confirms JSON HTTP 429 image response retries before content-type validation. |
| Cancellation | Tests cancel before request and during delayed API response; completion callback fires and late response is not cached. Generation checks discard late image-worker results. |
| Offline cache refresh | Test marks a cached row stale, reanalyzes its saved thumbnail with no network, updates RGB/version, and preserves metadata and timestamp. Startup queues stale-image work off the GUI thread. |
| Detection/color/cache | C++ detector fixtures match Python expected RGB and confidence; cache tests cover ranking, gender, name query, sorting, metadata backfill, and stale versions. |
| Responsiveness | Network I/O is Qt asynchronous. Image analysis and stale reanalysis use a four-thread pool; mocked delayed-request cancellation test proves the Qt event loop can cancel a live request. Local image analysis and result-card rebuilds remain synchronous. |
| Resource handling | SQLite statements finalize through RAII; SQLite opens with `FULLMUTEX`, and cache access is mutex-protected. Replies are removed and `deleteLater`'d; active replies abort on cancel/finish. The worker pool is joined during teardown. No sanitizer or forced-window-close stress run was performed. |

## API contract review

- Jikan's official v4 reference documents the public REST endpoints and rate-limit policy. The implementation uses `/characters` for text search and `/top/characters` otherwise. It sends the documented paging and ordering parameters, parses `data[]`, `mal_id`, `name`, `url`, `about`, `favorites`, `nicknames`, and `images.jpg.image_url`. Jikan does not provide gender, so girls-only mode skips it.
- AniList's official API is GraphQL. The implementation posts query plus variables (`p`, `n`, `q`) to `https://graphql.anilist.co`, requests `Page.characters`, and parses identity, gender, age, date of birth, blood type, favorites, description, image, and site URL. The tested public character query succeeded without an authorization header. No private fields or mutations are used.
- Both endpoints paginate at 25 records per page. Each source stops on an empty page, target completion, or `max(8, target / 5)` pages. Results deduplicate by case-folded name.
- Requests have finite timeouts. 429 and 5xx responses get bounded retries; connection failures get bounded retries. Numeric `Retry-After` values are honored up to 30 seconds; otherwise exponential delays apply. HTTP-date `Retry-After` values are not parsed. Rate limits can change; Jikan documentation currently states 3 requests/second and 60/minute, while this app spaces requests by at least 1.1 seconds. AniList requests are spaced by at least 1 second.
- API references: [Jikan REST API v4](https://docs.api.jikan.moe/), [AniList API docs](https://anilist.gitbook.io/anilist-apiv2-docs/), [AniList GraphQL endpoint](https://graphql.anilist.co).

## Build and automated tests

Fresh Release build in a new `audit-build` directory with MinGW-w64 (`GNU 16.2.0`), Qt 6, and SQLite 3.53.4 completed successfully. After the fix, `cmake --build audit-build --clean-first --parallel` succeeded and `ctest --test-dir audit-build --output-on-failure` passed all four suites: engine (0.12 s), cache (0.16 s), detector (0.16 s), and online search including the new retry regression (7.89 s). Total test time: 8.35 s.

## Python/C++ differences and unresolved limits

1. Python fetches Jikan and AniList concurrently in producer threads and uses up to four consumers for downloads/analysis. C++ starts page requests asynchronously but advances candidates sequentially within each source; only image analysis uses the four-worker pool. Large searches can therefore take longer.
2. Python shows 60 cards at a time and creates more on demand, reusing cards across refreshes. C++ reconstructs every matching card on each refresh. Large caches may block filtering or progress updates.
3. C++ local image decoding and hair analysis run on the GUI thread and can briefly block interaction for large files.
4. Qt applies EXIF orientation. Python's Pillow path converts to RGB without explicitly applying EXIF orientation, so rotated source images can yield different analysis.
5. UI interaction differs: Python has an explicit Cache only/Online toggle and incremental Show more; C++ offers a Refresh cached results button and displays all matches. C++ target-color edits refresh on Enter, while Python also applies on focus loss.
6. Live Jikan requests were not verified because the Jikan connection timed out in this environment. AniList live query was verified. Its returned image CDN URL returned HTTP 200 (118,483-byte PNG), decoded with Pillow as 230×345 RGB. The live CDN image was not passed through the C++ detector; mocked C++ download, Qt decode, analysis, and cache flow passed.
7. No automated interactive GUI session, sanitizer run, high-volume cache stress test, HTTP-date retry test, or window-close-during-worker stress test was performed.

## Post-fix validation

The clean MinGW rebuild and all four CTest suites passed after the image 429 retry fix.
