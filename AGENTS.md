# gucode

Keep the application entirely C. Native builds link system libcurl; gucOS builds
link the documented gucOS libcurl subset. Do not add platform runtime imports to
the application. Shared transport options must be supported by that subset.

Use DeepSeek for integration testing until the user directs otherwise. Never
commit keys, local credential paths, private transcripts or API request headers.
Live tests are opt-in and can incur charges. Use machine-job for build/test work;
browser jobs use the browser lane. Documentation belongs under docs/.
