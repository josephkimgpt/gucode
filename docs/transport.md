# Sanctioned curl subset

The application uses only curl_global_init/cleanup, curl_easy_init/cleanup,
curl_easy_setopt/getinfo/perform/strerror, and curl_slist_append/free_all.
Options: URL, FOLLOWLOCATION=1, NOSIGNAL=1, HTTPHEADER, POSTFIELDS,
POSTFIELDSIZE, WRITEFUNCTION, WRITEDATA, ERRORBUFFER, CONNECTTIMEOUT_MS,
TIMEOUT_MS, NOPROGRESS, XFERINFOFUNCTION. Info: RESPONSE_CODE.

All setopt results are checked. This source builds against native macOS libcurl
and the gucOS package; no `__http_*` or other browser runtime imports appear in the
application. Runtime imports belong only to the package.

The subset is specified in the package's README:
https://github.com/GroundUpCoder/gucos-packages/tree/main/apps/libcurl

DeepSeek uses HTTPS with explicit bearer authorization and streamed POST responses.
Use only trusted API endpoints. Browser CORS must permit the page origin, POST,
Authorization and Content-Type. An OPTIONS probe is only preliminary evidence;
real guest requests are the acceptance test.

The 1.0.1 runtime always follows redirects. The subset therefore requires explicit
FOLLOWLOCATION=1 and cannot restrict redirect destinations or observe intermediate
hops. The application validates its initial URL (HTTPS, or an exact loopback HTTP
host and port for local tests), but cannot enforce that policy on every redirect
with this platform API. Do not supply an untrusted redirecting endpoint; HTTPS
production hosting additionally applies the browser's mixed-content restrictions.
No full redirect-security guarantee is claimed. Adding per-request redirect policy
would require extending the core HTTP interface.

Source references:
- https://api-docs.deepseek.com/api/create-chat-completion/
- https://api-docs.deepseek.com/guides/tool_calls/
- https://curl.se/libcurl/c/curl_easy_setopt.html
