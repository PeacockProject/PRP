package manifest

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/pelletier/go-toml/v2"
)

// Device represents the device.toml structure
type Device struct {
	Device struct {
		Name         string `toml:"name"`
		Architecture string `toml:"architecture"`
		FlashMethod  string `toml:"flash_method"`
	} `toml:"device"`

	Quirks struct {
		KeepFbRefresherWithDM bool `toml:"keep_fb_refresher_with_dm"`
		XorgForceVT1          bool `toml:"xorg_force_vt1"`
	} `toml:"quirks"`

	Boot struct {
		GenerateBootImg bool   `toml:"generate_bootimg"`
		Cmdline         string `toml:"cmdline"`
		Android         struct {
			PageSize      int    `toml:"page_size"`
			Base          string `toml:"base"`
			KernelOffset  string `toml:"kernel_offset"`
			RamdiskOffset string `toml:"ramdisk_offset"`
			SecondOffset  string `toml:"second_offset"`
			TagsOffset    string `toml:"tags_offset"`
		} `toml:"android"`
	} `toml:"boot"`
}

// Package represents a package.toml metdata
type Package struct {
	Package struct {
		Name        string   `toml:"name"`
		Version     string   `toml:"version"`
		Description string   `toml:"description"`
		Provides    []string `toml:"provides"`
		Depends     []string `toml:"depends"`
	} `toml:"package"`

	Build struct {
		Dependencies        []string `toml:"dependencies"`
		DependenciesOpenRC  []string `toml:"dependencies_openrc"`
		DependenciesSystemd []string `toml:"dependencies_systemd"`
		BuildDeps           []string `toml:"build_deps"`
		BuildDepPackages    []string `toml:"build_dep_packages"`
		UseQemu             *bool    `toml:"use_qemu"`
		CrossCompile        string   `toml:"cross_compile"`
		Script              string   `toml:"script"`
		Source              string   `toml:"source"`
		Checksum            string   `toml:"checksum"`
	} `toml:"build"`

	ManifestDir string // Directory containing package.toml, not loaded from TOML
}

// LoadPackage loads a package.toml
func LoadPackage(path string) (*Package, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("failed to read package manifest: %w", err)
	}

	var pkg Package
	if err := toml.Unmarshal(data, &pkg); err != nil {
		return nil, fmt.Errorf("failed to parse package manifest: %w", err)
	}

	pkg.ManifestDir = filepath.Dir(path)
	return &pkg, nil
}

// LoadDevice loads a device.toml
func LoadDevice(path string) (*Device, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("failed to read device manifest: %w", err)
	}

	var dev Device
	if err := toml.Unmarshal(data, &dev); err != nil {
		return nil, fmt.Errorf("failed to parse device manifest: %w", err)
	}

	return &dev, nil
}
