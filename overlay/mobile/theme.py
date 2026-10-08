#!/usr/bin/env python3
"""Adapt standard Omarchy colors.toml palettes without executing theme code."""
import json
import fcntl
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tomllib

HOME = Path.home()
CONFIG = Path(os.getenv("XDG_CONFIG_HOME", HOME / ".config"))
STATE = Path(os.getenv("XDG_STATE_HOME", HOME / ".local/state"))
DATA = Path(os.getenv("XDG_DATA_HOME", HOME / ".local/share"))
MOBILE = STATE / "omarchy-mobile"
ROOTS = [DATA / "omarchy-mobile/themes", Path(os.getenv("OMARCHY_PATH", "/usr/share/omarchy")) / "themes", CONFIG / "omarchy/themes"]
DEFAULTS = dict(background="#1a1b26", foreground="#a9b1d6", bright_foreground="#c0caf5", accent="#7aa2f7", selection="#292e42", muted="#414868", lighter_background="#24283b")


def write_changed(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text() == content:
        return False
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(content)
    temporary.replace(path)
    return True


def themes():
    result = {}
    for root in ROOTS:
        for file in sorted(root.glob("*/colors.toml")):
            if re.fullmatch(r"[a-z0-9_][a-z0-9._+-]*", file.parent.name):
                result[file.parent.name] = file
    return result


IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp"}


def theme_context():
    available = themes()
    selected_file = MOBILE / "selected-theme"
    selected = selected_file.read_text().strip() if selected_file.exists() else "tokyo-night"
    file = available.get(selected)
    current = None
    # Support generated modern state and the earlier current-theme symlink.
    for candidate in [STATE / "omarchy/current/theme", CONFIG / "omarchy/current/theme"]:
        if (candidate / "colors.toml").is_file():
            current = candidate
            file = current / "colors.toml"
            namefile = current.parent / "theme.name"
            selected = namefile.read_text().strip() if namefile.exists() else current.resolve().name
            break
    return available, selected, file, current


def image_file(path):
    return path.is_file() and path.resolve().suffix.lower() in IMAGE_SUFFIXES


def wallpaper_ref(path):
    path = Path(path).resolve()
    return {
        "path": str(path),
        "url": path.as_uri() + "?v=" + str(path.stat().st_mtime_ns),
        "name": path.name,
    }


def wallpaper_choices(selected, current):
    slug = selected.lower().replace(" ", "-")
    # Match Omarchy's stock + user overlay model by filename.
    merged = {}
    for root in ROOTS:
        for path in sorted((root / slug / "backgrounds").glob("*")):
            if image_file(path):
                merged[path.name] = path.resolve()
    if current:
        for path in sorted((current / "backgrounds").glob("*")):
            if image_file(path):
                merged[path.name] = path.resolve()
    for path in sorted((CONFIG / "omarchy/backgrounds" / slug).glob("*")):
        if image_file(path):
            merged[path.name] = path.resolve()
    return list(dict.fromkeys(merged[name] for name in sorted(merged)))


def saved_backgrounds():
    try:
        value = json.loads((MOBILE / "backgrounds.json").read_text())
        return value if isinstance(value, dict) else {}
    except (FileNotFoundError, ValueError):
        return {}


def wallpaper_refs(paths):
    refs = []
    for path in paths:
        try:
            refs.append(wallpaper_ref(path))
        except OSError:
            pass
    return refs


def theme_previews(available):
    previews = {}
    for name in available:
        choices = wallpaper_choices(name, None)
        if choices:
            try:
                previews[name] = wallpaper_ref(choices[0])["url"]
            except OSError:
                pass
    return previews


def wallpaper_pick(selected, current, name):
    if current:
        raise ValueError("Use Omarchy's background command to change its current state")
    choices = wallpaper_choices(selected, current)
    if not choices:
        raise ValueError("This theme has no wallpapers")
    chosen = next((path for path in choices if path.name == name or str(path) == name), None)
    if chosen is None:
        raise ValueError("Unknown wallpaper")
    saved = saved_backgrounds()
    saved[selected] = str(chosen)
    write_changed(MOBILE / "backgrounds.json", json.dumps(saved, indent=2) + "\n")
    return chosen


def wallpaper_state(selected, current, advance=False):
    choices = wallpaper_choices(selected, current)
    saved = saved_backgrounds()
    chosen = None
    if current:
        link = current.parent / "background"
        if image_file(link):
            chosen = link.resolve()
    else:
        remembered = saved.get(selected)
        if isinstance(remembered, str) and Path(remembered) in choices:
            chosen = Path(remembered)
    if advance:
        if current:
            raise ValueError("Use Omarchy's background command to change its current state")
        if not choices:
            raise ValueError("This theme has no wallpapers")
        index = choices.index(chosen) if chosen in choices else -1
        chosen = choices[(index + 1) % len(choices)]
    elif chosen is None and choices:
        chosen = choices[0]
    if not current and chosen:
        saved[selected] = str(chosen)
        write_changed(MOBILE / "backgrounds.json", json.dumps(saved, indent=2) + "\n")
    ref = wallpaper_ref(chosen) if chosen else {"path": "", "url": "", "name": ""}
    ref["index"] = choices.index(chosen) + 1 if chosen in choices else 0
    ref["count"] = len(choices)
    ref["choices"] = wallpaper_refs(choices)
    return ref


def uncommented(text):
    return "\n".join(line.split("--", 1)[0] for line in text.splitlines())


def rounding_value(text):
    match = re.search(r"\brounding\s*=\s*(\d+)", uncommented(text))
    return int(match.group(1)) if match else None


def corner_radius():
    """Return the corner radius, or None when nobody has chosen one.

    The phone's Appearance setting wins. Otherwise Omarchy's Style > Corners
    toggle wins over looknfeel.lua. Square is 0. A commented line does not count.
    """
    try:
        prefs = json.loads((CONFIG / "omarchy-mobile/prefs.json").read_text())
    except (OSError, ValueError):
        prefs = {}
    if prefs.get("corners") == "square":
        return 0
    if prefs.get("corners") == "round":
        return 8
    toggle_dir = STATE / "omarchy/toggles/hypr"
    for name in ("rounded-corners.lua", "rounded-corners.conf"):
        path = toggle_dir / name
        if path.is_file():
            value = rounding_value(path.read_text())
            return 8 if value is None else value
    look = CONFIG / "hypr/looknfeel.lua"
    if look.is_file():
        value = rounding_value(look.read_text())
        if value is not None:
            return value
    return None


def palette(file):
    data = tomllib.loads(file.read_text()) if file else {}
    # Support both current semantic names and older Omarchy ANSI palettes.
    aliases = {"accent": "color4", "muted": "color8", "bright_foreground": "color15"}
    colors = dict(DEFAULTS)
    for key, value in data.items():
        if isinstance(value, str) and re.fullmatch(r"#[0-9a-fA-F]{6}", value):
            colors[key] = value
    for key, alias in aliases.items():
        if key not in data and alias in colors:
            colors[key] = colors[alias]
    if "lighter_background" not in data:
        bg = [int(colors["background"][i:i+2], 16) for i in (1, 3, 5)]
        fg = [int(colors["foreground"][i:i+2], 16) for i in (1, 3, 5)]
        colors["lighter_background"] = "#" + "".join(f"{round(a*.9+b*.1):02x}" for a, b in zip(bg, fg))
    return colors


def signal_owned(name, sig):
    result = subprocess.run(["pgrep", "-u", str(os.getuid()), "-x", name], capture_output=True, text=True)
    for pid in result.stdout.split():
        try:
            os.kill(int(pid), sig)
        except ProcessLookupError:
            pass


def sync():
    available, selected, file, current = theme_context()
    colors = palette(file)
    devicefile = CONFIG / "omarchy-mobile/device.json"
    device = json.loads(devicefile.read_text()) if devicefile.exists() else {}
    font = "JetBrainsMono Nerd Font"
    try:
        prefs = json.loads((CONFIG / "omarchy-mobile/prefs.json").read_text())
        candidate = prefs.get("fontFamily")
        if isinstance(candidate, str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9 +._-]{0,78}", candidate):
            font = candidate
    except (OSError, ValueError):
        pass
    chosen = corner_radius()
    try:
        previous_palette = json.loads((MOBILE / "palette.json").read_text())
    except (OSError, ValueError):
        previous_palette = {}
    if chosen is None:
        radius = previous_palette.get("radius") if isinstance(previous_palette.get("radius"), int) else 8
    else:
        radius = chosen
    result = {"name": selected, "colors": colors, "themes": sorted(available), "device": device,
              "wallpaper": wallpaper_state(selected, current), "fontFamily": font,
              "themePreviews": theme_previews(available),
              "corners": "round" if radius else "square", "radius": radius}
    border = 'hl.config({general={col={active_border={colors={"rgba(' + colors['accent'][1:] + 'ff)"},angle=0},inactive_border="rgba(' + colors['muted'][1:] + 'ff)"}}})'
    rounding = "hl.config({decoration={rounding=" + str(radius) + "}})"
    lua = 'dofile(' + json.dumps(str(DATA / 'omarchy-mobile/hypr-mobile.lua')) + ')\n'
    scale = device.get('scale')
    # A board may ask for a mode other than the panel's preferred one (90 Hz on guacamole).
    mode = device.get('displayMode')
    if not (isinstance(mode, str) and re.fullmatch(r'\d+x\d+@\d+(\.\d+)?', mode)):
        mode = 'preferred'
    if isinstance(scale, (int, float)) and not isinstance(scale, bool) and 0.5 <= scale <= 5:
        lua += 'hl.monitor({output="",mode="' + mode + '",position="auto",scale=' + str(scale) + '})\n'
    lua += border + '\n' + rounding + '\n'
    shortcut = device.get('terminalShortcut', '')
    if re.fullmatch(r'[A-Z0-9 +]+', shortcut):
        lua += 'hl.unbind(' + json.dumps(shortcut) + ')\nhl.bind(' + json.dumps(shortcut) + ', hl.dsp.exec_cmd("env KITTY_MOBILE_TOUCH=1 kitty"))\n'
    write_changed(CONFIG / 'hypr/mobile.lua', lua)
    previous = previous_palette
    # Optional trusted device integration, separate from imported theme assets.
    wallpaper_hook = CONFIG / "omarchy-mobile/wallpaper-apply"
    previous_wallpaper = (previous.get("wallpaper") or {}).get("path")
    if previous_wallpaper != result["wallpaper"].get("path") and os.access(wallpaper_hook, os.X_OK):
        subprocess.run([str(wallpaper_hook), result["wallpaper"]["path"]], check=True,
                       stdout=subprocess.DEVNULL)
    write_changed(MOBILE / "palette.json", json.dumps(result, indent=2) + "\n")
    if previous.get("fontFamily") != font:
        write_changed(CONFIG / "kitty/mobile-font.conf", f"font_family {font}\n")
    # Wallpaper changes must not restart the keyboard or reconfigure terminals.
    if previous.get("colors") != colors or previous.get("fontFamily") != font:
        lines = [f"background {colors['background']}", f"foreground {colors['foreground']}", f"cursor {colors['accent']}",
                 f"selection_background {colors.get('selection_background', colors['selection'])}", f"selection_foreground {colors.get('selection_foreground', colors['bright_foreground'])}"]
        ansi = ["background", "red", "green", "yellow", "blue", "magenta", "cyan", "foreground", "muted", "bright_red", "bright_green", "bright_yellow", "bright_blue", "bright_magenta", "bright_cyan", "bright_foreground"]
        for i, name in enumerate(ansi):
            lines.append(f"color{i} {colors.get(name, colors.get('color'+str(i), colors['foreground']))}")
        write_changed(CONFIG / "kitty/mobile-theme.conf", "\n".join(lines) + "\n")
        signal_owned("kitty", signal.SIGUSR1)
        # The keyboard (keyboard/shell.qml) takes the new colours by itself;
        # wvkbd, the fallback, only the next time the helper starts it (a
        # restart here would see Settings' text-input and flash it).
        helper = HOME / ".local/bin/omarchy-mobile-keyboard"
        if helper.exists() and os.getenv("WAYLAND_DISPLAY"):
            subprocess.run([str(helper), "hide"], stdout=subprocess.DEVNULL, check=False)
        # Only update the border colors, retaining the user's layout/bindings.
        subprocess.run(["hyprctl", "eval", border], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if previous.get("radius") != radius and os.getenv("WAYLAND_DISPLAY"):
        subprocess.run(["hyprctl", "eval", rounding], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return result


def main():
    args = sys.argv[1:]
    delegated = False
    if len(args) == 2 and args[0] == "set":
        if args[1] not in themes():
            raise ValueError("Unknown installed theme")
        if shutil.which("omarchy"):
            # Run outside our lock: Omarchy hooks may call mobile sync.
            subprocess.run(["omarchy", "theme", "set", args[1]], check=True, stdout=sys.stderr)
            delegated = True
    elif args == ["background", "next"]:
        if shutil.which("omarchy"):
            subprocess.run(["omarchy", "theme", "bg", "next"], check=True, stdout=sys.stderr)
            delegated = True
    elif len(args) == 3 and args[0] == "background" and args[1] == "set":
        if shutil.which("omarchy"):
            _, selected, _, current = theme_context()
            chosen = next((path for path in wallpaper_choices(selected, current)
                           if path.name == args[2] or str(path) == args[2]), None)
            if chosen is None:
                raise ValueError("Unknown wallpaper")
            subprocess.run(["omarchy", "theme", "bg", "set", str(chosen)], check=True, stdout=sys.stderr)
            delegated = True
    elif args not in ([], ["sync"]):
        raise ValueError("Usage: omarchy-mobile-theme [sync|set THEME|background next|background set NAME]")
    MOBILE.mkdir(parents=True, exist_ok=True)
    with (MOBILE / "theme.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not delegated and len(args) == 2 and args[0] == "set":
            write_changed(MOBILE / "selected-theme", args[1] + "\n")
        elif not delegated and args == ["background", "next"]:
            _, selected, _, current = theme_context()
            wallpaper_state(selected, current, advance=True)
        elif not delegated and len(args) == 3 and args[0] == "background" and args[1] == "set":
            _, selected, _, current = theme_context()
            wallpaper_pick(selected, current, args[2])
        print(json.dumps(sync()))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
