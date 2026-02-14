package builder

import (
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"

	"peacock/internal/chroot"
	"peacock/internal/manifest"
	"peacock/internal/pacman"
	"peacock/internal/runner"
)

const (
	archBootstrapURL = "https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst"
	archArmRootfsURL = "https://archlinuxarm.org/os"
)

type BuildOptions struct {
	UseQemu      *bool
	CrossCompile string
	ExtraPath    []string
	ExtraInclude []string
	ExtraLib     []string
	ExtraLdLib   []string
}

func archRootfsURL(targetArch string) (string, error) {
	switch targetArch {
	case "x86_64":
		return archBootstrapURL, nil
	case "armv7", "armv7h":
		return fmt.Sprintf("%s/ArchLinuxARM-armv7-latest.tar.gz", archArmRootfsURL), nil
	case "aarch64":
		return fmt.Sprintf("%s/ArchLinuxARM-aarch64-latest.tar.gz", archArmRootfsURL), nil
	default:
		return "", fmt.Errorf("unsupported target arch for arch rootfs: %s", targetArch)
	}
}

func qemuStaticName(targetArch string) string {
	switch targetArch {
	case "armv7", "armv7h":
		return "qemu-arm-static"
	case "aarch64":
		return "qemu-aarch64-static"
	default:
		return ""
	}
}

func hostArchString() string {
	switch runtime.GOARCH {
	case "amd64":
		return "x86_64"
	case "arm64":
		return "aarch64"
	case "arm":
		return "armv7"
	default:
		return runtime.GOARCH
	}
}

func copyFile(src, dst string, mode os.FileMode) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()

	if err := os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
		return err
	}
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer out.Close()

	if _, err := io.Copy(out, in); err != nil {
		return err
	}
	return os.Chmod(dst, mode)
}

func copyFileWithSudo(src, dst string) error {
	mkCmd := exec.Command("sudo", "mkdir", "-p", filepath.Dir(dst))
	mkCmd.Stdout = runner.LogWriter()
	mkCmd.Stderr = runner.LogWriter()
	if err := runner.RunCmd(mkCmd); err != nil {
		return err
	}
	cpCmd := exec.Command("sudo", "cp", "-f", src, dst)
	cpCmd.Stdout = runner.LogWriter()
	cpCmd.Stderr = runner.LogWriter()
	return runner.RunCmd(cpCmd)
}

// EnsureBuildChroot sets up an Arch-based build root for the given architecture.
func (b *Builder) EnsureBuildChroot(root string, chrootArch string, useQemu bool) error {
	cleanRoot := filepath.Clean(root)
	if cleanRoot == "" || cleanRoot == "." || cleanRoot == string(os.PathSeparator) {
		return fmt.Errorf("refusing to operate on unsafe chroot root: %q", root)
	}
	if !strings.Contains(cleanRoot, string(os.PathSeparator)+"peacock"+string(os.PathSeparator)) {
		return fmt.Errorf("refusing to operate outside peacock workdir: %q", root)
	}

	// If requesting standard x86 build env (Master Chroot)
	if chrootArch == "x86_64" {
		chrootExists := false
		if _, err := os.Stat(filepath.Join(root, "etc", "arch-release")); err == nil {
			chrootExists = true
		}
		
		// Register binfmt handlers EVERY time (not just on first bootstrap)
		// This ensures QEMU works even if host was rebooted (binfmt is lost on reboot)
		if chrootExists {
			fmt.Println("Master chroot exists, ensuring binfmt handlers are registered...")
			binfmtDir := filepath.Join(root, "usr", "lib", "binfmt.d")
			if entries, err := os.ReadDir(binfmtDir); err == nil {
				for _, entry := range entries {
					if !strings.HasPrefix(entry.Name(), "qemu-") || !strings.HasSuffix(entry.Name(), ".conf") {
						continue
					}
					confPath := filepath.Join(binfmtDir, entry.Name())
					data, err := os.ReadFile(confPath)
					if err != nil {
						continue
					}
					
					lines := strings.Split(string(data), "\n")
					for _, line := range lines {
						line = strings.TrimSpace(line)
						if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, ";") || !strings.HasPrefix(line, ":") {
							continue
						}
						
						// Replace path with absolute path to our static QEMU in master chroot
						parts := strings.Split(line, ":")
						if len(parts) >= 7 {
							// Check if the static binary exists in the chroot
							// Check if the static binary exists in the chroot
							// interpreter is usually /usr/bin/qemu-arm-static
							// We want ROOT/usr/bin/qemu-arm-static
							interpreterName := filepath.Base(parts[6])
							// Resolve symlinks/abs path logic? binfmtDir is inside ROOT/usr/lib/binfmt.d
							// So .. -> lib, .. -> usr, bin -> usr/bin
							
							// Just construct absolute path from 'root' variable which is passed to this function
							absStaticPath := filepath.Join(root, "usr", "bin", interpreterName)
							
							if _, err := os.Stat(absStaticPath); err == nil {
								parts[6] = absStaticPath
							} else {
								// Fallback: try host's non-static one (better than nothing)
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
						
						registerCmd := exec.Command("sudo", "sh", "-c", fmt.Sprintf("echo '%s' > /proc/sys/fs/binfmt_misc/register", line))
						registerCmd.Stdout = runner.LogWriter()
						registerCmd.Stderr = runner.LogWriter()
						if err := runner.RunCmd(registerCmd); err == nil {
							if len(parts) > 6 {
								fmt.Printf("Registered: %s (using %s)\n", parts[1], parts[6])
							}
						}
					}
				}
			}
			return nil
		}

		// Download and extract Arch Linux Bootstrap
		if _, err := os.Stat(root); err == nil {
			rmCmd := exec.Command("sudo", "rm", "-rf", root)
			rmCmd.Stdout = runner.LogWriter()
			rmCmd.Stderr = runner.LogWriter()
			if err := runner.RunCmd(rmCmd); err != nil {
				return fmt.Errorf("failed to clean existing chroot %s: %w", root, err)
			}
		}
		if err := os.MkdirAll(root, 0755); err != nil {
			return err
		}

		rootfsURL, err := archRootfsURL("x86_64")
		if err != nil {
			return err
		}

		tarball, err := b.Download(rootfsURL, "")
		if err != nil {
			return fmt.Errorf("failed to download arch rootfs: %w", err)
		}

		tmpDir, err := os.MkdirTemp("", "peacock-rootfs-")
		if err != nil {
			return err
		}
		defer func() {
			rmTmp := exec.Command("sudo", "rm", "-rf", tmpDir)
			rmTmp.Stdout = runner.LogWriter()
			rmTmp.Stderr = runner.LogWriter()
			_ = runner.RunCmd(rmTmp)
		}()

		cmd := exec.Command("sudo", "tar", "-xf", tarball, "-C", tmpDir) // Extract to tmp
		cmd.Stdout = runner.LogWriter()
		cmd.Stderr = runner.LogWriter()
		if err := runner.RunCmd(cmd); err != nil {
			return fmt.Errorf("failed to extract arch rootfs: %w", err)
		}

		// Locate root inside extraction
		rootSrc := tmpDir
		entries, err := os.ReadDir(tmpDir)
		if err == nil {
			for _, entry := range entries {
				if strings.HasPrefix(entry.Name(), "root.") {
					rootSrc = filepath.Join(tmpDir, entry.Name())
					break
				}
			}
		}
		// Also check strict "root.x86_64" usually found in bootstrap
		preferred := filepath.Join(tmpDir, "root.x86_64")
		if info, err := os.Stat(preferred); err == nil && info.IsDir() {
			rootSrc = preferred
		}

		// Move to final location
		rmRoot := exec.Command("sudo", "rm", "-rf", root)
		rmRoot.Stdout = runner.LogWriter()
		rmRoot.Stderr = runner.LogWriter()
		_ = runner.RunCmd(rmRoot)
		
		mkRoot := exec.Command("sudo", "mkdir", "-p", root)
		mkRoot.Stdout = runner.LogWriter()
		mkRoot.Stderr = runner.LogWriter()
		if err := runner.RunCmd(mkRoot); err != nil {
			return err
		}
		
		// Copy content
		copyCmd := exec.Command("sudo", "cp", "-a", rootSrc+string(os.PathSeparator)+".", root)
		copyCmd.Stdout = runner.LogWriter()
		copyCmd.Stderr = runner.LogWriter()
		if err := runner.RunCmd(copyCmd); err != nil {
			return fmt.Errorf("failed to install arch rootfs: %w", err)
		}

		// Bootstrap pacman in master using internal pacman (avoid host dependency)
		// We need to mount proc/sys/dev first
		if err := chroot.MountWithSudo(root); err != nil {
			return err
		}
		defer chroot.UnmountWithSudo(root)


		// Enable mirrors in master chroot and disable space check
		mirrorlistPath := filepath.Join(root, "etc", "pacman.d", "mirrorlist")
		if err := runner.RunCmd(exec.Command("sudo", "sed", "-i", "s/^#Server/Server/", mirrorlistPath)); err != nil {
			fmt.Printf("Warning: failed to update mirrorlist: %v\n", err)
		}
		
		confPath := filepath.Join(root, "etc", "pacman.conf")
		if err := runner.RunCmd(exec.Command("sudo", "sed", "-i", "s/^CheckSpace/#CheckSpace/", confPath)); err != nil {
            fmt.Printf("Warning: failed to disable CheckSpace: %v\n", err)
		}
		
		// Create /etc/mtab symlink in Master so pacman can check free space/mounts properly
		if err := runner.RunCmd(exec.Command("sudo", "ln", "-sf", "/proc/self/mounts", filepath.Join(root, "etc", "mtab"))); err != nil {
			fmt.Printf("Warning: failed to link /etc/mtab: %v\n", err)
		}

		// Initialize keys inside
		if err := runner.RunCmd(exec.Command("sudo", "chroot", root, "pacman-key", "--init")); err != nil {
             // Continue? Keys might fail but let's try populate
		}
		if err := runner.RunCmd(exec.Command("sudo", "chroot", root, "pacman-key", "--populate", "archlinux")); err != nil {
             // Ignore population error if keys aren't perfect, we can use --noconfirm
		}
		
		// Install packages inside
		// We need network in chroot (resolv.conf)
		if _, err := os.Stat("/etc/resolv.conf"); err == nil {
			_ = copyFileWithSudo("/etc/resolv.conf", filepath.Join(root, "etc", "resolv.conf"))
		}
		
		pkgs := []string{"base-devel", "qemu-user-static", "qemu-user-static-binfmt", "git"}
		args := append([]string{"chroot", root, "pacman", "-Sy", "--noconfirm"}, pkgs...)
		if err := runner.RunCmd(exec.Command("sudo", args...)); err != nil {
			return fmt.Errorf("failed to install packages in master chroot: %w", err)
		}
		
	// Register binfmt handlers by reading config files and writing to /proc
	// The qemu-user-static-binfmt package provides config files in /usr/lib/binfmt.d/
	// Format: :name:type:offset:magic:mask:interpreter:flags (one per line)
	fmt.Println("Registering QEMU binfmt handlers...")
	binfmtDir := filepath.Join(root, "usr", "lib", "binfmt.d")
	entries, err = os.ReadDir(binfmtDir)
	if err != nil {
		fmt.Printf("Warning: could not read binfmt.d directory: %v\n", err)
	} else {
		for _, entry := range entries {
			if !strings.HasPrefix(entry.Name(), "qemu-") || !strings.HasSuffix(entry.Name(), ".conf") {
				continue
			}
			confPath := filepath.Join(binfmtDir, entry.Name())
			fmt.Printf("Processing %s...\n", entry.Name())
			
			// Read and parse the conf file
			data, err := os.ReadFile(confPath)
			if err != nil {
				fmt.Printf("Warning: could not read %s: %v\n", entry.Name(), err)
				continue
			}
			
			// Process each line
			lines := strings.Split(string(data), "\n")
			for _, line := range lines {
				line = strings.TrimSpace(line)
				// Skip empty lines and comments
				if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, ";") {
					continue
				}
				
				// Each line should be a binfmt_misc registration string
				// Format: :name:type:offset:magic:mask:interpreter:flags
				if !strings.HasPrefix(line, ":") {
					continue
				}
				
				// Parse and modify interpreter path to use absolute path to our static QEMU in master chroot
				parts := strings.Split(line, ":")
				if len(parts) >= 7 {
					interpreterName := filepath.Base(parts[6])
					absStaticPath := filepath.Join(root, "usr", "bin", interpreterName)
					
					if _, err := os.Stat(absStaticPath); err == nil {
						parts[6] = absStaticPath
					} else {
						// Fallback: try host's non-static one
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
					// Ignore errors - entry might already be registered
					if len(parts) > 1 {
						fmt.Printf("Note: registration of %s skipped (might already exist or error occurred)\n", parts[1])
					}
				} else {
					if len(parts) > 6 {
						fmt.Printf("Registered: %s (using %s)\n", parts[1], parts[6])
					}
				}
			}
		}
	}

	return nil
	}

	// Case: ARM / Target Chroot
	// We require the master x86 chroot to function.
	masterRoot := filepath.Join(filepath.Dir(root), "..", "build-chroot", "master-x86_64") // naming convention?
	// Let's fix pathing: root is usually .../build-chroot/armv7
	// master is .../build-chroot/x86_64
	masterRoot = filepath.Join(filepath.Dir(root), "x86_64")
	
	if err := b.EnsureBuildChroot(masterRoot, "x86_64", false); err != nil {
		return fmt.Errorf("failed to ensure master chroot: %w", err)
	}

	// Mount Master chroot pseudo-filesystems so pacman (and mtab) works inside
	if err := chroot.MountWithSudo(masterRoot); err != nil {
		return fmt.Errorf("failed to mount master chroot: %w", err)
	}
	defer chroot.UnmountWithSudo(masterRoot)

	if _, err := os.Stat(filepath.Join(root, "etc", "arch-release")); err == nil {
		// Ensure QEMU is present even if chroot exists (needed for execution)
		qemuName := qemuStaticName(chrootArch)
		if qemuName != "" {
			qemuSrc := filepath.Join(masterRoot, "usr", "bin", qemuName)
			qemuDst := filepath.Join(root, "usr", "bin", qemuName)
			// Ensure destination usr/bin exists
			if err := runner.RunCmd(exec.Command("sudo", "mkdir", "-p", filepath.Dir(qemuDst))); err != nil {
				return fmt.Errorf("failed to ensure dest dir for qemu: %w", err)
			}
			if err := copyFileWithSudo(qemuSrc, qemuDst); err != nil {
				return fmt.Errorf("failed to copy %s to target: %w", qemuName, err)
			}
		}
		return nil // Already exists
	}

	// We create the ARM chroot using tools from masterRoot
	if err := os.MkdirAll(root, 0755); err != nil {
		return err
	}
	// Create var/lib/pacman so pacman -r works
	if err := os.MkdirAll(filepath.Join(root, "var", "lib", "pacman"), 0755); err != nil {
		return err
	}
	
	// Bind mount Host Cache to Target Pacman Cache to avoid re-downloads
	// Target path: root/var/cache/pacman/pkg
	targetCache := filepath.Join(root, "var", "cache", "pacman", "pkg")
	if err := os.MkdirAll(targetCache, 0755); err != nil {
		return err
	}
	
	// Host cache source: b.CacheDir is passed to Builder. 
	// Usually ~/.local/var/peacock/peacock-cache
	// We want to mount it to /var/cache/pacman/pkg
	if err := runner.RunCmd(exec.Command("sudo", "mount", "--bind", b.CacheDir, targetCache)); err != nil {
		return fmt.Errorf("failed to bind mount cache: %w", err)
	}
	// We need to unmount this eventually... defer unmount in the Caller?
	// Or just leave it mounted until reboot/cleanup? 
	// EnsureBuildChroot is called once per build-session usually.
	// But defer will run at end of function.
	// We need it mounted during install.
	defer runner.RunCmd(exec.Command("sudo", "umount", targetCache))
    
    // Actually, we can't defer unmount here because subsequent steps (BuildPackageInChroot) 
    // might essentially need it? No, BuildPackageInChroot mounts its own deps.
    // BUT the bootstrap install happens HERE. So we need it for THIS function scope.
    // OK to defer unmount.

	// Symlink /proc/self/mounts to /etc/mtab for pacman disk check
	// We need /etc to exist first (it does, likely created by MkdirAll above or future step? No, MkdirAll only created root and var/lib/pacman)
	// Actually, target root is currently empty except for var/lib/pacman.
	if err := os.MkdirAll(filepath.Join(root, "etc"), 0755); err != nil {
		return err
	}
	_ = runner.RunCmd(exec.Command("sudo", "ln", "-sf", "/proc/self/mounts", filepath.Join(root, "etc", "mtab")))

	// 1. Generate pacman config for ARM
	if err := pacman.GenerateConfig(root, "armv7h"); err != nil {
		return err
	}
	// Disable CheckSpace in target config to avoid mount point errors in nested chroot
	targetConfPath := filepath.Join(root, "etc", "pacman.conf")
	if err := runner.RunCmd(exec.Command("sudo", "sed", "-i", "s/^CheckSpace/#CheckSpace/", targetConfPath)); err != nil {
		fmt.Printf("Warning: failed to disable CheckSpace in target: %v\n", err)
	}
	
	// 2. Install 'base' and 'qemu-user-static' into root using Master's pacman
	// We need to execute `pacman -r /target ...` inside master.
	// We assume master has networking and keys setup.
	
	// Issue: Master chroot paths.
	// We probably need to bind mount `root` (target) into `masterRoot/mnt` to access it easily.
	// Or just use the full path if master is unprivileged? No, `arch-chroot` or `chroot` needs relative.
	// We will rely on `pacman.Install` with `execRoot`.
	// BUT `pacman.Install` runs `chroot execRoot pacman -r target`.
	// `target` must be visible inside `execRoot`.
	
	// Bind mount target into master
	mountPoint := filepath.Join(masterRoot, "mnt", "target")
	if err := runner.RunCmd(exec.Command("sudo", "mkdir", "-p", mountPoint)); err != nil {
		return err
	}
	// Bind mount
	bindCmd := exec.Command("sudo", "mount", "--rbind", root, mountPoint)
	if err := runner.RunCmd(bindCmd); err != nil {
		return fmt.Errorf("failed to bind mount target into master: %w", err)
	}
	defer func() {
		_ = runner.RunCmd(exec.Command("sudo", "umount", mountPoint))
	}()

	// Now install. Target path inside master is "/mnt/target".
	// Config file is at "/mnt/target/etc/pacman.conf"
	
	// Now install. Target path inside master is "/mnt/target".
	// Config file is at "/mnt/target/etc/pacman.conf"
	
	// We do NOT install qemu-user-static inside the ARM target.
	// It is an x86 package.
	// We rely on the Master chroot (or Host) having binfmt_misc set up with the 'F' flag (Fix Binary),
	// which allows the kernel to use the interpreter (qemu-arm-static) from the master/host 
	// without needing it present inside the target chroot.
	
	pkgs := []string{"base"} 
	
	if err := pacman.Install("/mnt/target", "/mnt/target/etc/pacman.conf", pkgs, "/mnt/target/var/cache/pacman/pkg", false, masterRoot); err != nil {
		return fmt.Errorf("failed to bootstrap ARM chroot from master: %w", err)
	}
	
	// 3. Copy qemu-arm-static from Master to Target
	// Required for execution on host systems where binfmt_misc expects interpreter inside chroot
	// or if the 'F' flag was not used during registration.
	qemuName := qemuStaticName(chrootArch)
	if qemuName != "" {
		qemuSrc := filepath.Join(masterRoot, "usr", "bin", qemuName)
		qemuDst := filepath.Join(root, "usr", "bin", qemuName)
		// Ensure destination usr/bin exists (it should from base install)
		if err := runner.RunCmd(exec.Command("sudo", "mkdir", "-p", filepath.Dir(qemuDst))); err != nil {
			return fmt.Errorf("failed to ensure dest dir for qemu: %w", err)
		}
		if err := copyFileWithSudo(qemuSrc, qemuDst); err != nil {
			return fmt.Errorf("failed to copy %s to target: %w", qemuName, err)
		}
	}
	
	return nil
}


func (b *Builder) installBuildDeps(root string, deps []string, execRoot string) error {
	if len(deps) == 0 {
		return nil
	}
	
	targetPath := root
	confPath := filepath.Join(root, "etc", "pacman.conf")
	
	if execRoot != "" {
		// We are using a master chroot.
		// We need to mount root into execRoot/mnt/target first?
		// Or assume caller has done mounting?
		// To be safe and robust, we should handle mount here or assume a standard mount point.
		// Let's assume standard mount point "/mnt/target" is used if execRoot is set,
		// and that 'root' passed here corresponds to that.
		// Actually, pacman.Install handles the chroot command.
		// But valid paths inside execRoot need to be passed.
		
		// Strategy: Bind mount 'root' to 'execRoot/mnt/deps-target' just for this op.
		mountPoint := filepath.Join(execRoot, "mnt", "deps-target")
		if err := runner.RunCmd(exec.Command("sudo", "mkdir", "-p", mountPoint)); err != nil {
			return err
		}
		if err := runner.RunCmd(exec.Command("sudo", "mount", "--rbind", root, mountPoint)); err != nil {
			return err
		}
		defer runner.RunCmd(exec.Command("sudo", "umount", mountPoint))
		
		// Mount cache
		cacheMount := filepath.Join(mountPoint, "var", "cache", "pacman", "pkg")
		if err := runner.RunCmd(exec.Command("sudo", "mkdir", "-p", cacheMount)); err != nil {return err}
		if err := runner.RunCmd(exec.Command("sudo", "mount", "--bind", b.CacheDir, cacheMount)); err != nil {return err}
		defer runner.RunCmd(exec.Command("sudo", "umount", cacheMount))

		targetPath = "/mnt/deps-target"
		confPath = filepath.Join(targetPath, "etc", "pacman.conf")
		
		// If we are installing deps, we might need a sanitized config inside the execRoot?
		// pacman.SanitizeConfig creates a temp file on HOST.
		// If we pass that temp file path to `chroot execRoot pacman ...`, the chroot won't see it
		// because /tmp is usually not shared unless we bind mount it.
		// This is a complication.
		
		// Alternative: Generate a config inside the mounted target?
		// Or just copy the sanitized config into the target temporarily.
		
		tmpConf, cleanup, err := pacman.SanitizeConfig(filepath.Join(root, "etc", "pacman.conf"))
		if err != nil {
			return err
		}
		defer cleanup()
		
		// Copy tmpConf to root/tmp/pacman-deps.conf
		innerConfPath := filepath.Join(root, "tmp", "pacman-deps.conf")
		if err := runner.RunCmd(exec.Command("sudo", "mkdir", "-p", filepath.Dir(innerConfPath))); err != nil {return err}
		if err := copyFileWithSudo(tmpConf, innerConfPath); err != nil {return err}
		
		// Inside chroot, path is /mnt/deps-target/tmp/pacman-deps.conf
		// Pass explicit cachedir
		return pacman.Install(targetPath, filepath.Join(targetPath, "tmp", "pacman-deps.conf"), deps, filepath.Join(targetPath, "var/cache/pacman/pkg"), false, execRoot)
	}

	confPath = filepath.Join(root, "etc", "pacman.conf")
	startConf, cleanup, err := pacman.SanitizeConfig(confPath)
	if err != nil {
		return fmt.Errorf("failed to sanitize pacman config: %w", err)
	}
	defer cleanup()

	// Mount cache if not using nested chroot (host execution)
	cacheMount := filepath.Join(root, "var", "cache", "pacman", "pkg")
	if err := os.MkdirAll(cacheMount, 0755); err != nil {return err}
	if err := runner.RunCmd(exec.Command("sudo", "mount", "--bind", b.CacheDir, cacheMount)); err != nil {return err}
	defer runner.RunCmd(exec.Command("sudo", "umount", cacheMount))

	// Using pacman to install build deps
	// Pass explicit cachedir (absolute path on host)
	return pacman.Install(root, startConf, deps, cacheMount, false, "")
}

// BuildPackageInChroot downloads and builds a package inside the given chroot.
func (b *Builder) BuildPackageInChroot(pkg *manifest.Package, targetArch string, root string, opts BuildOptions) (string, error) {
	if pkg.Build.Source == "" && pkg.Build.Script == "" {
		return "", fmt.Errorf("package %s has no source or build script", pkg.Package.Name)
	}

	useQemu := false
	if opts.UseQemu != nil {
		useQemu = *opts.UseQemu
	}

	hostArch := hostArchString()
	if !useQemu && opts.CrossCompile == "" && targetArch != hostArch {
		return "", fmt.Errorf("cross-arch build requires qemu or cross compiler; set build.cross_compile or --cross-compile, or enable qemu")
	}

	chrootArch := targetArch
	if !useQemu {
		chrootArch = hostArch
	}

	if err := b.EnsureBuildChroot(root, chrootArch, useQemu); err != nil {
		return "", err
	}
	
	masterRoot := ""
	// If EnsureBuildChroot created a nested environment, we know master is at ../x86_64
	if useQemu && ((hostArch == "amd64" && chrootArch != "x86_64") || (hostArch == "arm64" && chrootArch != "aarch64")) {
		// convention used in EnsureBuildChroot
		masterRoot = filepath.Join(filepath.Dir(root), "x86_64")
	}

	if err := chroot.MountWithSudo(root); err != nil {
		return "", err
	}
	defer chroot.UnmountWithSudo(root)

	if data, err := os.ReadFile("/etc/resolv.conf"); err == nil {
		_ = os.WriteFile(filepath.Join(root, "etc", "resolv.conf"), data, 0644)
	}

	if err := b.installBuildDeps(root, pkg.Build.BuildDeps, masterRoot); err != nil {
		return "", fmt.Errorf("failed to install build deps: %w", err)
	}

	tarball := ""
	if pkg.Build.Source != "" {
		var err error
		tarball, err = b.Download(pkg.Build.Source, pkg.Build.Checksum)
		if err != nil {
			return "", fmt.Errorf("failed to download source: %w", err)
		}
	}

	buildDir := filepath.Join(root, "build", pkg.Package.Name+"-"+pkg.Package.Version+"-"+targetArch)
	rmBuild := exec.Command("sudo", "rm", "-rf", buildDir)
	rmBuild.Stdout = runner.LogWriter()
	rmBuild.Stderr = runner.LogWriter()
	if err := runner.RunCmd(rmBuild); err != nil {
		return "", err
	}
	mkBuild := exec.Command("sudo", "mkdir", "-p", buildDir)
	mkBuild.Stdout = runner.LogWriter()
	mkBuild.Stderr = runner.LogWriter()
	if err := runner.RunCmd(mkBuild); err != nil {
		return "", err
	}
	chownBuild := exec.Command("sudo", "chown", "-R", fmt.Sprintf("%d:%d", os.Getuid(), os.Getgid()), buildDir)
	chownBuild.Stdout = runner.LogWriter()
	chownBuild.Stderr = runner.LogWriter()
	if err := runner.RunCmd(chownBuild); err != nil {
		return "", err
	}

	// Copy auxiliary files (config, patches) from package directory
	if pkg.ManifestDir != "" {
		files, err := os.ReadDir(pkg.ManifestDir)
		if err == nil {
			for _, file := range files {
				if file.Name() == "package.toml" {
					continue
				}
				srcFile := filepath.Join(pkg.ManifestDir, file.Name())
				destFile := filepath.Join(buildDir, file.Name())

				in, err := os.Open(srcFile)
				if err != nil {
					continue
				}

				out, err := os.Create(destFile)
				if err != nil {
					in.Close()
					continue
				}

				io.Copy(out, in)
				in.Close()
				out.Close()
			}
		}
	}

	fmt.Printf("Building package %s %s for %s in %s (chroot)\n", pkg.Package.Name, pkg.Package.Version, targetArch, buildDir)

	// Extract source if provided
	if tarball != "" {
		cmd := exec.Command("tar", "-xf", tarball, "-C", buildDir, "--strip-components=1")
		cmd.Stdout = runner.LogWriter()
		cmd.Stderr = runner.LogWriter()
		if err := runner.RunCmd(cmd); err != nil {
			return "", fmt.Errorf("failed to extract source: %w", err)
		}
	}

	if pkg.Build.Script != "" {
		scriptPath := filepath.Join(buildDir, "peacock-build.sh")
		scriptContent := "#!/bin/sh\nset -e\n" + pkg.Build.Script + "\n"
		if err := os.WriteFile(scriptPath, []byte(scriptContent), 0755); err != nil {
			return "", err
		}

		buildDirInChroot := filepath.Join("/build", pkg.Package.Name+"-"+pkg.Package.Version+"-"+targetArch)
		archEnv := ""
		switch targetArch {
		case "armv7", "armv7h":
			archEnv = "arm"
		case "aarch64":
			archEnv = "arm64"
		case "x86_64":
			archEnv = "x86_64"
		}
		pathPrefix := ""
		if len(opts.ExtraPath) > 0 {
			pathPrefix = strings.Join(opts.ExtraPath, ":") + ":"
		}
		pathSuffix := ""
		if pathPrefix != "" {
			pathSuffix = ":" + pathPrefix
		}
		envLines := []string{
			"export PATH=/usr/sbin:/usr/bin:/sbin:/bin" + pathSuffix + "/usr/local/sbin:/usr/local/bin",
		}
		if len(opts.ExtraInclude) > 0 && opts.CrossCompile == "" {
			envLines = append(envLines, "export C_INCLUDE_PATH="+strings.Join(opts.ExtraInclude, ":")+":${C_INCLUDE_PATH:-}")
		}
		if len(opts.ExtraLib) > 0 && opts.CrossCompile == "" {
			envLines = append(envLines, "export LIBRARY_PATH="+strings.Join(opts.ExtraLib, ":")+":${LIBRARY_PATH:-}")
		}
		if len(opts.ExtraLdLib) > 0 && opts.CrossCompile == "" {
			envLines = append(envLines, "export LD_LIBRARY_PATH="+strings.Join(opts.ExtraLdLib, ":")+":${LD_LIBRARY_PATH:-}")
		}
		if archEnv != "" {
			envLines = append(envLines, "export ARCH="+archEnv)
		}
		if opts.CrossCompile != "" {
			envLines = append(envLines, "export CROSS_COMPILE="+opts.CrossCompile)
		}
		envScriptPath := filepath.Join(buildDir, "peacock-env.sh")
		envContent := "#!/bin/sh\n" + strings.Join(envLines, "\n") + "\n"
		if err := os.WriteFile(envScriptPath, []byte(envContent), 0644); err != nil {
			return "", err
		}
		cmdArgs := []string{"/bin/sh", "-c", fmt.Sprintf("cd %s && . ./peacock-env.sh && /bin/sh ./peacock-build.sh", buildDirInChroot)}
		
		if masterRoot != "" {
			// Nested execution: Host -> Master -> Target -> Command
			
			// 1. Mount Target into Master
			mountPoint := filepath.Join(masterRoot, "mnt", "build-execution")
			if err := runner.RunCmd(exec.Command("sudo", "mkdir", "-p", mountPoint)); err != nil {
				return "", err
			}
			if err := runner.RunCmd(exec.Command("sudo", "mount", "--rbind", root, mountPoint)); err != nil {
				return "", fmt.Errorf("failed to mount target for execution: %w", err)
			}
			defer runner.RunCmd(exec.Command("sudo", "umount", mountPoint))
			
			// 2. Construct nested command
			// chroot master chroot /mnt/build-execution ...
			
			// We need to be careful with quoting. cmdArgs[0] is /bin/sh, [1] is -c, [2] is script
			// cmdArgs is actually: /bin/sh -c "cd ... && ..."
			
			// Target chroot cmd: chroot /mnt/build-execution /bin/sh -c "..."
			// Master chroot cmd: chroot masterRoot chroot /mnt/build-execution /bin/sh -c "..."
			
			// We construct the "inner" chroot command as a string to pass to master shell?
			// Or just chain args?
			// sudo chroot masterRoot chroot /mnt/build-execution /bin/sh -c "..." works if passed as args to exec.
			
			nestedArgs := []string{"chroot", masterRoot, "chroot", "/mnt/build-execution"}
			nestedArgs = append(nestedArgs, cmdArgs...)
			
			nestedCmd := exec.Command("sudo", nestedArgs...)
			nestedCmd.Stdout = runner.LogWriter()
			nestedCmd.Stderr = runner.LogWriter()
			
			if err := runner.RunCmd(nestedCmd); err != nil {
				return "", fmt.Errorf("nested build script failed: %w", err)
			}
		} else {
			if err := chroot.EnterWithSudo(root, cmdArgs); err != nil {
				return "", fmt.Errorf("build script failed: %w", err)
			}
		}
	}

	// Return build dir on host filesystem
	return buildDir, nil
}
