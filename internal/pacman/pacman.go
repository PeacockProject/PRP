package pacman

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"

	"peacock/internal/runner"
	"strings"
)

// GenerateConfig generates a basic pacman.conf for the target
func GenerateConfigContent(arch string) string {
	// Simple pacman.conf template
	// We use standard Arch Linux ARM mirrors for ARM builds
	if arch == "armv7" {
		arch = "armv7h"
	}

	conf := `
[options]
HoldPkg     = pacman glibc
Architecture = %s
CheckSpace
SigLevel    = Never

[core]
Server = http://mirror.archlinuxarm.org/$arch/$repo

[extra]
Server = http://mirror.archlinuxarm.org/$arch/$repo

[alarm]
Server = http://mirror.archlinuxarm.org/$arch/$repo

[aur]
Server = http://mirror.archlinuxarm.org/$arch/$repo
`
	// Adjust for x86_64
	if arch == "x86_64" {
		conf = `
[options]
HoldPkg     = pacman glibc
Architecture = auto
CheckSpace
SigLevel    = DatabaseOptional
LocalFileSigLevel = Never

[core]
Include = /etc/pacman.d/mirrorlist

[extra]
Include = /etc/pacman.d/mirrorlist
`
	}

	if arch != "x86_64" {
		conf = fmt.Sprintf(conf, arch)
	}
	return conf
}

func GenerateConfig(target string, arch string) error {
	conf := GenerateConfigContent(arch)

	confPath := filepath.Join(target, "etc", "pacman.conf")
	if err := os.MkdirAll(filepath.Dir(confPath), 0755); err != nil {
		return err
	}

	return os.WriteFile(confPath, []byte(conf), 0644)
}

// Install installs packages into the target root
// Install installs packages into the target root
// If execRoot is non-empty, the command is executed inside that chroot.
// NOTE: When using execRoot, verify that target and configFile paths are valid INSIDE that chroot.
func Install(target string, configFile string, packages []string, cacheDir string, skipScripts bool, execRoot string) error {
	if len(packages) == 0 {
		return nil
	}

	// Ensure target exists (on host, so we can mount into it if needed)
	if execRoot == "" {
		if err := os.MkdirAll(target, 0755); err != nil {
			return err
		}
	}

	// Create Pacman DB and Cache directories (on host)
	// If execRoot is used, these paths refer to host paths that must be mapped or exist inside.
	// For now, we assume simple host usage or caller-managed mounts.
	// But actually, if we are running INSIDE execRoot, we can't easily mkdir from host unless paths match.
	// We'll skip mkdirs if execRoot is set, assuming the environment is prepared or pacman handles it.
	if execRoot == "" {
		dbPath := filepath.Join(target, "var", "lib", "pacman")
		cachePath := filepath.Join(target, "var", "cache", "pacman", "pkg")
		if err := os.MkdirAll(dbPath, 0755); err != nil {
			return err
		}
		if err := os.MkdirAll(cachePath, 0755); err != nil {
			return err
		}
	}

	args := []string{
		"-r", target,
		"--noconfirm",
		"--config", configFile,
		"--overwrite", "*",
		"-Sy",
	}
	if skipScripts {
		args = append(args, "--noscriptlet")
	}
	if cacheDir != "" {
		args = append(args, "--cachedir", cacheDir)
	}
	args = append(args, packages...)

	var cmd *exec.Cmd
	if execRoot != "" {
		// Run inside chroot
		// sudo chroot execRoot pacman ...
		chrootArgs := append([]string{"chroot", execRoot, "pacman"}, args...)
		cmd = exec.Command("sudo", chrootArgs...)
	} else {
		cmd = exec.Command("sudo", append([]string{"pacman"}, args...)...)
	}

	cmd.Stdin = os.Stdin
	cmd.Stdout = runner.LogWriter()
	cmd.Stderr = runner.LogWriter()

	return runner.RunCmd(cmd)
}

// InstallLocal installs local package files (pacman -U)
func InstallLocal(target string, configFile string, packageFiles []string, cacheDir string, skipScripts bool, execRoot string) error {
	if len(packageFiles) == 0 {
		return nil
	}

	args := []string{
		"-r", target,
		"--noconfirm",
		"--config", configFile,
		"-U",
	}
	if skipScripts {
		args = append(args, "--noscriptlet")
	}
	if cacheDir != "" {
		args = append(args, "--cachedir", cacheDir)
	}
	args = append(args, packageFiles...)

	var cmd *exec.Cmd
	if execRoot != "" {
		chrootArgs := append([]string{"chroot", execRoot, "pacman"}, args...)
		cmd = exec.Command("sudo", chrootArgs...)
	} else {
		cmd = exec.Command("sudo", append([]string{"pacman"}, args...)...)
	}

	cmd.Stdin = os.Stdin
	cmd.Stdout = runner.LogWriter()
	cmd.Stderr = runner.LogWriter()

	return runner.RunCmd(cmd)
}

// SanitizeConfig reads a config, filters DownloadUser, and rewrites Include paths to be absolute
// if they are inside the chroot.
func SanitizeConfig(confPath string) (string, func(), error) {
	confData, err := os.ReadFile(confPath)
	if err != nil {
		return "", nil, fmt.Errorf("failed to read pacman.conf: %w", err)
	}

	rootDir := filepath.Dir(filepath.Dir(confPath)) // assuming conf is in .../etc/pacman.conf

	var confLines []string
	for _, line := range strings.Split(string(confData), "\n") {
		trimmed := strings.TrimSpace(line)
		if strings.HasPrefix(trimmed, "DownloadUser") {
			confLines = append(confLines, "#"+line)
			continue
		}
		if strings.HasPrefix(trimmed, "Include") {
			parts := strings.SplitN(trimmed, "=", 2)
			if len(parts) == 2 {
				path := strings.TrimSpace(parts[1])
				// If path starts with /etc, prepend rootDir to verify if it exists there
				if strings.HasPrefix(path, "/etc") {
					absPath := filepath.Join(rootDir, strings.TrimPrefix(path, "/"))
					if _, err := os.Stat(absPath); err == nil {
						confLines = append(confLines, fmt.Sprintf("Include = %s", absPath))
						continue
					}
				}
			}
		}
		confLines = append(confLines, line)
	}

	f, err := os.CreateTemp("", "peacock-pacman-*.conf")
	if err != nil {
		return "", nil, fmt.Errorf("failed to create temp config: %w", err)
	}
	tmpConf := f.Name()
	cleanup := func() {
		os.Remove(tmpConf)
	}

	if _, err := f.WriteString(strings.Join(confLines, "\n")); err != nil {
		f.Close()
		cleanup()
		return "", nil, err
	}
	f.Close()
	return tmpConf, cleanup, nil
}

// Bootstrap initializes pacman in the root (keys, config, sync) and installs packages
func Bootstrap(root string, packages []string) error {
	confPath := filepath.Join(root, "etc", "pacman.conf")
	startConf, cleanup, err := SanitizeConfig(confPath)
	if err != nil {
		return err
	}
	defer cleanup()

	// Create cache dirs
	if err := os.MkdirAll(filepath.Join(root, "var", "cache", "pacman", "pkg"), 0755); err != nil {
		return err
	}

	// We ignore key errors (e.g. archlinuxarm keyring missing on x86) since simple bootstrap might not need it
	// or user can fix keyrings manually.
	runner.RunCmd(exec.Command("sudo", "pacman-key", "--gpgdir", filepath.Join(root, "etc", "pacman.d", "gnupg"), "--init"))
	runner.RunCmd(exec.Command("sudo", "pacman-key", "--gpgdir", filepath.Join(root, "etc", "pacman.d", "gnupg"), "--populate", "archlinux", "archlinuxarm"))

	// Install using temp config
	return Install(root, startConf, packages, "", false, "")
}
