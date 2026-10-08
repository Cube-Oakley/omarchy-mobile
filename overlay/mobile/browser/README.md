# Browser

Omarchy Mobile's browser: Chromium's engine through Qt WebEngine, with a phone
interface (docs/apps-plan-20261002.md).

- `main.cpp`: the host. Quickshell cannot host WebEngine, so this program
  initializes it and gives the QML the Omarchy theme (`palette.json`), the
  shell's back gesture, the on-screen keyboard and one persistent profile.
  The profile keeps cookies and cache, sends a phone's user agent (a desktop
  one for sites switched to desktop mode), saves downloads to `~/Downloads`,
  and blocks the domains listed in
  `~/.local/share/omarchy-mobile/browser/blocklist.txt`. That file takes hosts
  lines, plain domains or `||domain^` rules; without it nothing is blocked.
- `Browser.qml`: the interface.
  - A bar at the bottom: tabs, the address (tap to type: an address or a
    search), and the menu (back, forward, reload, new tab, desktop site,
    bookmark, copy link, find in page, close tab, clear history).
  - A sideways swipe on the bar changes tab.
  - Overscroll as on iOS: a pull past the page's top or bottom stretches
    the page after the finger, with growing resistance, and it springs back
    on release. A page script reports the pull through the console, as the
    keyboard's does. Lists inside a page that can still scroll are left
    alone.
  - Pull to refresh: past the top, a circle shows in the gap above the page
    and turns to the accent colour once letting go will reload. A page that
    sets `overscroll-behavior` none or contain on its root still stretches;
    sites do that to turn off Chrome's own pull to refresh (X has its own),
    so for them there is no circle and no reload.
  - Find in page (the menu): the bar holds the search, the match count and
    next and previous.
  - The shell's left-edge swipe goes back a page.
  - A tab overview of cards, each the page's own shape with a picture of
    it as it last looked (taken when the page finishes loading, and before
    it is left or the overview opens), and its icon and title.
  - Full-screen video, sites' permission prompts, download notices.
  - The keyboard comes up for a page's text fields: a page script reports
    focus through the console.
  - A page's endless `requestAnimationFrame` loop is paced once it has run
    for a second with no touch, key or scroll: about 20 frames a second,
    then 10 after five seconds, with full rate again at the next touch;
    while a video plays on the page, no lower than 30 (a player may draw
    its video into a canvas). Video, CSS animations and scrolling are not
    paced. X's two animated Spaces avatars kept the screen at ~112 frames
    a second and 1.0-1.2 A with nothing moving to the eye. Paced, X at
    rest draws ~0.37 A, against 0.24 A standing still (October 7).
  - Pages go to Hyprland directly (Qt WebEngine patch 0007): Chromium's
    frames are a sub-surface below the window, which leaves a transparent
    hole for them, and only what changed is redrawn, in Chromium and in
    Hyprland; Qt draws nothing for a page's frames. A 57-pixel animation
    went from as much GPU and memory power as a full-screen change to
    under half. The window has an alpha channel for it (`main.cpp`); a
    page picture for the tab overview has Qt draw the page for a few
    frames. `OMARCHY_BROWSER_DIRECT=0` has Qt draw the pages, as before.
  - Open tabs come back at the next start (`browser-tabs.json`).
  - Bookmarks (the menu's Bookmark) and history (the last 1,000 pages,
    with visit counts), in `browser-bookmarks.json` and
    `browser-history.json`. While the address is edited, a list above
    the bar offers them, nearest the bar first. Before anything is typed
    it shows the bookmarks and recent pages; after, the matches, bookmarks
    first, then the pages visited most. The menu's Clear history empties
    it.

**Web apps.** The menu's Add to home screen takes a name and makes an icon
(the site's icon, or its first letter, on the theme's accent). It writes
`~/.local/share/applications/omarchy-mobile-webapp-SITE.desktop`, which the
drawer lists, and the icon to `~/.local/share/omarchy-mobile/webapps`. The
entry starts `omarchy-mobile-browser --app URL`:
- one page without the bar;
- its own profile (`webapp-SITE`), so its cookies are separate from the
  browser's;
- its own app ID, which is the entry's name;
- links to other sites open in the browser.

Settings → Apps lists web apps and removes one with its icon and profile.

**X and Grok** come as web apps (`install.sh` adds each once, with the
icons in `../webapps`). Both wear the Omarchy theme:
- **Variables:** both sites keep their colours in CSS variables. X has a
  shadcn layer of HSL parts and its own `--x-bg/fg/border` layer; Grok has
  `--surface-*`, `--fg-*`, `--border-*` and `--button-*`, also set on body.
- **The style sheet:** `siteCss()` builds one from the palette, with X's
  gray scale running from the background to the text. A page script adopts
  it as a constructed sheet when the page is created, which the sites'
  content security policies allow, and a theme change replaces it in the
  open page.
- **Dark or light:** pages are meant to learn it from Qt's colour scheme,
  but without a desktop portal the phone's Qt ignores the request, so
  `prefers-color-scheme` stays light. The script's `matchMedia` answers
  with the theme's scheme instead, which both sites read to pick their dark
  or light look.

- **X signed in:** its timeline is painted from atomic classes and inline
  styles in X's own palette (Lights out and Dim when the theme is dark,
  Default when light), not from the variables. A page script copies X's
  colour rules, in X's order, into a sheet after X's with each palette
  colour replaced by the theme's (alpha kept). Important rules matched on
  the style attribute's text do the same for inline colours. Measured on
  October 5: a full style recalculation took 16 ms with these sheets and 18
  ms without, so there is no cost.
- **X's top bar:** it was a 65% translucent bar with a 12 px backdrop blur
  over the scrolling posts, and it flickered, showing the posts through. A
  rule with a backdrop blur gets the theme's background, opaque, and no
  blur.

Checked on Osaka Jade: Grok's chat and X's sign-in page and timeline take
the theme's background, text, surfaces and accent.

**Further sites** wear the theme from a file in
`~/.config/omarchy-mobile/site-themes/` (a web app whose host is listed):
- `hosts`: the sites it is for (a host, with or without its port, or a
  domain).
- `vars`: the site's CSS variables, each set to a theme colour: a name
  (`background`, `foreground`, `secondary`, `accent`, `surface`,
  `selection`, `muted`), or `mix(a,b,t)`, either with an alpha
  (`surface/0.7`). They go on `selectors` (`:root,html,body` by default).
- `palette`: `"r,g,b"` colours the site paints with to theme colours; its
  style rules are rewritten as X's are (Tailwind's `rgb(r g b / …)` form
  too).
- `css`: more CSS, with `{{colour}}` for a theme colour.

**One process each.** The browser and every web app listen on a socket in
the runtime directory (`omarchy-mobile-browser.sock`,
`omarchy-mobile-webapp-SITE.sock`). A second start hands its address to the
running one, which opens it in a new tab (a web app goes there) and asks the
shell to bring its window forward: `focus ID` on the shell's request FIFO.
The shell's launcher also brings an open browser or web app forward instead
of starting it again.

`install.sh` builds it with cmake and ninja where `qt6-webengine` is
installed. It must not run as root: Chromium's sandbox refuses. Start it as
`omarchy-mobile-browser [URL]` or `omarchy-mobile-browser --app URL`.

**Frame rate.** A finger drag scrolls at the panel's 120 Hz: each touch
event drives a frame. Everything Chromium animates by itself (a fling after
the finger lifts, CSS and script animations, video) runs at 60 Hz. Qt
WebEngine's output device reports no vsync timing when it hands a frame
back, so Chromium keeps its default 60 Hz clock. Measured on October 4:
- **Drag:** 8.3 ms per frame.
- **Fling, `requestAnimationFrame`:** 16.7 ms per frame.
- **`--disable-frame-rate-limit`:** reaches 120 Hz, but Chromium then runs
  unthrottled (`requestAnimationFrame` every 0.4 ms), so it is not used.

The fix is a patch to Qt WebEngine (`NativeSkiaOutputDevice` passing the
screen's refresh rate in its presentation feedback) and a rebuild. The host
enables Chromium's scroll resampling; pages show no scrollbars at all
(touch scrolling), and main-frame navigations, pull-to-refresh reloads and
renderer exits go to `~/.local/state/omarchy-mobile/browser-events.log`, to
tell a page's own reloads from crashes. Qt Quick's
merging of a frame's touch moves stays on: without it, dragging fell to
60 Hz.

Still to come: a page's own colour for the bar, the blocklist's updates, a web
app's own menu (share, open in the browser).
