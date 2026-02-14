package builder

import (
	"archive/tar"
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"

	"peacock/internal/manifest"
	"peacock/internal/runner"
)

// PackageArtifact creates a simple .pkg.tar.gz that pacman can install (pacman -U)
// specific to the prototype needs.
func (b *Builder) PackageArtifact(buildDir string, pkg *manifest.Package, arch string) (string, error) {
	pkgRoot := buildDir
	stageDir := filepath.Join(buildDir, "stage")
	if info, err := os.Stat(stageDir); err == nil && info.IsDir() {
		pkgRoot = stageDir
	}

	// Normalize arch for pacman compatibility.
	pacmanArch := arch
	if arch == "armv7" {
		pacmanArch = "armv7h"
	}

	// Create a .PKGINFO file (pkgver includes pkgrel, per pacman format).
	pkgrel := "1"
	pkgVer := fmt.Sprintf("%s-%s", pkg.Package.Version, pkgrel)
	pkgInfo := fmt.Sprintf(`pkgname = %s
pkgver = %s
pkgdesc = %s
url = verify
builddate = 0
packager = Peacock
size = 1000
arch = %s
license = GPL
`, pkg.Package.Name, pkgVer, pkg.Package.Description, pacmanArch)

	for _, p := range pkg.Package.Provides {
		pkgInfo += fmt.Sprintf("provides = %s\n", p)
	}
	for _, d := range pkg.Package.Depends {
		if d == "" {
			continue
		}
		pkgInfo += fmt.Sprintf("depend = %s\n", d)
	}

	pkgInfoPath := filepath.Join(pkgRoot, ".PKGINFO")
	tmpFile, err := os.CreateTemp("", "peacock-pkginfo-")
	if err != nil {
		return "", err
	}
	if _, writeErr := tmpFile.Write([]byte(pkgInfo)); writeErr != nil {
		tmpFile.Close()
		os.Remove(tmpFile.Name())
		return "", writeErr
	}
	tmpFile.Close()
	defer os.Remove(tmpFile.Name())
	cpCmd := exec.Command("sudo", "install", "-m", "0644", tmpFile.Name(), pkgInfoPath)
	cpCmd.Stdout = runner.LogWriter()
	cpCmd.Stderr = runner.LogWriter()
	if runErr := runner.RunCmd(cpCmd); runErr != nil {
		return "", runErr
	}

	// TarGz the package root
	tarPath := filepath.Join(b.CacheDir, fmt.Sprintf("%s-%s-%s-%s.pkg.tar.gz", pkg.Package.Name, pkg.Package.Version, pkgrel, pacmanArch))
	file, err := os.Create(tarPath)
	if err != nil {
		return "", err
	}
	defer file.Close()

	gw := gzip.NewWriter(file)
	defer gw.Close()
	tw := tar.NewWriter(gw)
	defer tw.Close()

	// Recursively add package contents
	err = filepath.Walk(pkgRoot, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}

		relPath, _ := filepath.Rel(pkgRoot, path)
		if relPath == "." {
			return nil
		}

		// Handle symlinks safely to avoid missing-target errors.
		if info.Mode()&os.ModeSymlink != 0 {
			linkTarget, err := os.Readlink(path)
			if err != nil {
				return err
			}
			header := &tar.Header{
				Name:     relPath,
				Typeflag: tar.TypeSymlink,
				Linkname: linkTarget,
				Mode:     int64(info.Mode().Perm()),
			}
			if err := tw.WriteHeader(header); err != nil {
				return err
			}
			return nil
		}

		// Create header
		header, err := tar.FileInfoHeader(info, info.Name())
		if err != nil {
			return err
		}

		header.Name = relPath

		if err := tw.WriteHeader(header); err != nil {
			return err
		}
		if !info.IsDir() {
			f, err := os.Open(path)
			if err != nil {
				return err
			}
			defer f.Close()
			io.Copy(tw, f)
		}
		return nil
	})

	return tarPath, err
}
