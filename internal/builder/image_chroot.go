package builder

import (
	"archive/tar"
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"peacock/internal/chroot"
	"peacock/internal/pacman"
	"peacock/internal/runner"
)

// EnsureImageBuildChroot sets up the image-build-chroot environment.
// This is a separate x86_64 chroot used exclusively for assembling disk images.
// It contains QEMU for installing ARM packages and image tools (parted, e2fsprogs, etc.)
func (b *Builder) EnsureImageBuildChroot() (string, error) {
	root := filepath.Join(filepath.Dir(b.CacheDir), "image-build-chroot", "x86_64")

	// Check if already exists
	if _, err := os.Stat(filepath.Join(root, "etc", "arch-release")); err == nil {
		fmt.Println("Image build chroot already exists, skipping bootstrap")
		// Ensure special filesystems are mounted
		if err := chroot.MountWithSudo(root); err != nil {
			fmt.Printf("Warning: failed to mount chroot filesystems: %v\n", err)
		}
		return root, nil
	}

	fmt.Println("Setting up image build chroot...")

	// Create directory
	if err := os.MkdirAll(root, 0755); err != nil {
		return "", fmt.Errorf("failed to create image chroot dir: %w", err)
	}

	// Bootstrap base system
	if err := b.bootstrapImageChroot(root); err != nil {
		return "", fmt.Errorf("failed to bootstrap image chroot: %w", err)
	}

	// Mount special filesystems
	if err := chroot.MountWithSudo(root); err != nil {
		return "", fmt.Errorf("failed to mount special filesystems: %w", err)
	}

	// Install image-building tools
	if err := b.installImageTools(root); err != nil {
		chroot.UnmountWithSudo(root)
		return "", fmt.Errorf("failed to install image tools: %w", err)
	}

	fmt.Println("Image build chroot ready")
	return root, nil
}

// bootstrapImageChroot downloads and extracts the Arch Linux bootstrap tarball
func (b *Builder) bootstrapImageChroot(root string) error {
	fmt.Println("Bootstrapping image build chroot...")

	// Use cached bootstrap if available
	bootstrapTarball := filepath.Join(b.CacheDir, "archlinux-bootstrap-x86_64.tar.zst")
	if _, err := os.Stat(bootstrapTarball); os.IsNotExist(err) {
		return fmt.Errorf("bootstrap tarball not found in cache: %s (should have been downloaded during build chroot setup)", bootstrapTarball)
	}

	// Extract bootstrap
	extractCmd := exec.Command("sudo", "tar", "-xf", bootstrapTarball, "-C", root, "--strip-components=1")
	extractCmd.Stdout = runner.LogWriter()
	extractCmd.Stderr = runner.LogWriter()
	if err := runner.RunCmd(extractCmd); err != nil {
		return fmt.Errorf("failed to extract bootstrap: %w", err)
	}

	// Initialize pacman keyring
	fmt.Println("Initializing pacman keyring in image chroot...")
	keyringCmd := exec.Command("sudo", "chroot", root, "pacman-key", "--init")
	keyringCmd.Stdout = runner.LogWriter()
	keyringCmd.Stderr = runner.LogWriter()
	if err := runner.RunCmd(keyringCmd); err != nil {
		return fmt.Errorf("failed to init keyring: %w", err)
	}

	populateCmd := exec.Command("sudo", "chroot", root, "pacman-key", "--populate", "archlinux")
	populateCmd.Stdout = runner.LogWriter()
	populateCmd.Stderr = runner.LogWriter()
	if err := runner.RunCmd(populateCmd); err != nil {
		return fmt.Errorf("failed to populate keyring: %w", err)
	}

	// Enable mirrors
	mirrorlist := filepath.Join(root, "etc", "pacman.d", "mirrorlist")
	sedCmd := exec.Command("sudo", "sed", "-i", "s/^#Server/Server/", mirrorlist)
	if err := runner.RunCmd(sedCmd); err != nil {
		return fmt.Errorf("failed to enable mirrors: %w", err)
	}

	// Copy resolv.conf
	if _, err := os.Stat("/etc/resolv.conf"); err == nil {
		_ = copyFileWithSudo("/etc/resolv.conf", filepath.Join(root, "etc", "resolv.conf"))
	}

	return nil
}

// installImageTools installs QEMU and image-building utilities
func (b *Builder) installImageTools(root string) error {
	fmt.Println("Installing image-building tools...")

	packages := []string{
		"qemu-user-static",
		"qemu-user-static-binfmt",
		"parted",               // Disk partitioning
		"e2fsprogs",            // ext4 filesystem (root partition)
		"arch-install-scripts", // For genfstab, arch-chroot
	}

	args := append([]string{"chroot", root, "pacman", "-Sy", "--noconfirm"}, packages...)
	installCmd := exec.Command("sudo", args...)
	installCmd.Stdout = runner.LogWriter()
	installCmd.Stderr = runner.LogWriter()
	if err := runner.RunCmd(installCmd); err != nil {
		return fmt.Errorf("failed to install packages: %w", err)
	}

	// Register binfmt handlers (same as build chroot)
	if err := b.registerBinfmtInChroot(root); err != nil {
		fmt.Printf("Warning: binfmt registration failed: %v\n", err)
	}

	return nil
}

// registerBinfmtInChroot reads QEMU binfmt config files and registers them
func (b *Builder) registerBinfmtInChroot(root string) error {
	fmt.Println("Registering QEMU binfmt handlers...")
	binfmtDir := filepath.Join(root, "usr", "lib", "binfmt.d")
	entries, err := os.ReadDir(binfmtDir)
	if err != nil {
		return fmt.Errorf("could not read binfmt.d directory: %w", err)
	}

	for _, entry := range entries {
		if !strings.HasPrefix(entry.Name(), "qemu-") || !strings.HasSuffix(entry.Name(), ".conf") {
			continue
		}
		confPath := filepath.Join(binfmtDir, entry.Name())
		fmt.Printf("Processing %s...\n", entry.Name())

		data, err := os.ReadFile(confPath)
		if err != nil {
			fmt.Printf("Warning: could not read %s: %v\n", entry.Name(), err)
			continue
		}

		// Process each line (conf files contain one registration string per line)
		lines := strings.Split(string(data), "\n")
		for _, line := range lines {
			line = strings.TrimSpace(line)
			// Skip empty lines and comments
			if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, ";") {
				continue
			}

			// Each line should be a binfmt_misc registration string
			if !strings.HasPrefix(line, ":") {
				continue
			}

			// Parse and modify interpreter path to use absolute path to our static QEMU
			parts := strings.Split(line, ":")
			if len(parts) >= 7 {
				interpreterName := filepath.Base(parts[6])
				absStaticPath := filepath.Join(root, "usr", "bin", interpreterName)

				if _, err := os.Stat(absStaticPath); err == nil {
					parts[6] = absStaticPath
				} else {
					parts[6] = strings.Replace(parts[6], "-static", "", 1)
				}

				// Ensure the F flag is present
				if len(parts) >= 8 {
					if !strings.Contains(parts[7], "F") {
						parts[7] = parts[7] + "F"
					}
				} else {
					parts = append(parts, "F")
				}

				line = strings.Join(parts, ":")
			}

			// Write to /proc/sys/fs/binfmt_misc/register
			registerCmd := exec.Command("sudo", "sh", "-c", fmt.Sprintf("echo '%s' > /proc/sys/fs/binfmt_misc/register", line))
			registerCmd.Stdout = runner.LogWriter()
			registerCmd.Stderr = runner.LogWriter()
			if err := runner.RunCmd(registerCmd); err != nil {
				// Ignore errors - entry might already exist
				if len(parts) > 1 {
					fmt.Printf("Note: %s already registered or error\n", parts[1])
				}
			} else {
				if len(parts) > 6 {
					fmt.Printf("Registered: %s (using %s)\n", parts[1], parts[6])
				}
			}
		}
	}

	return nil
}

// InstallPackagesToRootfs installs ARM packages from cache into a rootfs directory
// This happens inside the image-build-chroot
func (b *Builder) InstallPackagesToRootfs(imageChrootRoot, rootfsPath string, packages []string, arch string) error {
	// De-duplicate to avoid duplicate targets in pacman.
	packages = uniqueStrings(packages)
	fmt.Printf("Installing %d packages to rootfs...\n", len(packages))

	// Ensure rootfs directory exists on host
	if err := runner.RunCmd(exec.Command("sudo", "mkdir", "-p", rootfsPath)); err != nil {
		return fmt.Errorf("failed to create rootfs dir: %w", err)
	}

	// Create var/lib/pacman inside rootfs to satisfy alpm
	dbPath := filepath.Join(rootfsPath, "var", "lib", "pacman")
	if err := runner.RunCmd(exec.Command("sudo", "mkdir", "-p", dbPath)); err != nil {
		return fmt.Errorf("failed to create pacman db dir: %w", err)
	}

	// Prepare mount points inside the chroot
	internalRootfs := "/mnt/rootfs"
	internalCache := "/mnt/cache"
	hostRootfsMount := filepath.Join(imageChrootRoot, "mnt", "rootfs")
	hostCacheMount := filepath.Join(imageChrootRoot, "mnt", "cache")

	// Create mount points
	if err := runner.RunCmd(exec.Command("sudo", "mkdir", "-p", hostRootfsMount)); err != nil {
		return fmt.Errorf("failed to create rootfs mount point: %w", err)
	}
	if err := runner.RunCmd(exec.Command("sudo", "mkdir", "-p", hostCacheMount)); err != nil {
		return fmt.Errorf("failed to create cache mount point: %w", err)
	}

	// Bind mount rootfs
	if !isMountPoint(hostRootfsMount) {
		if err := runner.RunCmd(exec.Command("sudo", "mount", "--bind", rootfsPath, hostRootfsMount)); err != nil {
			return fmt.Errorf("failed to bind mount rootfs: %w", err)
		}
	}
	defer chroot.UnmountPathWithSudo(hostRootfsMount)

	// Bind mount cache
	if !isMountPoint(hostCacheMount) {
		if err := runner.RunCmd(exec.Command("sudo", "mount", "--bind", b.CacheDir, hostCacheMount)); err != nil {
			return fmt.Errorf("failed to bind mount cache: %w", err)
		}
	}
	defer chroot.UnmountPathWithSudo(hostCacheMount)

	// Create pacman.conf for rootfs installation
	confContent := pacman.GenerateConfigContent(arch)

	// We write the config to a file accessible inside the chroot
	confPath := filepath.Join(imageChrootRoot, "tmp", "rootfs-pacman.conf")
	if err := os.WriteFile(confPath, []byte(confContent), 0644); err != nil {
		return fmt.Errorf("failed to write pacman config: %w", err)
	}

	// Identify local vs repo packages
	var repoPkgs []string
	var localPkgs []string

	fmt.Println("Identifying package locations...")
	for _, pkg := range packages {
		// Check if package file exists in host CacheDir
		pattern := filepath.Join(b.CacheDir, pkg+"-*.pkg.tar.gz")
		matches, err := filepath.Glob(pattern)
		if err != nil {
			fmt.Printf("Glob error for %s: %v\n", pkg, err)
		}

		var selected string
		var selectedMtime time.Time
		if len(matches) > 0 {
			// Filter matches to avoid architecture mismatch (e.g. picking x86_64 bash when building for arm)
			for _, m := range matches {
				base := filepath.Base(m)
				// Skip x86_64 artifacts if we are not targeting x86_64
				if arch != "x86_64" && strings.Contains(base, "x86_64") {
					continue
				}
				if !packageNameMatches(m, pkg) {
					continue
				}

				// Prefer the newest artifact by mtime. Lexical version sorting is wrong for
				// dotted versions (e.g. 1.0.12 sorts before 1.0.6), and the build pipeline
				// may leave old artifacts in the cache dir.
				fi, err := os.Stat(m)
				if err != nil {
					continue
				}
				if selected == "" || fi.ModTime().After(selectedMtime) {
					selected = m
					selectedMtime = fi.ModTime()
				}
			}
		}

		if selected != "" {
			// Found local file
			filename := filepath.Base(selected)
			fmt.Printf(" [LOCAL] Found %s -> %s\n", pkg, filename)
			localPkgs = append(localPkgs, filepath.Join(internalCache, filename))
		} else {
			fmt.Printf(" [REPO]  Assume %s is in repo (no local file matched pattern: %s)\n", pkg, pattern)
			repoPkgs = append(repoPkgs, pkg)
		}
	}
	repoPkgs = uniqueStrings(repoPkgs)
	localPkgs = uniqueStrings(localPkgs)

	// Mount proc/sys/dev to rootfs so hooks work
	// effectively chrooting into rootfs requires these
	targetProc := filepath.Join(rootfsPath, "proc")
	targetSys := filepath.Join(rootfsPath, "sys")
	targetDev := filepath.Join(rootfsPath, "dev")
	if err := runner.RunCmd(exec.Command("sudo", "mkdir", "-p", targetProc, targetSys, targetDev)); err != nil {
		return fmt.Errorf("failed to create pseudo-fs mount points: %w", err)
	}

	if !isMountPoint(targetProc) {
		if err := runner.RunCmd(exec.Command("sudo", "mount", "-t", "proc", "proc", targetProc)); err != nil {
			return fmt.Errorf("failed to mount proc in rootfs: %w", err)
		}
	}
	defer chroot.UnmountPathWithSudo(targetProc)

	if !isMountPoint(targetSys) {
		if err := runner.RunCmd(exec.Command("sudo", "mount", "-t", "sysfs", "sys", targetSys)); err != nil {
			return fmt.Errorf("failed to mount sysfs in rootfs: %w", err)
		}
	}
	defer chroot.UnmountPathWithSudo(targetSys)

	if !isMountPoint(targetDev) {
		if err := runner.RunCmd(exec.Command("sudo", "mount", "--bind", "/dev", targetDev)); err != nil {
			return fmt.Errorf("failed to bind mount /dev in rootfs: %w", err)
		}
		// Fully isolate rootfs /dev from host propagation.
		if err := runner.RunCmd(exec.Command("sudo", "mount", "--make-rprivate", targetDev)); err != nil {
			return fmt.Errorf("failed to mark rootfs /dev mount rprivate: %w", err)
		}
	}
	defer chroot.UnmountPathWithSudo(targetDev)

	commonArgs := []string{
		"chroot", imageChrootRoot,
		"pacman", "-r", internalRootfs,
		"--noconfirm",
		"--ask=4",
		"--needed",
		"--config", "/tmp/rootfs-pacman.conf",
		"--cachedir", internalCache,
	}

	// 1. Synchronize databases
	syncArgs := append(commonArgs, "-Sy")
	if err := runner.RunCmd(exec.Command("sudo", syncArgs...)); err != nil {
		return fmt.Errorf("failed to sync pacman db: %w", err)
	}

	// 2. Install Repo Packages
	if len(repoPkgs) > 0 {
		installArgs := append(commonArgs, "-S")
		installArgs = append(installArgs, repoPkgs...)
		cmd := exec.Command("sudo", installArgs...)
		cmd.Stdout = runner.LogWriter()
		cmd.Stderr = runner.LogWriter()
		if err := runner.RunCmd(cmd); err != nil {
			return fmt.Errorf("failed to install repo packages: %w", err)
		}
	}

	// 3. Install Local Packages (using -U)
	if len(localPkgs) > 0 {
		// Use -U for local files
		installArgs := append(commonArgs, "-U", "--overwrite", "*")
		installArgs = append(installArgs, localPkgs...)
		cmd := exec.Command("sudo", installArgs...)
		cmd.Stdout = runner.LogWriter()
		cmd.Stderr = runner.LogWriter()
		if err := runner.RunCmd(cmd); err != nil {
			return fmt.Errorf("failed to install local packages: %w", err)
		}
	}

	fmt.Println("Packages installed to rootfs successfully")
	return nil
}

func uniqueStrings(in []string) []string {
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

func isMountPoint(path string) bool {
	cmd := exec.Command("mountpoint", "-q", path)
	return cmd.Run() == nil
}

// CreateDiskImage creates a bootable disk image from a populated rootfs
// This creates partitions, filesystems, and copies the rootfs contents
func (b *Builder) CreateDiskImage(imageChrootRoot, rootfsPath, outputPath string, sizeM int) error {
	fmt.Printf("Creating disk image: %s (%dMB)\n", outputPath, sizeM)

	// Create empty image file
	ddCmd := exec.Command("dd", "if=/dev/zero", fmt.Sprintf("of=%s", outputPath), "bs=1M", fmt.Sprintf("count=%d", sizeM))
	ddCmd.Stdout = runner.LogWriter()
	ddCmd.Stderr = runner.LogWriter()
	if err := runner.RunCmd(ddCmd); err != nil {
		return fmt.Errorf("failed to create image file: %w", err)
	}

	// Set up loop device
	loopDevice, err := b.setupLoopDevice(outputPath)
	if err != nil {
		return fmt.Errorf("failed to setup loop device: %w", err)
	}
	defer b.detachLoopDevice(loopDevice)

	// Create partition table and partitions
	if err := b.partitionDisk(loopDevice); err != nil {
		return fmt.Errorf("failed to partition disk: %w", err)
	}

	// Format partitions
	bootPart := loopDevice + "p1"
	rootPart := loopDevice + "p2"
	if err := b.formatPartitions(bootPart, rootPart); err != nil {
		return fmt.Errorf("failed to format partitions: %w", err)
	}

	// Mount and copy rootfs
	if err := b.copyRootfsToImage(rootfsPath, rootPart, bootPart); err != nil {
		return fmt.Errorf("failed to copy rootfs: %w", err)
	}

	fmt.Printf("Disk image created successfully: %s\n", outputPath)
	return nil
}

func packageNameMatches(pkgPath, expected string) bool {
	f, err := os.Open(pkgPath)
	if err != nil {
		return false
	}
	defer f.Close()

	gzr, err := gzip.NewReader(f)
	if err != nil {
		return false
	}
	defer gzr.Close()

	tr := tar.NewReader(gzr)
	for {
		hdr, err := tr.Next()
		if err == io.EOF {
			return false
		}
		if err != nil {
			return false
		}
		if hdr.Name != ".PKGINFO" {
			continue
		}
		data, err := io.ReadAll(tr)
		if err != nil {
			return false
		}
		for _, line := range strings.Split(string(data), "\n") {
			if strings.HasPrefix(line, "pkgname = ") {
				name := strings.TrimSpace(strings.TrimPrefix(line, "pkgname = "))
				return name == expected
			}
		}
		return false
	}
}

// setupLoopDevice attaches the image file to a loop device
func (b *Builder) setupLoopDevice(imagePath string) (string, error) {
	cmd := exec.Command("sudo", "losetup", "--find", "--show", "--partscan", imagePath)
	output, err := cmd.CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("losetup failed: %w: %s", err, string(output))
	}
	loopDevice := strings.TrimSpace(string(output))
	fmt.Printf("Attached loop device: %s\n", loopDevice)
	return loopDevice, nil
}

// detachLoopDevice detaches the loop device
func (b *Builder) detachLoopDevice(loopDevice string) {
	cmd := exec.Command("sudo", "losetup", "-d", loopDevice)
	_ = cmd.Run()
}

// partitionDisk creates GPT partition table with boot and root partitions
func (b *Builder) partitionDisk(device string) error {
	fmt.Println("Creating partition table...")

	commands := [][]string{
		{"sudo", "parted", "-s", device, "mklabel", "gpt"},
		// Keep BOOT large enough for modern kernels + initramfs (desktop profiles can exceed 100MiB).
		{"sudo", "parted", "-s", device, "mkpart", "boot", "ext2", "1MiB", "513MiB"},
		{"sudo", "parted", "-s", device, "mkpart", "root", "ext4", "513MiB", "100%"},
		{"sudo", "parted", "-s", device, "set", "1", "boot", "on"},
	}

	for _, args := range commands {
		cmd := exec.Command(args[0], args[1:]...)
		cmd.Stdout = runner.LogWriter()
		cmd.Stderr = runner.LogWriter()
		if err := runner.RunCmd(cmd); err != nil {
			return fmt.Errorf("parted command failed: %w", err)
		}
	}

	return nil
}

// formatPartitions creates FAT32 boot and ext4 root filesystems
func (b *Builder) formatPartitions(bootPart, rootPart string) error {
	fmt.Println("Formatting partitions...")

	// Format boot partition as ext2 so lk2nd can mount/read extlinux directly.
	bootCmd := exec.Command("sudo", "mkfs.ext2", "-F", "-L", "BOOT", bootPart)
	bootCmd.Stdout = runner.LogWriter()
	bootCmd.Stderr = runner.LogWriter()
	if err := runner.RunCmd(bootCmd); err != nil {
		return fmt.Errorf("failed to format boot partition: %w", err)
	}

	// Format root partition (ext4) with legacy-compatible features
	rootCmd := exec.Command(
		"sudo", "mkfs.ext4",
		"-L", "ROOT",
		"-O", "^metadata_csum,^metadata_csum_seed,^64bit,^orphan_file",
		"-E", "lazy_itable_init=0,lazy_journal_init=0",
		rootPart,
	)
	rootCmd.Stdout = runner.LogWriter()
	rootCmd.Stderr = runner.LogWriter()
	if err := runner.RunCmd(rootCmd); err != nil {
		return fmt.Errorf("failed to format root partition: %w", err)
	}

	return nil
}

// copyRootfsToImage mounts partitions and copies rootfs contents
func (b *Builder) copyRootfsToImage(rootfsPath, rootPart, bootPart string) error {
	fmt.Println("Copying rootfs to image...")

	// Create mount points
	mountRoot := filepath.Join(os.TempDir(), "peacock-img-root")
	mountBoot := filepath.Join(mountRoot, "boot")

	if err := os.MkdirAll(mountBoot, 0755); err != nil {
		return fmt.Errorf("failed to create mount points: %w", err)
	}
	defer os.RemoveAll(mountRoot)

	// Mount root partition
	if err := runner.RunCmd(exec.Command("sudo", "mount", rootPart, mountRoot)); err != nil {
		return fmt.Errorf("failed to mount root partition: %w", err)
	}
	defer runner.RunCmd(exec.Command("sudo", "umount", mountRoot))

	// Create boot mount point inside root partition
	if err := runner.RunCmd(exec.Command("sudo", "mkdir", "-p", mountBoot)); err != nil {
		return fmt.Errorf("failed to create boot mount point: %w", err)
	}

	// Mount boot partition
	if err := runner.RunCmd(exec.Command("sudo", "mount", bootPart, mountBoot)); err != nil {
		return fmt.Errorf("failed to mount boot partition: %w", err)
	}
	defer runner.RunCmd(exec.Command("sudo", "umount", mountBoot))

	// Copy rootfs contents (skip pseudo filesystems)
	copyCmd := exec.Command(
		"sudo", "rsync",
		"-aAX", "--one-file-system",
		"--exclude=/proc/*",
		"--exclude=/sys/*",
		"--exclude=/dev/*",
		"--exclude=/run/*",
		"--exclude=/tmp/*",
		rootfsPath+"/", mountRoot+"/",
	)
	copyCmd.Stdout = runner.LogWriter()
	copyCmd.Stderr = runner.LogWriter()
	if err := runner.RunCmd(copyCmd); err != nil {
		return fmt.Errorf("failed to copy rootfs: %w", err)
	}

	// CRITICAL: Sync filesystem before unmount to prevent corruption
	fmt.Println("Syncing filesystem...")
	if err := runner.RunCmd(exec.Command("sync")); err != nil {
		return fmt.Errorf("failed to sync filesystem: %w", err)
	}

	fmt.Println("Rootfs copied successfully")
	return nil
}
