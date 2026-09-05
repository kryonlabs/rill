# Xfce plugin validation

Validated on 2026-09-05 in a private Xvfb X11 display and private D-Bus session,
using the installed Xfce session manager and Rill desktop. All 37 installed
plugin modules loaded. They remained loaded after panel size/orientation changes
and reloaded after panel restart. Desktop crash restart and session logout also
passed in this configuration.

The test observes mapped shared libraries in the panel and wrapper processes.
It does not establish functional correctness of each plugin, configuration
migration, physical hardware integration or Wayland/native Plan 9 support.
The test deliberately has no system-bus access, so power, network and other
hardware services are not validated by these results. Plugins needing accounts,
locations or custom commands use their unconfigured defaults.

Reproduce with `make plugin-smoke`. Set `RILL_PLUGIN_REPORT` to an absolute
filename to save the observed inventory as JSON. The inventory is discovered
from `/usr/share/xfce4/panel/plugins`, not a hard-coded list.

| Plugin | Module | API | Result |
| --- | --- | --- | --- |
| actions | actions | 2.0 | Loaded |
| applicationsmenu | applicationsmenu | 2.0 | Loaded |
| battery | battery | 2.0 | Loaded |
| clock | clock | 2.0 | Loaded |
| cpufreq | cpufreq | 2.0 | Loaded |
| cpugraph | cpugraph | 2.0 | Loaded |
| directorymenu | directorymenu | 2.0 | Loaded |
| diskperf | diskperf | 2.0 | Loaded |
| fsguard | fsguard | 2.0 | Loaded |
| genmon | genmon | 2.0 | Loaded |
| launcher | launcher | 2.0 | Loaded |
| mailwatch | mailwatch | 2.0 | Loaded |
| netload | netload | 2.0 | Loaded |
| notification-plugin | notification-plugin | 2.0 | Loaded |
| pager | pager | 2.0 | Loaded |
| places | places | 2.0 | Loaded |
| power-manager-plugin | xfce4powermanager | 2.0 | Loaded |
| pulseaudio | pulseaudio-plugin | 2.0 | Loaded |
| screenshooter | screenshooterplugin | 2.0 | Loaded |
| separator | separator | 2.0 | Loaded |
| showdesktop | showdesktop | 2.0 | Loaded |
| smartbookmark | smartbookmark | 2.0 | Loaded |
| systemload | systemload | 2.0 | Loaded |
| systray | systray | 2.0 | Loaded |
| tasklist | tasklist | 2.0 | Loaded |
| thunar-tpa | thunar-tpa | 2.0 | Loaded |
| wavelan | wavelan | 2.0 | Loaded |
| weather | weather | 2.0 | Loaded |
| whiskermenu | whiskermenu | 2.0 | Loaded |
| windowmenu | windowmenu | 2.0 | Loaded |
| xfce4-clipman-plugin | clipman | 2.0 | Loaded |
| xfce4-dict-plugin | xfce4dict | 2.0 | Loaded |
| xfce4-notes-plugin | notes | 2.0 | Loaded |
| xfce4-sensors-plugin | xfce4-sensors-plugin | 2.0 | Loaded |
| xfce4-timer-plugin | xfcetimer | 2.0 | Loaded |
| xfce4-verve-plugin | verve | 2.0 | Loaded |
| xkb | xkb | 2.0 | Loaded |
