# Home screen widgets

A widget is a folder in `~/.local/share/omarchy-mobile/widgets/` (the shell's
`install.sh` puts `clock/` there; an app's installer can add its own):

- `manifest.json`: `{"id", "name", "description", "sizes": [[w, h], ...]}`,
  sizes in grid cells, the first being the one offered first. Editing the
  home screen lists every widget found; a widget's resize button steps
  through its sizes.
- `Widget.qml`: the root item fills its cells. It must declare these, which
  the home screen sets:
  - `property var host`: the home screen. `host.open(appId)` opens an app
    (from the widget, with the launch animation), `host.weather` is the
    shell's weather (what `omarchy-mobile-weather status` prints),
    `host.weatherWanted()` asks for it when old, `host.weatherOpened()`
    shows the forecast.
  - `property bool active`: true while the widget's page is on screen and
    the screen is on. Fetch, tick and animate only then; nothing runs
    while an app is in front, the drawer is open or the screen is off.
  - `property int spanX`, `property int spanY`: its size in cells, to lay
    itself out for each size.

`import OmarchyMobile` gives `MobileTheme` (colours, font, corner radius) and
the shell's controls. While the home screen is being edited, a widget takes
no input.
