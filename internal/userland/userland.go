package userland

import (
	"fmt"
	"sort"
	"strings"
)

type Choice struct {
	Name        string
	Description string
	Packages    []string
	Requires3D  bool
	SystemdOnly bool
}

type OpenRCService struct {
	Name     string
	Runlevel string
}

var desktopChoices = map[string]Choice{
	"none":   {Name: "none", Description: "No desktop environment", Packages: nil},
	"xfce":   {Name: "xfce", Description: "XFCE (lightweight, good without 3D)", Packages: []string{"xfce4", "xfce4-goodies"}},
	"lxqt":   {Name: "lxqt", Description: "LXQt (lightweight, Qt)", Packages: []string{"lxqt"}},
	"mate":   {Name: "mate", Description: "MATE (traditional, moderate)", Packages: []string{"mate"}},
	"gnome":  {Name: "gnome", Description: "GNOME (heavy, needs 3D)", Packages: []string{"gnome"}, Requires3D: true, SystemdOnly: true},
	"plasma": {Name: "plasma", Description: "KDE Plasma (heavy, needs 3D)", Packages: []string{"plasma-desktop"}, Requires3D: true},
	"cinnamon": {
		Name:        "cinnamon",
		Description: "Cinnamon (heavy, needs 3D)",
		Packages:    []string{"cinnamon"},
		Requires3D:  true,
	},
}

var displayManagerChoices = map[string]Choice{
	"none":    {Name: "none", Description: "No display manager", Packages: nil},
	"lightdm": {Name: "lightdm", Description: "LightDM (lightweight)", Packages: []string{"lightdm", "lightdm-gtk-greeter"}},
	"greetd":  {Name: "greetd", Description: "greetd + tuigreet (simple TUI)", Packages: []string{"greetd", "greetd-tuigreet"}},
	"sddm":    {Name: "sddm", Description: "SDDM (Qt)", Packages: []string{"sddm"}},
	"gdm":     {Name: "gdm", Description: "GDM (GNOME, systemd)", Packages: []string{"gdm"}, Requires3D: true, SystemdOnly: true},
	"ly":      {Name: "ly", Description: "ly (console DM)", Packages: []string{"ly"}},
}

var baseDesktopPackages = []string{
	"xorg-server",
	"xorg-xinit",
	"xorg-xrandr",
	"xorg-xinput",
	"mesa",
	"libinput",
	"xf86-input-libinput",
	"xf86-video-fbdev",
	"ttf-dejavu",
	"noto-fonts",
}

func ResolveSelections(desktop, dm, initSystem string, extra []string) ([]string, []string, error) {
	d := strings.ToLower(strings.TrimSpace(desktop))
	m := strings.ToLower(strings.TrimSpace(dm))

	desktopChoice, ok := desktopChoices[d]
	if !ok {
		return nil, nil, fmt.Errorf("unknown desktop '%s'", desktop)
	}
	dmChoice, ok := displayManagerChoices[m]
	if !ok {
		return nil, nil, fmt.Errorf("unknown display manager '%s'", dm)
	}

	var pkgs []string
	var warnings []string

	if desktopChoice.SystemdOnly && initSystem != "systemd" {
		warnings = append(warnings, fmt.Sprintf("desktop '%s' is typically systemd-only", d))
	}
	if dmChoice.SystemdOnly && initSystem != "systemd" {
		warnings = append(warnings, fmt.Sprintf("display manager '%s' is typically systemd-only", m))
	}
	if desktopChoice.Requires3D {
		warnings = append(warnings, fmt.Sprintf("desktop '%s' needs 3D acceleration", d))
	}
	if dmChoice.Requires3D {
		warnings = append(warnings, fmt.Sprintf("display manager '%s' needs 3D acceleration", m))
	}

	if desktopChoice.Name != "none" || dmChoice.Name != "none" {
		pkgs = append(pkgs, baseDesktopPackages...)
	}
	pkgs = append(pkgs, desktopChoice.Packages...)

	if dmChoice.Name == "sddm" {
		pkgs = append(pkgs, dmChoice.Packages...)
		pkgs = append(pkgs, "peacock-sddm-theme-peacock-phone")
		// Our local sddm build currently ships a Qt6 daemon plus Qt5 greeter
		// artifacts, so both runtime stacks are required.
		pkgs = append(pkgs, "qt5-base", "qt5-declarative", "qt6-base", "qt6-declarative")
		// Enable touch keyboard support in SDDM regardless of Qt major at runtime.
		pkgs = append(pkgs, "qt5-virtualkeyboard", "qt6-virtualkeyboard")
		if initSystem == "openrc" {
			pkgs = append(pkgs, "sddm-openrc")
			pkgs = append(pkgs, "dbus", "dbus-openrc", "elogind", "elogind-openrc")
			warnings = append(warnings, "sddm-openrc sourced from local Artix init script")
		}
	} else if dmChoice.Name == "lightdm" && initSystem == "openrc" {
		pkgs = append(pkgs, dmChoice.Packages...)
		pkgs = append(pkgs, "dbus", "dbus-openrc", "elogind", "elogind-openrc")
	} else {
		pkgs = append(pkgs, dmChoice.Packages...)
	}

	if len(extra) > 0 {
		pkgs = append(pkgs, extra...)
	}

	return unique(pkgs), warnings, nil
}

func DescribeChoices() string {
	var desktops []string
	for k := range desktopChoices {
		desktops = append(desktops, k)
	}
	sort.Strings(desktops)

	var dms []string
	for k := range displayManagerChoices {
		dms = append(dms, k)
	}
	sort.Strings(dms)

	var b strings.Builder
	b.WriteString("Desktop choices:\n")
	for _, k := range desktops {
		c := desktopChoices[k]
		flags := choiceFlags(c)
		b.WriteString(fmt.Sprintf("  - %s: %s%s\n", c.Name, c.Description, flags))
	}
	b.WriteString("Display manager choices:\n")
	for _, k := range dms {
		c := displayManagerChoices[k]
		flags := choiceFlags(c)
		b.WriteString(fmt.Sprintf("  - %s: %s%s\n", c.Name, c.Description, flags))
	}
	return b.String()
}

func DesktopNames() []string {
	var out []string
	for k := range desktopChoices {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

func DisplayManagerNames() []string {
	var out []string
	for k := range displayManagerChoices {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

func DisplayManagerService(name string) string {
	switch strings.ToLower(strings.TrimSpace(name)) {
	case "lightdm":
		return "lightdm"
	case "sddm":
		return "sddm"
	case "gdm":
		return "gdm"
	case "greetd":
		return "greetd"
	case "ly":
		return "ly"
	default:
		return ""
	}
}

func DisplayManagerOpenRCServices(name string, initSystem string) []OpenRCService {
	if strings.ToLower(strings.TrimSpace(initSystem)) != "openrc" {
		return nil
	}

	switch strings.ToLower(strings.TrimSpace(name)) {
	case "sddm", "lightdm", "gdm":
		return []OpenRCService{
			{Name: "dbus", Runlevel: "default"},
			{Name: "elogind", Runlevel: "boot"},
		}
	default:
		return nil
	}
}

func choiceFlags(c Choice) string {
	var flags []string
	if c.Requires3D {
		flags = append(flags, "needs-3d")
	}
	if c.SystemdOnly {
		flags = append(flags, "systemd-only")
	}
	if len(flags) == 0 {
		return ""
	}
	return " [" + strings.Join(flags, ", ") + "]"
}

func unique(in []string) []string {
	seen := make(map[string]struct{}, len(in))
	out := make([]string, 0, len(in))
	for _, v := range in {
		v = strings.TrimSpace(v)
		if v == "" {
			continue
		}
		if _, ok := seen[v]; ok {
			continue
		}
		seen[v] = struct{}{}
		out = append(out, v)
	}
	return out
}
