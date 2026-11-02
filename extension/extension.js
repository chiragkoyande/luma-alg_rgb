import GObject from "gi://GObject";
import GLib from "gi://GLib";
import Gio from "gi://Gio";
import St from "gi://St";

import { Extension, gettext as _ } from "resource:///org/gnome/shell/extensions/extension.js";
import * as Main from "resource:///org/gnome/shell/ui/main.js";
import * as PanelMenu from "resource:///org/gnome/shell/ui/panelMenu.js";
import * as PopupMenu from "resource:///org/gnome/shell/ui/popupMenu.js";
import * as Slider from "resource:///org/gnome/shell/ui/slider.js";

const LOG_PREFIX = "[ALG RGB]";
const CLI_NAME = "alg-rgb";
const DASHBOARD_URL = "http://127.0.0.1:8722";
const MAX_BRIGHTNESS = 4;

const COLORS = [
  { name: "Red", hex: "#ff3b30" },
  { name: "Orange", hex: "#ff7f00" },
  { name: "Yellow", hex: "#ffd60a" },
  { name: "Lime", hex: "#7fff00" },
  { name: "Green", hex: "#00ff46" },
  { name: "Cyan", hex: "#00ffc8" },
  { name: "Ice", hex: "#22d3ee" },
  { name: "Light blue", hex: "#007fc8" },
  { name: "Blue", hex: "#2962ff" },
  { name: "Violet", hex: "#7c4dff" },
  { name: "Magenta", hex: "#ff00c8" },
  { name: "Pink", hex: "#ff007f" },
  { name: "Flesh", hex: "#ff7f3f" },
  { name: "Bluish white", hex: "#3f7f7f" },
  { name: "White", hex: "#ffffff" },
  { name: "Off", hex: "#000000" },
];

const EFFECTS = [
  { id: "none", label: "Static" },
  { id: "random", label: "Random" },
  { id: "flash", label: "Flash" },
  { id: "tempo", label: "Tempo" },
  { id: "dance", label: "Dance" },
  { id: "wave", label: "Wave" },
  { id: "cycle", label: "Cycle" },
  { id: "breath", label: "Breath" },
];

const EFFECT_LABELS = Object.fromEntries(EFFECTS.map((effect) => [effect.id, effect.label]));

function findCli() {
  const candidates = [
    "/usr/local/bin/alg-rgb",
    GLib.find_program_in_path(CLI_NAME),
    "/usr/bin/alg-rgb",
  ].filter(Boolean);

  for (const candidate of candidates) {
    if (GLib.file_test(candidate, GLib.FileTest.IS_EXECUTABLE)) return candidate;
  }

  return null;
}

const Indicator = GObject.registerClass(
  class Indicator extends PanelMenu.Button {
    _init() {
      super._init(0.0, _("ALG RGB"));

      this._cli = findCli();
      this._state = null;
      this._syncing = false;
      this._destroyed = false;
      this._sliderTimer = 0;
      this._refreshTimer = 0;
      this._colorButtons = [];
      this._effectItems = new Map();

      this.add_child(
        new St.Icon({
          icon_name: "input-keyboard-symbolic",
          style_class: "system-status-icon",
        }),
      );

      this._buildMenu();
      this._refreshStatus();
    }

    _buildMenu() {
      const title = new PopupMenu.PopupMenuItem(_("ALG RGB"));
      title.setSensitive(false);
      this.menu.addMenuItem(title);

      this._statusLabel = new St.Label({
        text: this._cli ? _("Checking keyboard…") : _("alg-rgb CLI not found"),
        style_class: "alg-status",
      });
      const statusItem = new PopupMenu.PopupBaseMenuItem({
        reactive: false,
        can_focus: false,
      });
      statusItem.add_child(this._statusLabel);
      this.menu.addMenuItem(statusItem);

      this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
      this._buildColorGrid();
      this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
      this._buildBrightnessControl();
      this._buildEffectMenu();
      this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
      this._buildActions();

      if (!this._cli) {
        this._setError(_("Install alg-rgb to control the keyboard"));
      }
    }

    _buildColorGrid() {
      const item = new PopupMenu.PopupBaseMenuItem({
        reactive: false,
        can_focus: false,
      });
      const container = new St.BoxLayout({
        vertical: true,
        x_expand: true,
        style_class: "alg-color-grid",
      });

      COLORS.forEach((color, index) => {
        if (index % 4 === 0) {
          this._currentRow = new St.BoxLayout({
            x_expand: true,
            style_class: "alg-color-row",
          });
          container.add_child(this._currentRow);
        }

        const button = new St.Button({
          style_class: "alg-color-button",
          reactive: true,
          can_focus: true,
          track_hover: true,
        });
        button.set_style(`background-color: ${color.hex};`);
        button.accessible_name = color.name;
        button._algHex = color.hex;
        button.connect("clicked", () => this._applyColor(color.hex));
        this._colorButtons.push(button);
        this._currentRow.add_child(button);
      });

      item.add_child(container);
      this.menu.addMenuItem(item);
      this._currentRow = null;
    }

    _buildBrightnessControl() {
      const item = new PopupMenu.PopupBaseMenuItem({
        reactive: false,
        can_focus: false,
      });
      const box = new St.BoxLayout({
        vertical: true,
        x_expand: true,
        style_class: "alg-slider-box",
      });

      this._brightnessLabel = new St.Label({
        text: _("Brightness: 3"),
        style_class: "alg-slider-label",
      });
      box.add_child(this._brightnessLabel);

      this._slider = new Slider.Slider(3 / MAX_BRIGHTNESS);
      this._slider.style_class = "alg-slider";
      this._slider.connect("notify::value", () => this._onSliderChanged());
      box.add_child(this._slider);

      item.add_child(box);
      this.menu.addMenuItem(item);
    }

    _buildEffectMenu() {
      this._effectMenu = new PopupMenu.PopupSubMenuMenuItem(_("Effect: Static"));
      this.menu.addMenuItem(this._effectMenu);

      EFFECTS.forEach((effect) => {
        const item = new PopupMenu.PopupMenuItem(_(effect.label));
        item.connect("activate", () => this._applyEffect(effect.id));
        this._effectMenu.menu.addMenuItem(item);
        this._effectItems.set(effect.id, item);
      });
    }

    _buildActions() {
      this._powerItem = new PopupMenu.PopupMenuItem(_("Turn Off"));
      this._powerItem.connect("activate", () => this._togglePower());
      this.menu.addMenuItem(this._powerItem);

      const refreshItem = new PopupMenu.PopupMenuItem(_("Refresh status"));
      refreshItem.connect("activate", () => this._refreshStatus());
      this.menu.addMenuItem(refreshItem);

      const dashboardItem = new PopupMenu.PopupMenuItem(_("Open dashboard"));
      dashboardItem.connect("activate", () => this._openDashboard());
      this.menu.addMenuItem(dashboardItem);
    }

    _setStatus(text) {
      this._statusLabel.text = text;
      this._statusLabel.remove_style_class_name("alg-error");
    }

    _setError(text) {
      this._statusLabel.text = text.length > 72 ? `${text.slice(0, 69)}…` : text;
      this._statusLabel.add_style_class_name("alg-error");
    }

    _run(args, onSuccess) {
      if (this._destroyed) return;
      if (!this._cli) {
        this._setError(_("alg-rgb CLI not found"));
        return;
      }

      try {
        const proc = Gio.Subprocess.new(
          args,
          Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_PIPE,
        );

        proc.communicate_utf8_async(null, null, (subprocess, result) => {
          if (this._destroyed) return;
          try {
            const [ok, stdout, stderr] = subprocess.communicate_utf8_finish(result);
            if (!ok) {
              this._setError(stderr.trim() || _("Command failed"));
              return;
            }
            onSuccess(stdout.trim());
          } catch (error) {
            this._setError(error.message);
          }
        });
      } catch (error) {
        this._setError(error.message);
      }
    }

    _command(args, pendingText) {
      if (!this._cli) {
        this._setError(_("alg-rgb CLI not found"));
        return;
      }

      if (pendingText) this._setStatus(pendingText);
      this._run(args, () => {
        this._setStatus(_("Applied"));
        if (this._refreshTimer) {
          GLib.source_remove(this._refreshTimer);
          this._refreshTimer = 0;
        }
        this._refreshTimer = GLib.timeout_add(260, () => {
          this._refreshTimer = 0;
          this._refreshStatus();
          return GLib.SOURCE_REMOVE;
        });
      });
    }

    _refreshStatus() {
      if (this._destroyed || !this._cli) return;

      this._run([this._cli, "status", "--json"], (stdout) => {
        try {
          const data = JSON.parse(stdout);
          if (!data.ok) throw new Error(data.error || _("Keyboard unavailable"));
          this._applyState(data);
        } catch (error) {
          this._setError(error.message);
        }
      });
    }

    _applyState(data) {
      this._state = data;
      this._syncing = true;
      this._slider.value = Math.max(0, Math.min(1, data.brightness / MAX_BRIGHTNESS));
      this._syncing = false;

      this._brightnessLabel.text = _("Brightness: %d").format(data.brightness);
      this._powerItem.label.text = data.power === "on" ? _("Turn Off") : _("Turn On");

      const effect = data.effect || "none";
      if (this._effectMenu.label) {
        this._effectMenu.label.text = _("Effect: %s").format(EFFECT_LABELS[effect] || _("Static"));
      }

      this._selectColor(data.color);
      this._setStatus(
        _("%s · %s · %d/%d").format(
          data.power === "on" ? _("On") : _("Off"),
          String(data.color).toUpperCase(),
          data.brightness,
          MAX_BRIGHTNESS,
        ),
      );
    }

    _selectColor(hex) {
      const wanted = String(hex).toLowerCase();
      for (const button of this._colorButtons) {
        button.remove_style_class_name("selected");
        if (button._algHex.toLowerCase() === wanted) {
          button.add_style_class_name("selected");
        }
      }
    }

    _applyColor(hex) {
      this._selectColor(hex);
      this._command(
        [this._cli, "color", hex],
        _("Applying %s…").format(hex.toUpperCase()),
      );
    }

    _onSliderChanged() {
      if (this._syncing) return;

      const level = Math.round(this._slider.value * MAX_BRIGHTNESS);
      if (this._state && level === this._state.brightness) return;

      this._brightnessLabel.text = _("Brightness: %d").format(level);
      if (this._state) this._state.brightness = level;

      if (this._sliderTimer) GLib.source_remove(this._sliderTimer);
      this._sliderTimer = GLib.timeout_add(170, () => {
        this._sliderTimer = 0;
        this._command(
          [this._cli, "brightness", String(level)],
          _("Setting brightness…"),
        );
        return GLib.SOURCE_REMOVE;
      });
    }

    _applyEffect(effect) {
      if (this._effectMenu.label) {
        this._effectMenu.label.text = _("Effect: %s").format(
          EFFECT_LABELS[effect] || _("Static"),
        );
      }
      this._command(
        [this._cli, "effect", effect],
        _("Applying effect…"),
      );
    }

    _togglePower() {
      const on = this._state?.power !== "on";
      this._powerItem.label.text = on ? _("Turn Off") : _("Turn On");
      this._command(
        [this._cli, on ? "on" : "off"],
        on ? _("Turning on…") : _("Turning off…"),
      );
    }

    _openDashboard() {
      try {
        Gio.Subprocess.new(
          ["xdg-open", DASHBOARD_URL],
          Gio.SubprocessFlags.NONE,
        );
      } catch (error) {
        this._setError(error.message);
      }
    }

    destroy() {
      this._destroyed = true;
      if (this._sliderTimer) {
        GLib.source_remove(this._sliderTimer);
        this._sliderTimer = 0;
      }
      if (this._refreshTimer) {
        GLib.source_remove(this._refreshTimer);
        this._refreshTimer = 0;
      }
      console.log(`${LOG_PREFIX} Indicator destroyed`);
      super.destroy();
    }
  },
);

export default class AlgRgbExtension extends Extension {
  enable() {
    console.log(`${LOG_PREFIX} Enabled`);
    this._indicator = new Indicator();
    Main.panel.addToStatusArea(this.uuid, this._indicator);
  }

  disable() {
    console.log(`${LOG_PREFIX} Disabled`);
    this._indicator?.destroy();
    this._indicator = null;
  }
}
