---
page_icon: img/moduiThumb.jpg
icon_width: 120px
---
## Support for LV2 Plugins with MOD User Interfaces

{% include pageIconL.html %}

Many LV2 plugins provide custom plugin user interfaces based on [MOD Audio's](https://mod.audio/desktop/) 
[ModUi framework](https://wiki.mod.audio/wiki/MOD_Web_GUI_Framework). When using such a plugin, you can choose whether to use the MOD Web GUI Framework user interface (the MOD UI) or the default PiPedal user interface. For the most part, the same functionality is available in both interfaces.

Plugins that are distributed via Linux distributions usually do not implement MOD user interfaces; but all (or almost all) of the plugins that are available from the [PatchStorage website](https://patchstorage.com/platform/lv2-plugins/) do implement MOD user interfaces. Many plugins that are available from GitHub or other sources also implement MOD user interfaces. Often you can find updated versions of a plugin that is published by a distro on PatchStorage which does provide a MOD UI.

Here's an example of a MOD UI for the ZAM Eq2 plugin:

<img src="img/zam-modui.jpg" alt="ZAM Eq2 MOD UI" style="width: 100%; max-width: 400px;box-sizing: border-box;padding-left: 24px; padding-right: 24px; "/>


And the PiPedal UI for the same plugin:

<img src="img/zam-pp-ui.png" alt="ZAM Eq2 PiPedal UI" style="display: block; box-sizing: border-box; padding-left: 24px; padding-right: 24px; width: 100%; max-width: 400px; margin-left: auto; margin-right: auto"/>

Which UI you use is entirely up to you. MOD UIs tend to be a bit unfriendly for small-format devices like phones, but they do look great in a desktop browser. Sometimes the MOD UI will make better use of available screen space; sometimes the Pipedal UI will look better. It really depends on the plugin and your personal preferences. In this particular case, the MOD UI is more space efficient, but that is not always true.

To select which user interface to use, tap on the UI selection button in the toolbar for the plugin. If the plugin does not provide a MOD UI, the button will be disabled. 

<img src="img/selectUi.png" alt="MOD UI selection button" style="display: block; width: 100%; padding-left: 20px; padding-right: 20px; max-width: 300px; margin-left: auto; margin-right: auto"/>

Support for MOD UIs in PiPedal is still experimental, so you may encounter some issues. PiPedal does not yet support all of the features of the MOD UI framework, not because it can't, but because we haven't been able to locate examples of plugins that use those features, which makes it difficult to test. If you encounter any issues, please report them on PiPedal's [GitHub Issues page](https://github.com/rerdavies/pipedal/issues), and we will do our best to provide quick fixes.

### Installing MOD UIs for plugins from Debian and Ubuntu packages

MOD Audio publishes MOD UIs for many plugins that Debian and Ubuntu package without one, including most of the Guitarix plugins, the x42 plugins, the ZAM plugins, the Dragonfly reverbs and several DPF plugins, in their [mod-lv2-data](https://github.com/moddevices/mod-lv2-data) repository. The PiPedal source tree includes a script that downloads these MOD UIs and adds them to the plugins you have installed:

```
git clone --depth 1 https://github.com/rerdavies/pipedal.git
cd pipedal
sudo apt install git python3-lilv
tools/install-mod-guis.sh --dry-run     # show what would be installed
sudo tools/install-mod-guis.sh
sudo systemctl restart pipedald
```

On Ubuntu 26.04 this adds MOD UIs to 124 of the plugins in the `guitarix-lv2`, `x42-plugins`, `zam-plugins`, `dragonfly-reverb-lv2` and `dpf-plugins-lv2` packages.

`sudo tools/install-mod-guis.sh --install-plugins` first installs the packages from your distribution's archive (never from PPAs or other third-party repositories) that contain plugins MOD has UIs for but that aren't installed yet (e.g. `blop-lv2`, `fomp`, `swh-lv2`, `setbfree`, `abgate`), and then installs the MOD UIs for those too. Use `--install-plugins=fomp,swh-lv2` to install only some of them, and add `--dry-run` to see the list first. PiPedal reads its plugin list when it starts, so restart it afterwards.

How it works, and things to be aware of:

- Each MOD UI is installed in `/usr/local/lib/lv2` as a separate overlay bundle (`<bundle>-modgui.lv2`) that adds only the MOD UI description and its image, template and stylesheet files to the installed plugin. MOD's own builds of the plugins are never installed, and nothing installed by the packages is modified.
- MOD's UIs were made for MOD's builds of the plugins, which aren't always the same version as your distribution's. A MOD UI is only installed if every control, port and parameter it uses exists in the installed plugin; the script lists the ones it skips, and why. When a control's range differs from MOD's build, the UI is installed with a note: PiPedal uses the installed plugin's range, but the UI's artwork may show different scale markings.
- Some MOD UIs refer to a port by another name than the installed plugin uses: a typo in MOD's files (e.g. `exp_gm_gain` for `exp_fm_gain` in fomp's Moog High-Pass Filter), or a port that was renamed since MOD's build (e.g. several Guitarix plugins). The script adapts these UIs instead of skipping them, and lists them separately with each change:
  - A port is mapped to the installed one listed in [`tools/mod-gui-port-aliases.txt`](https://github.com/rerdavies/pipedal/blob/main/tools/mod-gui-port-aliases.txt). Each line is `<plugin URI> <port symbol in the MOD UI> <installed port symbol>  # reason` (`-` as the installed symbol hides the control). The entries were checked by hand against MOD's description of each plugin and the installed one: index, name and range of the port, and the UI's label for it.
  - Ports that aren't listed there are only mapped when the match is unambiguous: exactly one input (or output) control port of the installed plugin has the same index as the port in MOD's build and a similar name or the same range, or a nearly identical symbol (one or two typos). Ports that MOD's build has under their own name are never used for another.
  - Otherwise, the control is hidden: its entry is removed from the UI description, and the template elements that refer to it are removed (a knob's caption next to it may remain). A UI is skipped if most of its controls would be hidden, or if its script or stylesheet refers to a port that would change.
  - Only the overlay's copies change: its `modgui.ttl`, and adapted copies of the templates in the overlay's `pipedal-adapted` folder (the originals in the resource folder are left as MOD made them). The adapted UI must pass the same compatibility check as any other before it is installed.
- For the Guitarix plugins, the script also checks the UIs in the Guitarix 0.47.0 release (the version Debian and Ubuntu package), and uses those where MOD has none (e.g. the Redeye Chump and Big Chump amps) or where they match the installed plugin better.
- An overlay only ever contains MOD UI data: a MOD UI whose description includes anything else (plugin data such as `lv2:binary`, or references to files other than its own templates, stylesheets and images) is skipped. Files are only copied from inside the MOD UI's own bundle, and symbolic links are never followed: a MOD UI whose files include one is skipped. (In mod-lv2-data, only the duplicate `*-bad.lv2` bundles contain symbolic links, to the files of the bundle they duplicate, so nothing is lost.)
- `--install-plugins` checks with `apt-get -s` that every package it would install, including dependencies, comes from the official archive, and installs nothing otherwise.
- Downloads are pinned to tested versions of both repositories. The overlays use about 90MB of disk space, because each one contains the complete resource folder of the MOD bundle it comes from.
- If a plugin has two MOD UIs (e.g. you later install a version of the plugin that comes with its own), PiPedal uses one whose files exist; if both are complete, which one it uses is undefined. The script never adds a MOD UI to a plugin that already has a working one, and when run again, removes the overlays that are no longer needed.
- It can safely be run again at any time, e.g. after installing or updating plugin packages. `sudo tools/install-mod-guis.sh --uninstall` removes everything it installed. Run `tools/install-mod-guis.sh --help` for other options (`--dest`, `--lv2-dir`, and `--source` to use a local copy of mod-lv2-data).
- `tools/install-gx-modgui.sh`, which used to install the MOD UIs of a few Guitarix plugins, now runs `install-mod-guis.sh`.

The mod-lv2-data repository doesn't state a license for its contents. The MOD UI files (images, templates and stylesheets) were created by MOD Audio and the plugins' authors, and remain theirs. PiPedal doesn't distribute them: the script downloads them from MOD's public repository for your own use with the plugins you have installed. The Guitarix UI files are part of Guitarix (GPL).

&nbsp;


--------
[<< Which Plugins are Supported?](WhichLv2PluginsAreSupported.md)  | [Up](Documentation.md) | [Frequenty Asked Questions >>](FAQ.md)
