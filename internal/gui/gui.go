package gui

import (
	"fmt"
	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/widget"
)

// Start launches the Peacock GUI
func Start() {
	a := app.New()
	w := a.NewWindow("Peacock Distro Builder")

	// Device Selector
	// Mock devices for now or list from dir
	devices := []string{"samsung-i9500", "generic-x86_64"}
	combo := widget.NewSelect(devices, func(value string) {
		fmt.Println("Selected device:", value)
	})
	combo.PlaceHolder = "Select Device"

	// Init System Selector
	initSystems := []string{"systemd", "openrc", "runit"}
	initRadio := widget.NewRadioGroup(initSystems, func(value string) {
		fmt.Println("Selected init:", value)
	})
	initRadio.Selected = "systemd"

	// Extra packages
	extraPkgs := widget.NewEntry()
	extraPkgs.SetPlaceHolder("Extra packages (comma-separated)")

	// Userland selectors
	desktops := []string{"none", "xfce", "lxqt", "mate", "gnome", "plasma", "cinnamon"}
	desktopCombo := widget.NewSelect(desktops, func(value string) {
		fmt.Println("Selected desktop:", value)
	})
	desktopCombo.PlaceHolder = "Select Desktop"
	desktopCombo.SetSelected("none")

	dms := []string{"none", "lightdm", "greetd", "sddm", "gdm", "ly"}
	dmCombo := widget.NewSelect(dms, func(value string) {
		fmt.Println("Selected display manager:", value)
	})
	dmCombo.PlaceHolder = "Select Display Manager"
	dmCombo.SetSelected("none")

	// User account
	usernameEntry := widget.NewEntry()
	usernameEntry.SetPlaceHolder("Username")
	passwordEntry := widget.NewPasswordEntry()
	passwordEntry.SetPlaceHolder("Password")

	// Log Area
	logArea := widget.NewMultiLineEntry()
	logArea.SetText("Peacock initialized.\nReady to build.")
	logArea.Rows = 10
    logArea.Disable() // Read-only

	// Build Button
	buildBtn := widget.NewButton("Build Image", func() {
		logArea.SetText(logArea.Text + "\nStarting build for " + combo.Selected + "...")
		// TODO: Hook into actual build logic
	})

	content := container.NewVBox(
		widget.NewLabel("Peacock Configuration"),
		combo,
		widget.NewLabel("Extra Packages"),
		extraPkgs,
		widget.NewLabel("Userland (Desktop + Display Manager)"),
		widget.NewLabel("Note: GNOME/Plasma/Cinnamon typically require 3D acceleration."),
		desktopCombo,
		dmCombo,
		widget.NewLabel("Init System"),
		initRadio,
		widget.NewLabel("User Account"),
		usernameEntry,
		passwordEntry,
		buildBtn,
		widget.NewLabel("Build Logs"),
		logArea,
	)

	w.SetContent(content)
	w.Resize(fyne.NewSize(600, 400))
	w.ShowAndRun()
}
