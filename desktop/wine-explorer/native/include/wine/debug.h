/* SPDX-License-Identifier: MIT
 * Compile-time replacement for Wine's tracing transport in this standalone
 * Windows-side source build. No Wine/Unix debug entry points are imported.
 * Startup errors use WineExplorerSupport's real report/message box instead.
 */
#ifndef MIHAWK_WINE_STANDALONE_DEBUG_H
#define MIHAWK_WINE_STANDALONE_DEBUG_H
#define WINE_DEFAULT_DEBUG_CHANNEL(name)
#define TRACE(...) ((void)0)
#define WARN(...) ((void)0)
#define ERR(...) ((void)0)
#define FIXME(...) ((void)0)
#endif
