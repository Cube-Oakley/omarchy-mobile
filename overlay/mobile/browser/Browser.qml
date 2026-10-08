import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import QtWebEngine

// The browser's phone interface (main.cpp hosts it as `host`): the page with
// a bar at the bottom (tabs, the address, the menu), a tab overview of
// cards, and sheets for the menu, permissions and downloads. While the
// address is edited, bookmarks and history are suggested above the bar. The
// shell's back gesture goes back a page. Colours follow the Omarchy theme.
// A web app (host.appMode) is one page without the bar: links to other sites
// open in the browser, and the browser's files are left alone.
Window {
    id: win
    title: "Browser"
    width: 480
    height: 1000
    visible: true
    color: theme.background
    // Closed from the switcher: end the process (the tabs are saved). A
    // process left without its window kept the single-instance socket, so a
    // later start handed it the address and nothing came up.
    onClosing: Qt.quit()

    QtObject {
        id: theme
        readonly property var colors: (host.theme && host.theme.colors) || ({})
        readonly property color background: colors.background || "#1a1b26"
        readonly property color surface: colors.lighter_background || "#24283b"
        readonly property color foreground: colors.bright_foreground || "#c0caf5"
        readonly property color secondary: colors.foreground || "#a9b1d6"
        readonly property color accent: colors.accent || "#7aa2f7"
        readonly property color selection: colors.selection || "#292e42"
        readonly property color muted: colors.muted || "#414868"
        readonly property color danger: colors.red || colors.color1 || "#f7768e"
        readonly property string font: (host.theme && host.theme.fontFamily) || "JetBrainsMono Nerd Font"
        readonly property bool square: host.theme && host.theme.corners ? host.theme.corners === "square" : true
        function radius(size) { return square ? 0 : size; }
    }

    readonly property bool appMode: host.appMode

    // A web app of a site whose own colours are CSS variables wears the
    // Omarchy theme: its variables are set from the palette in a style sheet
    // a page script adopts as the page is created. A constructed sheet is
    // not an inline style, so the sites' content security policies allow it.
    // A theme change reaches the open page at once.
    readonly property string themedSite: {
        if (!appMode) return "";
        const site = hostOf(host.startUrl);
        if (site === "x.com" || site.endsWith(".x.com") || site === "twitter.com") return "x.com";
        if (site === "grok.com" || site.endsWith(".grok.com")) return "grok.com";
        if (site === "discord.com" || site.endsWith(".discord.com")) return "discord.com";
        if (site === "web.telegram.org") return "web.telegram.org";
        return customTheme ? "custom" : "";
    }
    // A further site's theme from ~/.config/omarchy-mobile/site-themes: the
    // file whose "hosts" holds this web app's host (with or without its
    // port), or its domain for a subdomain.
    readonly property var customTheme: {
        if (!appMode) return null;
        const site = hostOf(host.startUrl), name = site.replace(/:\d+$/, "");
        for (const file of host.siteThemes) {
            for (const entry of file.hosts || []) {
                if (entry === site || entry === name || name.endsWith("." + entry)) return file;
            }
        }
        return null;
    }
    // A colour in a site theme file: a theme colour's name (background,
    // foreground, secondary, accent, surface, selection, muted), or
    // mix(a,b,t), either with an alpha after a slash ("surface/0.7").
    function themeColour(expression) {
        const m = /^\s*(?:mix\((\w+),\s*(\w+),\s*([\d.]+)\)|(\w+))\s*(?:\/\s*([\d.]+))?\s*$/.exec(expression || "");
        if (!m) return expression;
        const colour = m[4] ? theme[m[4]] : (theme[m[1]] && theme[m[2]] ? mix(theme[m[1]], theme[m[2]], Number(m[3])) : null);
        if (!colour) return expression;
        if (m[5] === undefined) return hex(colour);
        return "rgba(" + [colour.r, colour.g, colour.b].map(v => Math.round(v * 255)).join(",") + "," + m[5] + ")";
    }
    readonly property string siteTheme: themedSite ? siteCss(themedSite) : ""
    // A web app's own start-up script, run before its pages' scripts: a site
    // theme file's "script", or one built in. Snapchat's web client shows a
    // signed-out visitor its download page first and sends one back within
    // 10 s straight to sign-in, so its web app says it has just been there.
    readonly property string siteScript: {
        if (!appMode) return "";
        if (siteOf(host.startUrl) === "snapchat.com")
            return "try { if (location.hostname === 'www.snapchat.com')"
                + " localStorage.setItem('lastPreauthRedirect', String(Date.now())); } catch (e) {}";
        return customTheme && typeof customTheme.script === "string" ? customTheme.script : "";
    }
    onSiteThemeChanged: {
        for (let i = 0; i < tabs.count; i++) {
            const v = views.itemAt(i);
            if (v && siteTheme) v.runJavaScript(themeScript(siteTheme));
        }
    }
    function hex(c) { return Qt.rgba(c.r, c.g, c.b, 1).toString(); }
    // "H S% L%", the form X's variables take inside hsl().
    function hsl(c) {
        return Math.round(Math.max(0, c.hslHue) * 360) + " " + Math.round(c.hslSaturation * 100) + "% "
            + Math.round(c.hslLightness * 100) + "%";
    }
    // "r, g, b", the form Telegram's -rgb variables take inside rgba().
    function rgbTriple(c) { return [c.r, c.g, c.b].map(v => Math.round(v * 255)).join(", "); }
    function rgbaOf(c, a) { return "rgba(" + rgbTriple(c) + ", " + a + ")"; }
    function mix(a, b, t) { return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1); }
    // Discord's colour ramps (October 6: lightness of each step in its
    // darker theme). Its themes take their colours from these steps, so the
    // web app re-colours the steps: grey ramps by where each step's lightness
    // lies between Discord's background and text steps (darker theme
    // neutral-92 and -5 for a dark palette, light theme neutral-2 and -72 for
    // a light one), its brand ramps around the accent.
    readonly property var discordRamps: ({
        neutral: [100,98.4,97.1,95.5,94.1,92.5,91.4,89.8,88.4,86.9,85.5,83.9,82.7,81.2,79.8,78.2,77.1,75.5,74.1,72.5,71.4,70,68.4,67.3,65.7,64.3,63.1,61.6,60.4,59,57.6,56.3,55.1,53.5,52.4,51,49.6,48.2,47.1,45.9,44.3,43.1,42,40.6,39.4,38.2,36.9,35.5,34.3,33.1,32.4,31.8,31.2,30.6,29.8,29.2,28.4,28,27.3,26.5,25.9,25.3,24.7,23.9,23.5,22.7,22.2,21.6,21,20.4,19.6,19.2,18.4,18,17.3,16.7,16.3,15.5,15.1,14.3,13.7,13.3,12.5,12.2,11.4,11,10.4,9.8,9.2,8.8,8,7.5,6.9,5.9,5.1,4.3,3.1,2.2,1,0],
        plum: [98.4,95.5,92.5,89.8,86.9,83.9,79.8,74.1,67.3,60.4,53.5,44.3,38.2,33.1,28,23.9,21,18.4,16.3,14.3,12.2,11,9.2,8,6.9,5.1,3.1],
        blurple: [95.1,94.5,93.7,92.9,92.4,91.6,91,90.2,89.6,89,88.2,87.6,86.9,86.3,85.7,85.1,84.5,83.7,83.1,82.5,82,81.4,80.8,80.2,79.6,79,78.2,77.8,77.1,76.5,75.7,74.9,74.5,73.7,73.3,72.5,72,71.4,70.8,70.2,69.4,69,68.4,67.8,67.3,66.9,66.3,65.9,65.1,64.7,63.1,61.6,60,58.6,57.1,55.7,54.3,52.7,51.4,50,48.6,47.3,46.1,44.7,43.3,42.2,40.8,39.6,38.2,37.1,35.7,34.5,33.5,32.4,31.2,29.8,28.6,27.5,26.3,25.3,24.1,22.9,21.8,21,19.8,18.8,17.6,16.7,15.7,14.5,13.7,12.5,11.6,10.6,9.2,8,6.7,4.9,2.5,0],
        primary: [[100,98.4],[130,95.5],[160,92.5],[200,89.8],[230,86.9],[260,83.9],[300,79.8],[330,74.1],[345,68.4],[360,61.6],[400,53.5],[430,44.3],[460,38.2],[500,33.1],[530,27.3],[560,23.9],[600,21],[630,18.4],[645,17.3],[660,15.1],[700,12.5],[730,11],[760,9.2],[800,7.5],[830,5.1],[860,2.2],[900,1]],
        brand: [[100,95.1],[130,95.1],[160,95.1],[200,92.9],[230,91],[260,89],[300,85.7],[330,82],[345,79],[360,77.1],[400,71.4],[430,69.4],[460,67.3],[500,64.7],[530,58.6],[560,51.4],[600,43.3],[630,37.1],[660,32.4],[700,24.1],[730,22.9],[760,21],[800,18.8],[830,13.7],[860,8],[900,2.5]]
    })
    function siteCss(site) {
        const bg = theme.background, fg = theme.foreground, dim = theme.secondary;
        const surface = theme.surface, sel = theme.selection, line = theme.muted, accent = theme.accent;
        const vars = {};
        if (site === "x.com") {
            // Its shadcn layer (HSL parts) and its own --x layer (colours).
            Object.assign(vars, {
                "--background": hsl(bg), "--color-background": hsl(bg), "--popover": hsl(surface),
                "--foreground": hsl(fg), "--color-text": hsl(fg), "--primary": hsl(fg),
                "--card-foreground": hsl(fg), "--popover-foreground": hsl(fg), "--secondary-foreground": hsl(fg),
                "--secondary": hsl(sel), "--muted": hsl(sel), "--border": hsl(line), "--input": hsl(line),
                "--muted-foreground": hsl(dim), "--color-blue-500": hsl(accent), "--ring": hsl(accent),
                "--color-gray-0": hsl(surface),
                "--x-bg-primary": hex(bg), "--x-bg-secondary": hex(surface), "--x-bg-tertiary": hex(sel),
                "--x-bg-modal": hex(surface), "--x-bg-sheets": hex(surface), "--x-bg-inputs": hex(sel),
                "--x-fg-primary": hex(fg), "--x-fg-secondary": hex(dim), "--x-fg-tertiary": hex(mix(bg, fg, 0.45)),
                "--x-fg-inverted": hex(bg), "--x-fg-on-color": hex(bg), "--x-fg-brand": hex(accent),
                "--x-border-normal": hex(line), "--x-border-hover": hex(mix(line, fg, 0.15)),
                "--x-border-active": hex(mix(line, fg, 0.3)), "--x-blue-500": hex(accent), "--x-ring": hex(accent)
            });
            // Its gray scale runs from the background to the text.
            [[50, 0.07], [100, 0.1], [200, 0.13], [300, 0.17], [400, 0.22], [500, 0.28], [600, 0.33],
             [700, 0.4], [800, 0.55], [900, 0.7], [1000, 0.85], [1100, 1]]
                .forEach(step => vars["--color-gray-" + step[0]] = hsl(mix(bg, fg, step[1])));
        } else if (site === "grok.com") {
            Object.assign(vars, {
                "--surface-inset": hex(bg), "--surface-base": hex(bg), "--surface-l1": hex(surface),
                "--surface-l1-hover": hex(sel), "--surface-l2": hex(sel), "--surface-l2-active": hex(mix(sel, fg, 0.08)),
                "--surface-l3": hex(mix(sel, fg, 0.08)), "--surface-l4": hex(mix(sel, fg, 0.08)),
                "--surface-l4-hover": hex(mix(sel, fg, 0.12)), "--surface-elevated": hex(surface),
                "--surface-invert": hex(fg), "--fg-primary": hex(fg), "--fg-secondary": hex(dim),
                "--fg-tertiary": hex(mix(bg, fg, 0.55)), "--fg-quaternary": hex(mix(bg, fg, 0.42)),
                "--fg-invert": hex(bg), "--fg-link": hex(accent), "--fg-link-hover": hex(mix(accent, fg, 0.2)),
                "--border-l1": hex(line), "--border-l2": hex(mix(line, fg, 0.12)), "--border-l3": hex(mix(line, fg, 0.25)),
                "--ring": hex(accent), "--button-filled": hex(accent),
                "--button-filled-hover": hex(mix(accent, fg, 0.15)), "--button-filled-active": hex(mix(accent, bg, 0.15))
            });
        } else if (site === "discord.com") {
            const light = c => 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
            const dark = light(bg) < light(fg);
            const bgRef = dark ? 7.5 : 98.4, fgRef = dark ? 94.1 : 19.2, brandRef = 64.7;
            const high = dark ? fg : bg, low = dark ? bg : fg;
            const clamp = v => Math.max(0, Math.min(1, v));
            const ramp = (a, b, t) => Qt.rgba(clamp(a.r + (b.r - a.r) * t), clamp(a.g + (b.g - a.g) * t),
                                              clamp(a.b + (b.b - a.b) * t), 1);
            const grey = l => hsl(ramp(bg, fg, (l - bgRef) / (fgRef - bgRef)));
            const brand = l => hsl(l >= brandRef ? ramp(accent, high, (l - brandRef) / (100 - brandRef))
                                                 : ramp(accent, low, (brandRef - l) / brandRef));
            discordRamps.neutral.forEach((l, i) => vars["--neutral-" + (i + 1) + "-hsl"] = grey(l));
            discordRamps.plum.forEach((l, i) => vars["--plum-" + i + "-hsl"] = grey(l));
            discordRamps.primary.forEach(s => vars["--primary-" + s[0] + "-hsl"] = grey(s[1]));
            discordRamps.blurple.forEach((l, i) => vars["--blurple-" + (i + 1) + "-hsl"] = brand(l));
            discordRamps.brand.forEach(s => vars["--brand-" + s[0] + "-hsl"] = brand(s[1]));
            // Hover and selection overlays: a grey at varying opacity.
            [1, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96]
                .forEach(n => { vars["--opacity-" + n + "-hsl"] = hsl(dim); vars["--opacity-white-" + n + "-hsl"] = hsl(high); });
            return ":root,html,.theme-dark,.theme-darker,.theme-midnight,.theme-light{"
                + Object.keys(vars).map(name => name + ":" + vars[name] + " !important").join(";") + "}";
        } else if (site === "web.telegram.org") {
            // Telegram Web K: day colours set on the page, night ones on .night.
            const out = mix(surface, accent, 0.3), outText = mix(accent, fg, 0.4), field = mix(bg, surface, 0.6);
            Object.assign(vars, {
                "--body-background-color": hex(bg), "--background-color": hex(bg), "--background-color-rgb": rgbTriple(bg),
                "--surface-color": hex(surface), "--surface-color-rgb": rgbTriple(surface),
                "--primary-color": hex(accent), "--primary-color-rgb": rgbTriple(accent),
                "--light-primary-color": rgbaOf(accent, 0.08), "--light-filled-primary-color": hex(mix(surface, accent, 0.15)),
                "--dark-primary-color": hex(mix(accent, bg, 0.15)), "--peer-color": hex(accent), "--peer-color-rgb": rgbTriple(accent),
                "--saved-color": hex(accent), "--light-filled-saved-color": hex(mix(accent, fg, 0.35)),
                "--message-highlighting-color-rgb": rgbTriple(accent), "--link-color": hex(accent),
                "--message-background-color": hex(surface), "--light-message-background-color": rgbaOf(surface, 0.08),
                "--light-filled-message-background-color": hex(surface), "--dark-message-background-color": hex(mix(surface, bg, 0.5)),
                "--message-out-background-color": hex(out), "--message-out-background-color-rgb": rgbTriple(out),
                "--light-message-out-background-color": hex(out), "--light-filled-message-out-background-color": hex(out),
                "--dark-message-out-background-color": hex(mix(out, bg, 0.3)),
                "--message-out-primary-color": hex(outText), "--message-out-primary-color-rgb": rgbTriple(outText),
                "--light-filled-message-out-primary-color": hex(mix(out, accent, 0.5)),
                "--primary-text-color": hex(fg), "--primary-text-color-rgb": rgbTriple(fg),
                "--secondary-text-color": hex(dim), "--secondary-text-color-rgb": rgbTriple(dim),
                "--light-secondary-text-color": rgbaOf(dim, 0.08), "--light-filled-secondary-text-color": hex(mix(bg, dim, 0.12)),
                "--secondary-color": hex(dim), "--border-color": hex(line), "--chatlist-pinned-color": hex(dim),
                "--input-search-background-color": hex(field), "--input-search-border-color": hex(line),
                "--old-input-background-color": hex(field), "--input-placeholder-color": hex(dim),
                "--input-message-placeholder-color": hex(dim), "--limit-line-empty-background": hex(sel),
                "--scrollbar-color": rgbaOf(fg, 0.2), "--original-ripple-color": rgbaOf(fg, 0.08),
                "--skeleton-color": rgbaOf(fg, 0.12), "--badge-text-color": hex(bg)
            });
            return ":root,html,html.night,.night{"
                + Object.keys(vars).map(name => name + ":" + vars[name] + " !important").join(";") + "}";
        } else if (site === "custom" && customTheme) {
            const custom = customTheme.vars || {};
            for (const name in custom) vars[name] = themeColour(custom[name]);
            const selectors = customTheme.selectors || ":root,html,body";
            const extra = (customTheme.css || "").replace(/\{\{(\w+(?:\([^)]*\))?(?:\/[\d.]+)?)\}\}/g, (m, e) => themeColour(e));
            return selectors + "{" + Object.keys(vars).map(name => name + ":" + vars[name] + " !important").join(";") + "}" + extra;
        } else {
            return "";
        }
        // Grok also sets its variables on body.
        const body = Object.keys(vars).map(name => name + ":" + vars[name] + " !important").join(";");
        // X sets the page's own background and text colour on body inline.
        const page = site === "x.com" ? "html,body{background-color:" + hex(bg) + "!important;color:" + hex(fg) + "!important}"
            + inlinePaletteCss(sitePalette(site)) : "";
        return ":root,:root[data-theme],[data-theme],html,body,.dark,.light,.dark-theme,.light-theme{" + body + "}" + page;
    }
    // X paints its signed-in pages from atomic classes in its own colours, not
    // the variables above. Its palette for the theme's scheme (Lights out and
    // Dim when dark, Default when light) maps by RGB to the theme's colours,
    // keeping each colour's alpha; themeScript rewrites its rules with it.
    function sitePalette(site) {
        if (site === "custom" && customTheme && customTheme.palette) {
            // A site theme file's palette: "r,g,b" to a theme colour (themeColour).
            const rgbOf = c => [c.r, c.g, c.b].map(v => Math.round(v * 255)).join(",");
            const own = { "__bg": rgbOf(theme.background) };
            for (const key in customTheme.palette) {
                const value = themeColour(customTheme.palette[key]);
                if (/^#[0-9a-f]{6}$/i.test(value)) own[key.replace(/\s+/g, "")] = rgbOf(Qt.color(value));
            }
            return own;
        }
        if (site !== "x.com") return null;
        const bg = theme.background, fg = theme.foreground, dim = theme.secondary;
        const surface = theme.surface, sel = theme.selection, line = theme.muted, accent = theme.accent;
        const from = themeDark ? {
            "0,0,0": bg, "22,24,28": surface, "24,24,24": surface, "32,35,39": sel, "47,51,54": line, "51,54,57": line,
            "62,65,68": line, "231,233,234": fg, "113,118,123": dim, "29,155,240": accent,
            "26,140,216": mix(accent, bg, 0.12), "239,243,244": fg, "15,20,25": bg,
            "21,32,43": bg, "30,39,50": surface, "39,51,64": sel, "56,68,77": line, "247,249,249": fg, "139,152,165": dim
        } : {
            "255,255,255": bg, "247,249,249": surface, "239,243,244": line, "15,20,25": fg, "83,100,113": dim,
            "29,155,240": accent, "26,140,216": mix(accent, fg, 0.12), "231,233,234": sel, "207,217,222": line
        };
        const rgb = c => [c.r, c.g, c.b].map(v => Math.round(v * 255)).join(",");
        const palette = { "__bg": rgb(bg) };
        for (const key in from) palette[key] = rgb(from[key]);
        return palette;
    }
    // X also writes most of its colours as inline styles (React's style
    // attribute), which beat any rule but an important one. Each palette
    // colour gets important rules for the inline styles that carry it, matched
    // on the attribute's text ("color: rgb(r, g, b)"; " color" and a leading
    // "color" so background-color is not taken for color). Text, background
    // and border colours are all X writes inline (October 5: 665 of 2807
    // elements). Its translucent backgrounds (the 65% top bar, 75% sheets,
    // 10-20% hover tints) keep their alpha; a transparent one (alpha 0) is
    // left alone.
    function inlinePaletteCss(palette) {
        if (!palette) return "";
        const props = [["color", [" color: ", "color: "]], ["background-color", ["background-color: "]],
                       ["border-top-color", ["border-top-color: "]], ["border-bottom-color", ["border-bottom-color: "]],
                       ["border-left-color", ["border-left-color: "]], ["border-right-color", ["border-right-color: "]]];
        const translucent = ["0,0,0", "15,20,25", "21,32,43", "29,155,240", "231,233,234", "239,243,244", "255,255,255"];
        const alphas = ["0.1", "0.2", "0.65", "0.75", "0.85"];
        const select = (prop, prefix, i, value) =>
            "[style" + (prop === "color" && i === 1 ? "^" : "*") + "=\"" + prefix + value + "\"]";
        let css = "[style*=\"background-color: black\"]{background-color:rgb(" + palette.__bg + ")!important}";
        for (const key in palette) {
            if (key === "__bg") continue;
            const rgb = key.split(",").join(", ");
            for (const [prop, prefixes] of props)
                css += prefixes.map((prefix, i) => select(prop, prefix, i, "rgb(" + rgb + ")")).join(",")
                    + "{" + prop + ":rgb(" + palette[key] + ")!important}";
            if (translucent.indexOf(key) < 0) continue;
            for (const alpha of alphas)
                css += select("background-color", "background-color: ", 0, "rgba(" + rgb + ", " + alpha + ")")
                    + "{background-color:rgba(" + palette[key] + "," + alpha + ")!important}";
        }
        return css;
    }
    // The page side of sitePalette: X's colour rules, in X's own order, are
    // copied into a sheet that comes after X's, with the colours in the
    // palette replaced and the rest kept. Keeping every colour rule and its
    // order keeps X's cascade: a later class still beats an earlier one on
    // the same element (X's text starts from a base colour that a later
    // class replaces). Scanned again whenever X adds rules. A rule with a
    // backdrop blur (X's top bar) gets the theme's background, opaque, and no
    // blur: the blur over scrolling posts flickered, showing them through.
    readonly property string paletteScan: "
            var over = window.__omarchyOver || (window.__omarchyOver = new CSSStyleSheet());
            window.__omarchyPalette = palette;
            if (document.adoptedStyleSheets.indexOf(over) < 0)
                document.adoptedStyleSheets = document.adoptedStyleSheets.concat([over]);
            if (window.__omarchyRescan) { window.__omarchyRescan(); return; }
            var props = ['color', 'background-color', 'border-top-color', 'border-right-color',
                         'border-bottom-color', 'border-left-color', 'fill', 'stroke', 'outline-color',
                         '--tw-gradient-from', '--tw-gradient-via', '--tw-gradient-to', '--tw-gradient-stops'];
            // Copied only where a colour changed: they can hold url()s,
            // which a copy would resolve against the page instead.
            var changedOnly = ['background-image', 'box-shadow'];
            var counted = -1;
            // Every colour in a value whose RGB is in the palette, in any of
            // CSS's forms: rgb(r, g, b), rgba(r, g, b, a), Tailwind's
            // rgb(r g b / var(--tw-bg-opacity, 1)) minified or not, and
            // #rrggbb or #rrggbbaa (gradient stops); the alpha is kept.
            function convert(value) {
                var palette = window.__omarchyPalette;
                return value.replace(/(rgba?\\(\\s*)(\\d+)(\\s*,\\s*|\\s+)(\\d+)(\\s*,\\s*|\\s+)(\\d+)/g,
                    function (all, open, r, a, g, b, bl) {
                        var to = palette[r + ',' + g + ',' + bl];
                        if (!to) return all;
                        to = to.split(',');
                        return open + to[0] + a + to[1] + b + to[2];
                    }).replace(/#([0-9a-fA-F]{6})([0-9a-fA-F]{2})?\\b/g, function (all, hex, alpha) {
                        var to = palette[[0, 2, 4].map(function (i) { return parseInt(hex.substr(i, 2), 16); }).join(',')];
                        if (!to) return all;
                        return '#' + to.split(',').map(function (v) { return ('0' + Number(v).toString(16)).slice(-2); }).join('')
                            + (alpha || '');
                    });
            }
            function walk(list, out) {
                for (var i = 0; i < list.length; i++) {
                    var rule = list[i];
                    if (rule.cssRules && rule.conditionText !== undefined && !rule.selectorText) {
                        var inner = [];
                        walk(rule.cssRules, inner);
                        if (inner.length) out.push('@media ' + rule.conditionText + '{' + inner.join('') + '}');
                        continue;
                    }
                    if (!rule.style || !rule.selectorText) continue;
                    var style = rule.style, sel = rule.selectorText, body = '', mapped = false;
                    for (var p = 0; p < props.length; p++) {
                        var value = style.getPropertyValue(props[p]);
                        if (!value) continue;
                        var colour = convert(value);
                        mapped = mapped || colour !== value;
                        body += props[p] + ':' + colour + ';';
                    }
                    for (p = 0; p < changedOnly.length; p++) {
                        value = style.getPropertyValue(changedOnly[p]);
                        colour = value && convert(value);
                        if (colour && colour !== value) body += changedOnly[p] + ':' + colour + ';';
                    }
                    if (body) out.push(sel + '{' + body + '}');
                    if (style.backdropFilter && style.backdropFilter !== 'none')
                        out.push(':is(' + sel + '):not(#__omarchy){backdrop-filter:none!important;-webkit-backdrop-filter:none!important;'
                            + 'background-color:rgb(' + window.__omarchyPalette.__bg + ')!important}');
                }
            }
            function scan() {
                var sheets = document.styleSheets, total = 0, lists = [];
                for (var i = 0; i < sheets.length; i++) {
                    try { lists.push(sheets[i].cssRules); total += sheets[i].cssRules.length; } catch (e) {}
                }
                if (total === counted) return;
                counted = total;
                var out = [];
                for (i = 0; i < lists.length; i++) walk(lists[i], out);
                over.replaceSync(out.join(''));
                window.__omarchyRuleCount = out.length;
            }
            window.__omarchyRescan = function () { counted = -1; scan(); };
            document.addEventListener('DOMContentLoaded', scan);
            addEventListener('load', scan);
            setInterval(scan, 1500);
            scan();"
    // Pages here cannot be told the theme's scheme through Qt: without a
    // desktop portal the phone's Qt ignores a colour scheme request, so
    // prefers-color-scheme stays light. Both sites choose their dark or light
    // look in script, so the script's matchMedia answers for the theme.
    readonly property bool themeDark: theme.background.hslLightness < 0.5
    function themeScript(css) {
        const palette = sitePalette(themedSite);
        return (palette ? "(function (palette) {" + paletteScan + "})(" + JSON.stringify(palette) + ");" : "")
            + "(function (css, dark) {"
            + " var sheet = window.__omarchyTheme || (window.__omarchyTheme = new CSSStyleSheet());"
            + " sheet.replaceSync(css);"
            + " window.__omarchyDark = dark;"
            + " function adopt() { if (document.adoptedStyleSheets.indexOf(sheet) < 0)"
            + " document.adoptedStyleSheets = document.adoptedStyleSheets.concat([sheet]); }"
            + " adopt(); document.addEventListener('DOMContentLoaded', adopt); addEventListener('load', adopt);"
            + " if (window.__omarchyMedia) return; window.__omarchyMedia = true;"
            + " var media = window.matchMedia.bind(window);"
            + " window.matchMedia = function (query) {"
            + "  var list = media(query), scheme = /prefers-color-scheme:\\s*(dark|light)/.exec(String(query));"
            + "  if (!scheme) return list;"
            + "  return new Proxy(list, { get: function (target, name) {"
            + "   if (name === 'matches') return (scheme[1] === 'dark') === window.__omarchyDark;"
            + "   var value = target[name]; return typeof value === 'function' ? value.bind(target) : value; } });"
            + " };"
            + "})(" + JSON.stringify(css) + ", " + (themeDark ? "true" : "false") + ");";
    }
    // The bar's height where it takes room (not in a web app or full screen).
    readonly property real barSpace: bar.visible ? bar.height : 0
    readonly property string home: "https://duckduckgo.com/"
    readonly property string search: "https://duckduckgo.com/?q="
    property int current: 0
    onCurrentChanged: if (finding) stopFinding()
    property bool overview: false
    property bool menuOpen: false
    property bool editing: false
    property bool fullscreen: false
    property var permission: null
    // A page's file input (<input type=file>, a "take a picture" button):
    // the picker sheet offers the camera and the newest photos.
    property var fileRequest: null
    property var recent: []
    // The camera's capture in progress: its target file, until it appears
    // or the user comes back without it.
    property string capturePath: ""
    function openPicker(request) {
        if (fileRequest) fileRequest.dialogReject();
        fileRequest = request;
        recent = host.recentPhotos(6);
    }
    function wantsImages(request) {
        const types = request ? request.acceptedMimeTypes : [];
        return !types || types.length === 0 || types.some(t => /^(image\/|\*\/\*|\.(jpe?g|png|heic|webp)$)/i.test(t));
    }
    function pickFile(path) {
        const request = fileRequest;
        fileRequest = null;
        if (request) request.dialogAccept(["file://" + path]);
    }
    function cancelPicker() {
        const request = fileRequest;
        fileRequest = null;
        capturePath = "";
        if (request) request.dialogReject();
    }
    function takePhoto() {
        const path = host.startCapture();
        if (!path) { notice = "The camera could not be opened"; cancelPicker(); return; }
        capturePath = path;
    }
    Timer {
        interval: 400
        repeat: true
        running: win.capturePath !== ""
        onTriggered: if (host.fileReady(win.capturePath)) { const path = win.capturePath; win.capturePath = ""; win.pickFile(path); }
    }
    // Back from the camera without a photo (this window in front for 1.5 s
    // on end: it can take the focus for a moment while the shell parks it
    // for the camera): the page gets none.
    Timer { id: captureGone; interval: 1500; onTriggered: if (win.capturePath !== "" && !host.fileReady(win.capturePath)) win.cancelPicker() }
    onActiveChanged: {
        if (active && capturePath !== "") captureGone.restart();
        else captureGone.stop();
    }
    property string notice: ""
    // Overscroll: the page edge the finger is pulling past (1 the top, -1 the
    // bottom, 0 none) as the page script reports it, and where the finger
    // was then. From there the distance comes from the finger itself, each
    // frame, rather than from the page's messages, which arrive unevenly.
    property int pullEdge: 0
    property real pullAnchor: 0
    property real lastPull: 0
    // The page has its own pull to refresh (overscroll-behavior none or
    // contain on its root, as X's): it stretches, but letting go does not
    // also reload it.
    property bool pullOwned: false
    // Pixels past the top (positive) or the bottom (negative).
    readonly property real pullPx: pullEdge !== 0 && finger.active
        ? Math.max(0, (finger.point.position.y - pullAnchor) * pullEdge) * pullEdge : 0
    // Page events go to browser-events.log in the state directory, for
    // reloads nobody asked for (a page's own, or after a renderer exit).
    function navigationName(type) {
        switch (type) {
        case WebEngineNavigationRequest.LinkClickedNavigation: return "link";
        case WebEngineNavigationRequest.TypedNavigation: return "typed";
        case WebEngineNavigationRequest.FormSubmittedNavigation: return "form";
        case WebEngineNavigationRequest.BackForwardNavigation: return "back-forward";
        case WebEngineNavigationRequest.ReloadNavigation: return "reload";
        case WebEngineNavigationRequest.RedirectNavigation: return "redirect";
        default: return "other";
        }
    }
    function startPull(edge, owned) {
        if (!finger.active) return;
        pullEdge = edge;
        pullOwned = owned;
        pullAnchor = finger.point.position.y;
    }
    function fingerLifted() {
        if (pullEdge === 1 && !pullOwned && lastPull >= 160 && view) {
            host.logEvent("pull to refresh " + view.url);
            view.reload();
        }
        pullEdge = 0;
        lastPull = 0;
    }
    // Pull to refresh: 0 to 1 over the first 160 pixels past the top; at 1,
    // letting go reloads.
    readonly property real pull: Math.max(0, Math.min(1, pullPx / 160))
    // The page follows an overscroll with resistance, as on iOS (the stretch
    // approaches the view's height), and springs back on release.
    property real stretch: 0
    function rubber(d) {
        const h = Math.max(1, pageArea.height);
        const s = (1 - 1 / (Math.abs(d) * 0.55 / h + 1)) * h;
        return d < 0 ? -s : s;
    }
    onPullPxChanged: {
        if (pullPx !== 0) {
            lastPull = pullPx;
            springBack.stop();
            stretch = rubber(pullPx);
        } else if (stretch !== 0) {
            springBack.restart();
        }
    }
    NumberAnimation { id: springBack; target: win; property: "stretch"; to: 0; duration: 380; easing.type: Easing.OutCubic }
    // Find in page: the bar holds the search instead of the address.
    property bool finding: false
    property int matches: 0
    property int match: 0
    // Add to home screen: the sheet is open.
    property bool adding: false
    readonly property var view: tabs.count > current ? views.itemAt(current) : null
    // Bookmarks ({url, title}) and history ({url, title, last, count}, the
    // latest first), in browser-bookmarks.json and browser-history.json.
    property var bookmarks: []
    property var history: []
    readonly property int historyLimit: 1000

    // What was typed: a web address as it is, a domain-like word with https,
    // anything else searched.
    function target(text) {
        const t = text.trim();
        if (/^[a-z][a-z0-9+.-]*:\/\//i.test(t) || t.startsWith("about:")) return t;
        if (/^[^\s/]+\.[a-z]{2,}(:\d+)?(\/.*)?$/i.test(t) || /^localhost(:\d+)?(\/.*)?$/.test(t)) return "https://" + t;
        return search + encodeURIComponent(t);
    }
    function hostOf(url) {
        const m = /^[a-z]+:\/\/([^/?#]+)/i.exec(url || "");
        return m ? m[1].replace(/^www\./, "") : (url || "");
    }
    // A web app's site: the host's last two labels, three under a country's
    // own second level (example.co.uk). accounts.snapchat.com is Snapchat's.
    function siteOf(url) {
        const labels = hostOf(url).replace(/:\d+$/, "").split(".");
        const country = labels.length > 2 && /^[a-z]{2}$/.test(labels[labels.length - 1])
                        && /^(co|com|org|net|gov|ac|edu)$/.test(labels[labels.length - 2]);
        return labels.slice(country ? -3 : -2).join(".");
    }
    // The tab overview shows each tab as it last looked: the current page is
    // pictured when it finishes loading, and before it is left or the
    // overview opens. A hidden page is not drawn, so it cannot be pictured.
    function captureCurrent() { if (view) view.capture(); }
    function showOverview() { captureCurrent(); overview = true; }
    Timer { id: shotLater; interval: 900; onTriggered: win.captureCurrent() }
    function newTab(url, foreground) {
        tabs.append({start: url || home});
        if (foreground !== false) { captureCurrent(); current = tabs.count - 1; }
        overview = false;
        saveTabs();
    }
    function closeTab(index) {
        if (tabs.count === 1) { views.itemAt(0).url = home; return; }
        tabs.remove(index);
        current = Math.min(current, tabs.count - 1);
        saveTabs();
    }
    function saveTabs() {
        if (appMode) return;
        const list = [];
        for (let i = 0; i < tabs.count; i++) {
            const v = views.itemAt(i);
            list.push(v && v.url ? v.url.toString() : tabs.get(i).start);
        }
        host.writeState("browser-tabs.json", JSON.stringify({tabs: list, current: current}));
    }
    function savedList(name, key) {
        try { const list = JSON.parse(host.readState(name))[key]; return Array.isArray(list) ? list : []; }
        catch (e) { return []; }
    }
    function isBookmarked(url) { return bookmarks.some(b => b.url === url); }
    function toggleBookmark() {
        if (!view) return;
        const url = view.url.toString();
        bookmarks = isBookmarked(url) ? bookmarks.filter(b => b.url !== url)
                                      : [{url: url, title: view.title || hostOf(url)}].concat(bookmarks);
        host.writeState("browser-bookmarks.json", JSON.stringify({bookmarks: bookmarks}));
        notice = isBookmarked(url) ? "Bookmarked" : "Bookmark removed";
    }
    // Web pages only, once each load has finished; the title follows later.
    function remember(url, title) {
        if (appMode || !/^https?:/i.test(url)) return;
        const old = history.find(h => h.url === url);
        history = [{url: url, title: title || (old ? old.title : ""), last: Date.now(), count: (old ? old.count : 0) + 1}]
            .concat(history.filter(h => h.url !== url)).slice(0, historyLimit);
        historySave.restart();
    }
    function retitle(url, title) {
        if (appMode) return;
        const entry = history.find(h => h.url === url);
        if (!entry || !title || entry.title === title) return;
        entry.title = title;
        historySave.restart();
    }
    function clearHistory() {
        history = [];
        host.writeState("browser-history.json", JSON.stringify({history: []}));
        notice = "History cleared";
    }
    // What to offer while the address is edited: bookmarks then recent pages
    // before anything is typed, else the matches, bookmarks first, then the
    // pages visited most.
    function suggestions(text) {
        const t = text.trim().toLowerCase();
        const seen = {}, out = [];
        const add = (entry, marked) => {
            if (seen[entry.url]) return;
            seen[entry.url] = true;
            out.push({url: entry.url, title: entry.title || hostOf(entry.url), bookmark: marked});
        };
        if (!t) {
            bookmarks.forEach(b => add(b, true));
            history.slice(0, 12).forEach(h => add(h, isBookmarked(h.url)));
            return out.slice(0, 24);
        }
        const match = e => e.url.toLowerCase().includes(t) || (e.title || "").toLowerCase().includes(t);
        bookmarks.filter(match).forEach(b => add(b, true));
        history.filter(match).sort((a, b) => b.count - a.count || b.last - a.last).forEach(h => add(h, false));
        return out.slice(0, 12);
    }
    function open(url) {
        if (view) view.url = url;
        address.focus = false;
    }
    Timer {
        id: historySave
        interval: 2000
        onTriggered: host.writeState("browser-history.json", JSON.stringify({history: win.history}))
    }
    function stopFinding() {
        finding = false;
        matches = match = 0;
        findField.text = "";
        if (view) view.findText("");
    }
    function addToHome() {
        const url = view ? view.url.toString() : "";
        const name = appName.text;
        appIcon.grabToImage(result => {
            const path = host.webAppIconPath(url);
            if (path) result.saveToFile(path);
            notice = host.addWebApp(name, url) ? name + " is on the home screen" : "This page can't be added";
        });
        adding = false;
        appName.focus = false;
    }
    function goBack() {
        if (permission) { permission.deny(); permission = null; return; }
        if (adding) { adding = false; return; }
        if (menuOpen) { menuOpen = false; return; }
        if (finding) { stopFinding(); return; }
        if (editing) { address.focus = false; return; }
        if (overview) { overview = false; return; }
        if (fullscreen && view) { view.triggerWebAction(WebEngineView.ExitFullScreen); return; }
        if (view && view.canGoBack) view.goBack();
        // A web app's popup tab with nowhere to go back to: back to the app.
        else if (appMode && tabs.count > 1) closeTab(current);
    }

    ListModel { id: tabs }
    Component.onCompleted: {
        if (appMode) {
            title = host.startUrl;
            tabs.append({start: host.startUrl});
            return;
        }
        bookmarks = savedList("browser-bookmarks.json", "bookmarks");
        history = savedList("browser-history.json", "history");
        let restored = [];
        try { const saved = JSON.parse(host.readState("browser-tabs.json")); restored = saved.tabs || []; current = saved.current || 0; }
        catch (e) {}
        for (const url of restored) tabs.append({start: url});
        if (host.startUrl) newTab(target(host.startUrl));
        else if (!tabs.count) newTab(home);
        current = Math.max(0, Math.min(current, tabs.count - 1));
    }
    Connections {
        target: host
        function onBackRequested() { win.goBack(); }
        // Another start of the browser (or of this web app) and its address.
        function onOpenRequested(url) {
            if (url && win.appMode) { if (win.view) win.view.url = url; }
            else if (url) win.newTab(win.target(url));
            win.visible = true;
            host.raise();
        }
    }

    // The keyboard for the page's text fields: a page script reports focus
    // through the console (no channel into the page is needed).
    readonly property string keyboardScript: "
        (function () {
            if (window.__omarchyKeyboard) return; window.__omarchyKeyboard = true;
            var skip = ['button','checkbox','radio','submit','reset','file','image','range','color','hidden'];
            function typed(e) {
                if (!e) return false;
                if (e.isContentEditable) return true;
                if (e.tagName === 'TEXTAREA') return true;
                return e.tagName === 'INPUT' && skip.indexOf((e.type || 'text').toLowerCase()) < 0;
            }
            document.addEventListener('focusin', function (ev) { if (typed(ev.target)) console.info('omarchy-keyboard:show'); }, true);
            document.addEventListener('focusout', function (ev) { if (typed(ev.target)) console.info('omarchy-keyboard:hide'); }, true);
        })();"
    Timer { id: keyboardHide; interval: 200; onTriggered: host.keyboard("hide") }
    // A page's endless animation frames at a calm rate. A page that asks for
    // a frame every frame on its own (X's Spaces avatars: two 57 px canvases
    // redrawn at 120 Hz) has the whole screen redrawn each time, in Chromium,
    // Qt and Hyprland: about 2.5 cores and 1-1.2 A on October 7, against
    // 0.24 A for X standing still. Each frame costs about 11 mA. Once such a
    // loop has run for a second with no touch, key or scroll, its frames
    // come about 20 times a second, and 10 after five seconds (0.5 A and
    // less); a touch brings full rate back at once, and short animations
    // (under a second) and full-screen pages keep it, and while a video
    // plays on the page, no loop drops below 30 (a player may draw its
    // video into a canvas). Only requestAnimationFrame is paced: CSS
    // animations, video and scrolling are Chromium's own.
    readonly property string framesScript: "
        (function () {
            if (window.__omarchyFrames) return; window.__omarchyFrames = true;
            var raf = window.requestAnimationFrame, caf = window.cancelAnimationFrame;
            var CALM_AFTER = 1000, CALM_INTERVAL = 50, CALMER_AFTER = 5000, CALMER_INTERVAL = 100, VIDEO_INTERVAL = 33, GAP = 120;
            var input = -1e9, start = 0, last = -1e9, queue = new Map(), running = null, next = 1, pending = 0, timer = 0;
            var playing = new Set();
            function run(time) {
                pending = 0;
                last = performance.now();
                running = queue; queue = new Map();
                running.forEach(function (callback) {
                    try { callback(time); } catch (e) { reportError(e); }
                });
                running = null;
            }
            function frame() { timer = 0; pending = raf.call(window, run); }
            function request() {
                if (pending || timer) return;
                var t = performance.now();
                if (t - last > GAP) start = t;
                var still = Math.min(t - start, t - input);
                if (still > CALM_AFTER && !document.fullscreenElement) {
                    var interval = playing.size ? VIDEO_INTERVAL : still > CALMER_AFTER ? CALMER_INTERVAL : CALM_INTERVAL;
                    timer = setTimeout(frame, Math.max(0, last + interval - 8 - t));
                } else frame();
            }
            function touched() {
                input = performance.now();
                if (timer) { clearTimeout(timer); frame(); }
            }
            ['pointerdown', 'pointermove', 'touchstart', 'touchmove', 'wheel', 'keydown', 'scroll'].forEach(function (type) {
                addEventListener(type, touched, {capture: true, passive: true});
            });
            ['playing', 'pause', 'ended', 'emptied'].forEach(function (type) {
                addEventListener(type, function (event) {
                    if (!(event.target instanceof HTMLMediaElement) || event.target instanceof HTMLAudioElement) return;
                    if (type === 'playing') playing.add(event.target); else playing.delete(event.target);
                }, {capture: true, passive: true});
            });
            // Proxies, so the functions still read as native code to the page.
            window.requestAnimationFrame = window.webkitRequestAnimationFrame = new Proxy(raf, {
                apply: function (target, self, args) {
                    if (typeof args[0] !== 'function') return Reflect.apply(target, window, args);
                    var id = next++;
                    queue.set(id, args[0]);
                    request();
                    return id;
                }
            });
            window.cancelAnimationFrame = window.webkitCancelAnimationFrame = new Proxy(caf, {
                apply: function (target, self, args) {
                    if (running) running.delete(args[0]);
                    queue.delete(args[0]);
                    if (queue.size) return;
                    if (timer) { clearTimeout(timer); timer = 0; }
                    if (pending) { caf.call(window, pending); pending = 0; }
                }
            });
        })();"
    // Overscroll, in the main frame: the page says when one finger starts
    // pulling past its top (start:1) or bottom (start:-1), and when that
    // ends; the window follows the finger itself from there (see `finger`).
    // A pull in an element that can still scroll that way is left alone.
    // The page itself always stretches, as on iOS, even when its root sets
    // overscroll-behavior none or contain: sites do that to turn off
    // Chrome's own pull to refresh (X runs its own), so it is reported as
    // owned and only the reload is skipped.
    readonly property string pullScript: "
        (function () {
            if (window.__omarchyPull) return; window.__omarchyPull = true;
            var active = false, anchor = null, edge = 0, lastY = 0, pulled = 0;
            function owned() {
                var root = getComputedStyle(document.documentElement).overscrollBehaviorY;
                var body = document.body ? getComputedStyle(document.body).overscrollBehaviorY : 'auto';
                return root === 'none' || root === 'contain' || body === 'none' || body === 'contain';
            }
            function scrollsFurther(target, down) {
                for (var e = target; e && e !== document.body && e !== document.documentElement; e = e.parentElement) {
                    if (e.scrollHeight <= e.clientHeight + 1) continue;
                    var y = getComputedStyle(e).overflowY;
                    if (y !== 'auto' && y !== 'scroll') continue;
                    if (down ? e.scrollTop > 0 : e.scrollTop + e.clientHeight < e.scrollHeight - 1) return true;
                }
                var page = document.scrollingElement || document.documentElement;
                return down ? window.scrollY > 0 : window.scrollY + window.innerHeight < page.scrollHeight - 1;
            }
            function report(value) {
                if (value === pulled) return;
                pulled = value;
                console.info(value ? 'omarchy-pull:start:' + value + (owned() ? ':owned' : '') : 'omarchy-pull:end');
            }
            function stop() { active = false; anchor = null; report(0); }
            addEventListener('touchstart', function (ev) {
                active = ev.touches.length === 1;
                anchor = null;
                if (active) lastY = ev.touches[0].clientY;
            }, {passive: true, capture: true});
            addEventListener('touchmove', function (ev) {
                if (!active) return;
                if (ev.touches.length !== 1) { stop(); return; }
                var y = ev.touches[0].clientY;
                if (anchor === null && y !== lastY && !scrollsFurther(ev.target, y > lastY)) {
                    anchor = lastY;
                    edge = y > lastY ? 1 : -1;
                }
                lastY = y;
                if (anchor === null) return;
                if ((y - anchor) * edge <= 0) { anchor = null; report(0); return; }
                report(edge);
            }, {passive: true, capture: true});
            addEventListener('touchend', stop, {passive: true, capture: true});
            addEventListener('touchcancel', stop, {passive: true, capture: true});
        })();"

    Item {
        id: pageArea
        anchors { top: parent.top; left: parent.left; right: parent.right; bottom: parent.bottom; bottomMargin: win.barSpace }

        clip: true
        StackLayout {
            id: stack
            anchors.fill: parent
            // Off screen, every tab is a background tab to Chromium (host.shown).
            visible: host.shown
            currentIndex: win.current
            transform: Translate { y: win.stretch }
            Repeater {
                id: views
                model: tabs
                WebEngineView {
                    id: page
                    required property int index
                    required property string start
                    profile: host.profile
                    url: start
                    settings.fullScreenSupportEnabled: true
                    settings.pdfViewerEnabled: true
                    settings.focusOnNavigationEnabled: false
                    // Touch scrolling: no scrollbars, in the page or its inner scrollers.
                    settings.showScrollBars: false
                    backgroundColor: theme.background
                    userScripts.collection: [{name: "frames", sourceCode: win.framesScript,
                                              injectionPoint: WebEngineScript.DocumentCreation,
                                              worldId: WebEngineScript.MainWorld, runsOnSubFrames: true},
                                             {name: "keyboard", sourceCode: win.keyboardScript,
                                              injectionPoint: WebEngineScript.DocumentReady,
                                              worldId: WebEngineScript.ApplicationWorld, runsOnSubFrames: true},
                                             {name: "pull", sourceCode: win.pullScript,
                                              injectionPoint: WebEngineScript.DocumentReady,
                                              worldId: WebEngineScript.ApplicationWorld, runsOnSubFrames: false}]
                        // A site theme file's site also themes its frames: a self-hosted app may show
                        // each module in a frame of its own (another port), with the same colours.
                        .concat(win.siteTheme ? [{name: "theme", sourceCode: win.themeScript(win.siteTheme),
                                                  injectionPoint: WebEngineScript.DocumentCreation,
                                                  worldId: WebEngineScript.MainWorld,
                                                  runsOnSubFrames: win.themedSite === "custom" && win.customTheme.subframes !== false}] : [])
                        .concat(win.siteScript ? [{name: "site", sourceCode: win.siteScript,
                                                   injectionPoint: WebEngineScript.DocumentCreation,
                                                   worldId: WebEngineScript.MainWorld, runsOnSubFrames: false}] : [])
                    onJavaScriptConsoleMessage: (level, message) => {
                        if (message === "omarchy-keyboard:show") { keyboardHide.stop(); host.keyboard("show"); }
                        else if (message === "omarchy-keyboard:hide") keyboardHide.restart();
                        else if (message.startsWith("omarchy-pull:start:")) {
                            const parts = message.slice(19).split(":");
                            win.startPull(Number(parts[0]), parts[1] === "owned");
                        }
                        else if (message === "omarchy-pull:end") win.pullEdge = 0;
                    }
                    onFindTextFinished: result => { win.matches = result.numberOfMatches; win.match = result.activeMatch; }
                    // A web app keeps to its site (subdomains included: a sign-in page on
                    // accounts.snapchat.com): a link elsewhere opens in the browser.
                    // Main-frame navigations are logged with their type: a page's own
                    // location.reload() shows as "reload".
                    onNavigationRequested: request => {
                        if (request.isMainFrame)
                            host.logEvent("navigate " + win.navigationName(request.navigationType) + " " + request.url);
                        if (!win.appMode || !request.isMainFrame
                            || request.navigationType !== WebEngineNavigationRequest.LinkClickedNavigation
                            || win.siteOf(request.url.toString()) === win.siteOf(host.startUrl)) return;
                        request.reject();
                        host.openInBrowser(request.url.toString());
                    }
                    // A web app opens a "new window" for its own site in place (Snapchat's
                    // Chat link to its web client); one elsewhere goes to the browser.
                    onNewWindowRequested: request => {
                        if (win.appMode) {
                            if (win.siteOf(request.requestedUrl.toString()) === win.siteOf(host.startUrl))
                                page.url = request.requestedUrl;
                            else
                                host.openInBrowser(request.requestedUrl.toString());
                            return;
                        }
                        win.newTab("about:blank", request.userInitiated);
                        Qt.callLater(() => { const v = views.itemAt(tabs.count - 1); if (v) v.acceptAsNewWindow(request); });
                    }
                    onWindowCloseRequested: if (tabs.count > 1) win.closeTab(index)
                    onFullScreenRequested: request => { win.fullscreen = request.toggleOn; request.accept(); }
                    onPermissionRequested: permission => { win.permission = permission; }
                    onFileDialogRequested: request => { request.accepted = true; win.openPicker(request); }
                    onUrlChanged: if (index === win.current) win.saveTabs()
                    onLoadingChanged: info => {
                        if (info.status !== WebEngineLoadingInfo.LoadSucceededStatus) return;
                        win.remember(url.toString(), title);
                        if (index === win.current) shotLater.restart();
                    }
                    onTitleChanged: {
                        win.retitle(url.toString(), title);
                        if (win.appMode && title) win.title = title;
                    }
                    onRenderProcessTerminated: (status, code) => {
                        host.logEvent("renderer exited status " + status + " code " + code + " " + url);
                        if (status !== WebEngineView.NormalTerminationStatus) reload();
                    }
                    // The tab overview's picture of this page at half size. The
                    // grab result is kept: its image lives as long as it does.
                    // Shown by the compositor directly, the page is only a hole
                    // in Qt's window (main.cpp), so Qt draws it for a few frames
                    // first.
                    property var shot: null
                    function capture() {
                        if (!visible || width < 2 || height < 2) return;
                        host.directPages(false);
                        grabSoon.restart();
                    }
                    Timer {
                        id: grabSoon
                        interval: 50
                        onTriggered: {
                            const grabbing = page.visible && page.width >= 2 && page.height >= 2
                                && page.grabToImage(result => { page.shot = result; host.directPages(true); },
                                                    Qt.size(Math.round(page.width / 2), Math.round(page.height / 2)));
                            if (!grabbing) host.directPages(true);
                        }
                    }
                }
            }
        }
        // Watches the finger for the overscroll without taking the touch: a
        // passive handler above the page, so the page still gets every event.
        Item {
            anchors.fill: parent
            PointHandler {
                id: finger
                acceptedDevices: PointerDevice.TouchScreen
                onActiveChanged: if (!active) win.fingerLifted()
            }
        }
        // Pull to refresh: a circle in the gap above the stretched page, which
        // turns to the accent colour once letting go reloads.
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: (win.stretch - height) / 2
            width: 44
            height: 44
            radius: 22
            visible: win.stretch > 8 && !win.pullOwned
            opacity: Math.min(1, (win.stretch - 8) / 40)
            color: theme.surface
            border.width: 1
            border.color: theme.muted
            Text {
                anchors.centerIn: parent
                text: "\u{F0450}"   // refresh
                rotation: win.pull * 270
                color: win.pull >= 1 ? theme.accent : theme.secondary
                font.family: theme.font
                font.pixelSize: 22
            }
        }
        // The loading line.
        Rectangle {
            anchors { left: parent.left; bottom: parent.bottom }
            height: 3
            visible: win.view !== null && win.view.loading
            width: parent.width * (win.view ? win.view.loadProgress / 100 : 0)
            color: theme.accent
        }
    }

    // The bar: tabs, the address, the menu. A sideways swipe on it changes tab.
    Rectangle {
        id: bar
        visible: !win.fullscreen && !win.appMode
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 64
        color: theme.background
        Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top } height: 1; color: theme.muted }
        RowLayout {
            anchors { fill: parent; leftMargin: 10; rightMargin: 10; topMargin: 8; bottomMargin: 8 }
            spacing: 8
            BarButton {
                visible: !win.finding
                text: tabs.count > 99 ? ":D" : String(tabs.count)
                boxed: true
                onClicked: { if (win.overview) win.overview = false; else win.showOverview(); win.menuOpen = false; }
            }
            // Find in page, in the address's place.
            Rectangle {
                visible: win.finding
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: theme.radius(12)
                color: theme.surface
                border.width: 1
                border.color: theme.accent
                TextInput {
                    id: findField
                    anchors { left: parent.left; right: findCount.left; top: parent.top; bottom: parent.bottom
                              leftMargin: 14; rightMargin: 8 }
                    verticalAlignment: TextInput.AlignVCenter
                    color: theme.foreground
                    selectionColor: theme.selection
                    font.family: theme.font
                    font.pixelSize: 16
                    clip: true
                    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    onTextEdited: if (win.view) win.view.findText(text)
                    onAccepted: if (win.view && text) win.view.findText(text)
                    onActiveFocusChanged: host.keyboard(activeFocus ? "show" : "hide")
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: findField.text === ""
                        text: "Find in page"
                        color: theme.secondary
                        font: findField.font
                    }
                }
                Text {
                    id: findCount
                    anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
                    text: findField.text === "" ? "" : win.matches === 0 ? "0" : win.match + "/" + win.matches
                    color: theme.secondary
                    font.family: theme.font
                    font.pixelSize: 13
                }
            }
            BarButton {
                visible: win.finding
                text: "\u{F0143}"   // chevron up
                onClicked: if (win.view && findField.text) win.view.findText(findField.text, WebEngineView.FindBackward)
            }
            BarButton {
                visible: win.finding
                text: "\u{F0140}"   // chevron down
                onClicked: if (win.view && findField.text) win.view.findText(findField.text)
            }
            Rectangle {
                visible: !win.finding
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: theme.radius(12)
                color: theme.surface
                border.width: address.activeFocus ? 1 : 0
                border.color: theme.accent
                TextInput {
                    id: address
                    // Typed into since editing began: until then the
                    // suggestions are bookmarks and recent pages.
                    property bool typed: false
                    anchors { fill: parent; leftMargin: 14; rightMargin: 14 }
                    verticalAlignment: TextInput.AlignVCenter
                    color: theme.foreground
                    selectionColor: theme.selection
                    font.family: theme.font
                    font.pixelSize: 16
                    clip: true
                    inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                    text: activeFocus ? text : (win.view ? (win.view.loading ? win.hostOf(win.view.url.toString())
                                                                             : (win.view.title || win.hostOf(win.view.url.toString()))) : "")
                    onActiveFocusChanged: {
                        win.editing = activeFocus;
                        typed = false;
                        if (activeFocus) {
                            text = win.view ? win.view.url.toString() : "";
                            selectAll();
                            host.keyboard("show");
                        } else {
                            host.keyboard("hide");
                        }
                    }
                    onTextEdited: typed = true
                    onAccepted: {
                        if (win.view) win.view.url = win.target(text);
                        focus = false;
                    }
                }
            }
            BarButton {
                text: win.finding || (win.view && win.view.loading) ? "\u{F0156}" : "\u{F01D9}"   // close / dots-vertical
                onClicked: {
                    if (win.finding) win.stopFinding();
                    else if (win.view && win.view.loading) win.view.stop();
                    else win.menuOpen = !win.menuOpen;
                }
            }
        }
        DragHandler {
            target: null
            yAxis.enabled: false
            onActiveChanged: {
                if (active || Math.abs(translation.x) < 80 || tabs.count < 2) return;
                win.captureCurrent();
                win.current = (win.current + (translation.x < 0 ? 1 : -1) + tabs.count) % tabs.count;
                win.saveTabs();
            }
        }
    }

    // Suggestions while the address is edited, nearest the bar first.
    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top; bottom: bar.top }
        visible: win.editing
        color: theme.background
        ListView {
            id: suggestionList
            anchors.fill: parent
            clip: true
            verticalLayoutDirection: ListView.BottomToTop
            model: win.editing ? win.suggestions(address.typed ? address.text : "") : []
            delegate: Rectangle {
                id: suggestion
                required property var modelData
                width: suggestionList.width
                height: 64
                color: suggestionTap.pressed ? theme.selection : "transparent"
                Text {
                    id: suggestionIcon
                    anchors { left: parent.left; leftMargin: 14; verticalCenter: parent.verticalCenter }
                    width: 28
                    horizontalAlignment: Text.AlignHCenter
                    text: suggestion.modelData.bookmark ? "\u{F00C0}" : "\u{F02DA}"   // bookmark / history
                    color: suggestion.modelData.bookmark ? theme.accent : theme.secondary
                    font.family: theme.font
                    font.pixelSize: 20
                }
                Column {
                    anchors { left: suggestionIcon.right; leftMargin: 16; right: parent.right; rightMargin: 18
                              verticalCenter: parent.verticalCenter }
                    spacing: 2
                    Text {
                        width: parent.width
                        text: suggestion.modelData.title
                        color: theme.foreground
                        font.family: theme.font
                        font.pixelSize: 15
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: win.hostOf(suggestion.modelData.url)
                        color: theme.secondary
                        font.family: theme.font
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                }
                TapHandler { id: suggestionTap; onTapped: win.open(suggestion.modelData.url) }
            }
        }
    }

    // The tab overview.
    Rectangle {
        anchors { fill: parent; bottomMargin: bar.height }
        visible: win.overview
        color: theme.background
        GridView {
            id: grid
            anchors { fill: parent; margins: 12 }
            cellWidth: width / 2
            // The page's own shape, so a card looks like the tab it opens.
            cellHeight: Math.round(cellWidth * Math.max(1.2, pageArea.height / Math.max(1, pageArea.width)))
            model: tabs
            delegate: Item {
                id: card
                required property int index
                width: grid.cellWidth
                height: grid.cellHeight
                readonly property var tabView: views.itemAt(index)
                readonly property var shot: tabView ? tabView.shot : null
                readonly property string title: tabView ? (tabView.title || "New tab") : ""
                Item {
                    id: frame
                    anchors { fill: parent; margins: 6 }
                    Rectangle {
                        id: cardMask
                        anchors.fill: parent
                        radius: theme.radius(14)
                        visible: false
                        layer.enabled: true
                    }
                    // The page as it last looked (or its title until it has
                    // been pictured), rounded like the card.
                    Rectangle {
                        anchors.fill: parent
                        color: theme.surface
                        layer.enabled: true
                        layer.effect: MultiEffect {
                            maskEnabled: true
                            maskSource: cardMask
                            maskThresholdMin: 0.5
                            maskSpreadAtMin: 1.0
                        }
                        Image {
                            anchors.fill: parent
                            visible: card.shot !== null
                            source: card.shot ? card.shot.url : ""
                            fillMode: Image.PreserveAspectCrop
                            verticalAlignment: Image.AlignTop
                        }
                        Column {
                            visible: card.shot === null
                            anchors { fill: parent; margins: 12 }
                            spacing: 6
                            Text {
                                width: parent.width - 28
                                text: card.title
                                color: theme.foreground
                                font.family: theme.font
                                font.pixelSize: 15
                                wrapMode: Text.Wrap
                                maximumLineCount: 6
                                elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                text: card.tabView ? win.hostOf(card.tabView.url.toString()) : ""
                                color: theme.secondary
                                font.family: theme.font
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                        }
                        Rectangle {
                            visible: card.shot !== null
                            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                            height: 36
                            color: Qt.rgba(theme.surface.r, theme.surface.g, theme.surface.b, 0.92)
                            Row {
                                anchors { fill: parent; leftMargin: 10; rightMargin: 10 }
                                spacing: 8
                                Image {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 16; height: 16
                                    source: card.tabView ? card.tabView.icon : ""
                                    visible: status === Image.Ready
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: parent.width - 24
                                    text: card.title
                                    color: theme.foreground
                                    font.family: theme.font
                                    font.pixelSize: 13
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: theme.radius(14)
                        color: "transparent"
                        border.width: card.index === win.current ? 2 : 1
                        border.color: card.index === win.current ? theme.accent : theme.muted
                    }
                    Rectangle {
                        anchors { right: parent.right; top: parent.top; margins: 8 }
                        width: 30; height: 30
                        radius: 15
                        color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.8)
                        Text {
                            anchors.centerIn: parent
                            text: "\u{F0156}"
                            color: theme.foreground
                            font.family: theme.font
                            font.pixelSize: 18
                        }
                        // Takes the tap for itself: the card's own handler
                        // also saw it, opened the next tab in the closed
                        // one's place and closed the overview.
                        TapHandler {
                            margin: 8
                            gesturePolicy: TapHandler.ReleaseWithinBounds
                            onTapped: win.closeTab(card.index)
                        }
                    }
                    TapHandler { onTapped: { win.current = card.index; win.overview = false; win.saveTabs(); } }
                }
            }
        }
        Rectangle {
            anchors { right: parent.right; bottom: parent.bottom; margins: 16 }
            width: 64; height: 64
            radius: theme.radius(20)
            color: theme.accent
            Text { anchors.centerIn: parent; text: "\u{F0415}"; color: theme.background; font.family: theme.font; font.pixelSize: 30 }
            TapHandler { onTapped: { win.newTab(win.home); address.forceActiveFocus(); } }
        }
    }

    // The menu.
    Sheet {
        visible: win.menuOpen
        onDismissed: win.menuOpen = false
        Column {
            width: parent.width
            spacing: 4
            Row {
                spacing: 8
                MenuIcon { text: "\u{F004D}"; enabled: win.view && win.view.canGoBack; onClicked: win.view.goBack() }
                MenuIcon { text: "\u{F0054}"; enabled: win.view && win.view.canGoForward; onClicked: win.view.goForward() }
                MenuIcon { text: "\u{F0450}"; onClicked: { win.view.reload(); win.menuOpen = false; } }
                MenuIcon { text: "\u{F0415}"; onClicked: { win.menuOpen = false; win.newTab(win.home); address.forceActiveFocus(); } }
            }
            MenuItem {
                // Per site: its pages are asked for with a desktop agent.
                readonly property string site: win.view ? win.view.url.toString().replace(/^[a-z]+:\/\/([^/?#:]+).*$/i, "$1") : ""
                label: "Desktop site"
                // Read again each time the menu opens (the host is not a property).
                checked: win.menuOpen && site !== "" && host.isDesktop(site)
                onClicked: {
                    host.setDesktop(site, !host.isDesktop(site));
                    win.view.reload();
                    win.menuOpen = false;
                }
            }
            MenuItem {
                label: "Bookmark"
                checked: win.menuOpen && win.view !== null && win.isBookmarked(win.view.url.toString())
                onClicked: { win.toggleBookmark(); win.menuOpen = false; }
            }
            MenuItem { label: "Share"; onClicked: { win.menuOpen = false; host.share(win.view.url.toString(), win.view.title || ""); } }
            MenuItem { label: "Copy link"; onClicked: { host.copy(win.view.url.toString()); win.notice = "Link copied"; win.menuOpen = false; } }
            MenuItem {
                label: "Add to home screen"
                onClicked: {
                    win.menuOpen = false;
                    appName.text = win.view ? (win.view.title || win.hostOf(win.view.url.toString())) : "";
                    win.adding = true;
                }
            }
            MenuItem {
                label: "Find in page"
                onClicked: { win.menuOpen = false; win.overview = false; win.finding = true; findField.forceActiveFocus(); }
            }
            MenuItem { label: "Close tab"; onClicked: { win.closeTab(win.current); win.menuOpen = false; } }
            MenuItem { label: "Clear history"; onClicked: { win.clearHistory(); win.menuOpen = false; } }
            Text {
                width: parent.width
                topPadding: 8
                visible: host.blocklistSize > 0
                text: host.blockedCount + " requests blocked (" + host.blocklistSize + " domains listed)"
                color: theme.secondary
                font.family: theme.font
                font.pixelSize: 12
            }
        }
    }

    // Add to home screen: the name, and the icon the drawer will show (the
    // site's icon on the theme's accent, or its first letter).
    Sheet {
        visible: win.adding
        onDismissed: win.adding = false
        Column {
            width: parent.width
            spacing: 14
            Row {
                id: addRow
                width: parent.width
                spacing: 16
                // Drawn at the saved size (192 px) and shown at 72.
                Item {
                    width: 72
                    height: 72
                    Rectangle {
                        id: appIcon
                        width: 192
                        height: 192
                        scale: 72 / 192
                        transformOrigin: Item.TopLeft
                        radius: theme.radius(48)
                        color: theme.accent
                        Image {
                            id: siteIcon
                            anchors.centerIn: parent
                            width: 112
                            height: 112
                            sourceSize: Qt.size(128, 128)
                            source: win.adding && win.view ? win.view.icon : ""
                            smooth: true
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: siteIcon.status !== Image.Ready
                            text: (appName.text || "?").substring(0, 1).toUpperCase()
                            color: theme.background
                            font.family: theme.font
                            font.pixelSize: 96
                            font.bold: true
                        }
                    }
                }
                Column {
                    width: addRow.width - 72 - 16
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6
                    Text { text: "Add to home screen"; color: theme.secondary; font.family: theme.font; font.pixelSize: 13 }
                    Rectangle {
                        width: parent.width
                        height: 48
                        radius: theme.radius(10)
                        color: theme.surface
                        border.width: appName.activeFocus ? 1 : 0
                        border.color: theme.accent
                        TextInput {
                            id: appName
                            anchors { fill: parent; leftMargin: 12; rightMargin: 12 }
                            verticalAlignment: TextInput.AlignVCenter
                            color: theme.foreground
                            font.family: theme.font
                            font.pixelSize: 16
                            clip: true
                            maximumLength: 60
                            onActiveFocusChanged: host.keyboard(activeFocus ? "show" : "hide")
                            onAccepted: win.addToHome()
                        }
                    }
                }
            }
            RowLayout {
                width: parent.width
                spacing: 10
                SheetButton { Layout.fillWidth: true; label: "Cancel"; onClicked: { win.adding = false; appName.focus = false; } }
                SheetButton { Layout.fillWidth: true; label: "Add"; primary: true; onClicked: win.addToHome() }
            }
        }
    }

    // A page asking for a file: the camera, the newest photos, or nothing.
    Sheet {
        visible: win.fileRequest !== null && win.capturePath === ""
        onDismissed: win.cancelPicker()
        Column {
            width: parent.width
            spacing: 12
            Text {
                width: parent.width
                text: win.wantsImages(win.fileRequest) ? "Add a photo" : "Add a file"
                color: theme.foreground
                font.family: theme.font
                font.pixelSize: 19
                font.bold: true
            }
            SheetButton {
                width: parent.width
                visible: win.wantsImages(win.fileRequest)
                label: "Take photo"
                primary: true
                onClicked: win.takePhoto()
            }
            Text {
                visible: win.recent.length > 0
                text: "Recent photos"
                color: theme.secondary
                font.family: theme.font
                font.pixelSize: 13
            }
            Grid {
                width: parent.width
                visible: win.recent.length > 0
                columns: 3
                spacing: 8
                Repeater {
                    model: win.recent
                    Rectangle {
                        required property string modelData
                        width: (parent.width - 16) / 3
                        height: width
                        radius: theme.radius(10)
                        color: theme.surface
                        clip: true
                        Image {
                            anchors.fill: parent
                            source: "file://" + parent.modelData
                            sourceSize: Qt.size(300, 300)
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                        }
                        TapHandler { onTapped: win.pickFile(parent.modelData) }
                    }
                }
            }
            SheetButton { width: parent.width; label: "Cancel"; onClicked: win.cancelPicker() }
        }
    }

    // A site asking for the camera, the microphone, the location...
    Sheet {
        visible: win.permission !== null
        onDismissed: { if (win.permission) win.permission.deny(); win.permission = null; }
        Column {
            width: parent.width
            spacing: 12
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: win.permission ? win.hostOf(win.permission.origin.toString()) + " wants to use your "
                      + ({1: "microphone", 2: "camera", 3: "camera and microphone", 4: "screen", 5: "screen and sound",
                          6: "mouse pointer", 7: "notifications", 8: "location", 9: "clipboard", 10: "fonts"})[win.permission.permissionType] || "device"
                      : ""
                color: theme.foreground
                font.family: theme.font
                font.pixelSize: 17
            }
            RowLayout {
                width: parent.width
                spacing: 10
                SheetButton { Layout.fillWidth: true; label: "Block"; onClicked: { win.permission.deny(); win.permission = null; } }
                SheetButton { Layout.fillWidth: true; label: "Allow"; primary: true; onClicked: { win.permission.grant(); win.permission = null; } }
            }
        }
    }

    // Downloads land in ~/Downloads, said in a passing notice.
    Connections {
        target: host.profile
        function onDownloadRequested(download) {
            download.downloadDirectory = host.downloads;
            download.accept();
            win.notice = "Downloading " + download.downloadFileName;
        }
        function onDownloadFinished(download) {
            win.notice = download.state === 2 ? "Saved " + download.downloadFileName + " in Downloads" : "Download failed";
        }
    }
    Rectangle {
        anchors { horizontalCenter: parent.horizontalCenter; bottom: bar.top; bottomMargin: 16 }
        visible: win.notice !== "" && !win.editing
        width: noticeText.implicitWidth + 32
        height: 44
        radius: theme.radius(22)
        color: theme.surface
        Text { id: noticeText; anchors.centerIn: parent; text: win.notice; color: theme.foreground; font.family: theme.font; font.pixelSize: 14 }
        onVisibleChanged: if (visible) noticeTimer.restart()
        Timer { id: noticeTimer; interval: 3000; onTriggered: win.notice = "" }
    }

    component BarButton: Rectangle {
        id: button
        property string text: ""
        property bool boxed: false
        signal clicked()
        Layout.preferredWidth: 48
        Layout.fillHeight: true
        radius: theme.radius(12)
        color: tap.pressed ? theme.selection : "transparent"
        Rectangle {
            anchors.centerIn: parent
            visible: button.boxed
            width: 30; height: 30
            radius: theme.radius(8)
            color: "transparent"
            border.width: 2
            border.color: theme.foreground
        }
        Text {
            anchors.centerIn: parent
            text: button.text
            color: theme.foreground
            font.family: theme.font
            font.pixelSize: button.boxed ? 14 : 24
            font.bold: button.boxed
        }
        TapHandler { id: tap; onTapped: button.clicked() }
    }

    component Sheet: Item {
        id: sheet
        default property alias content: box.data
        signal dismissed()
        anchors { fill: parent; bottomMargin: win.barSpace }
        // A tap above the panel dismisses the sheet. The panel's own buttons'
        // taps reach this handler too (a child's TapHandler does not stop
        // the one beneath), so the position decides: a Take photo tap had
        // cancelled the page's file request as the camera opened.
        Rectangle {
            anchors.fill: parent
            color: "#80000000"
            TapHandler { onTapped: point => { if (point.position.y < panel.y) sheet.dismissed(); } }
        }
        Rectangle {
            id: panel
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: box.implicitHeight + 40
            color: theme.background
            radius: theme.radius(18)
            Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top } height: 1; color: theme.muted }
            Item {
                id: box
                anchors { fill: parent; margins: 20 }
                implicitHeight: childrenRect.height
            }
        }
    }

    component MenuIcon: Rectangle {
        id: icon
        property string text: ""
        signal clicked()
        width: 64; height: 56
        radius: theme.radius(12)
        color: iconTap.pressed ? theme.selection : theme.surface
        opacity: enabled ? 1 : 0.35
        Text { anchors.centerIn: parent; text: icon.text; color: theme.foreground; font.family: theme.font; font.pixelSize: 24 }
        TapHandler { id: iconTap; onTapped: icon.clicked() }
    }

    component MenuItem: Rectangle {
        id: item
        property string label: ""
        property bool checked: false
        signal clicked()
        width: parent ? parent.width : 0
        height: 52
        color: itemTap.pressed ? theme.selection : "transparent"
        Text { anchors { left: parent.left; leftMargin: 6; verticalCenter: parent.verticalCenter } text: item.label; color: theme.foreground; font.family: theme.font; font.pixelSize: 16 }
        Text {
            anchors { right: parent.right; rightMargin: 6; verticalCenter: parent.verticalCenter }
            visible: item.checked
            text: "\u{F012C}"
            color: theme.accent
            font.family: theme.font
            font.pixelSize: 20
        }
        TapHandler { id: itemTap; onTapped: item.clicked() }
    }

    component SheetButton: Rectangle {
        id: sb
        property string label: ""
        property bool primary: false
        signal clicked()
        implicitHeight: 52
        radius: theme.radius(12)
        color: primary ? (sbTap.pressed ? Qt.darker(theme.accent, 1.2) : theme.accent) : (sbTap.pressed ? theme.selection : theme.surface)
        Text { anchors.centerIn: parent; text: sb.label; color: sb.primary ? theme.background : theme.foreground; font.family: theme.font; font.pixelSize: 16; font.bold: true }
        TapHandler { id: sbTap; onTapped: sb.clicked() }
    }
}
