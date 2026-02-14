package chroot

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"syscall"

	"peacock/internal/runner"
)

// Mount binds /dev, /proc, and /sys into the target chroot
func Mount(target string) error {
	mounts := []struct {
		Source string
		Target string
		Fstype string
		Flags  uintptr
	}{
		{"/dev", filepath.Join(target, "dev"), "devtmpfs", syscall.MS_BIND | syscall.MS_REC},
		{"/proc", filepath.Join(target, "proc"), "proc", 0},
		{"/sys", filepath.Join(target, "sys"), "sysfs", syscall.MS_BIND | syscall.MS_REC},
	}

	for _, m := range mounts {
		mkCmd := exec.Command("sudo", "mkdir", "-p", m.Target)
		mkCmd.Stdout = runner.LogWriter()
		mkCmd.Stderr = runner.LogWriter()
		if err := runner.RunCmd(mkCmd); err != nil {
			return fmt.Errorf("failed to create mount point %s: %w", m.Target, err)
		}
		// Check if already mounted (naive check)
		// Real implementation might parse /proc/mounts, but for now just try mount
		// If it fails, strictly it's an error, but simple bind mount might be idempotent if carefully done.
		// However, syscall.Mount will fail if busy or etc.
		// Let's just try to mount.
		if err := syscall.Mount(m.Source, m.Target, m.Fstype, m.Flags, ""); err != nil {
			// If error is "busy", maybe it's already mounted. warn and continue?
			// For safety, let's error out for now.
			return fmt.Errorf("failed to mount %s to %s: %w", m.Source, m.Target, err)
		}
	}
	// Also mount /dev/pts
	ptsTarget := filepath.Join(target, "dev", "pts")
	ptsMk := exec.Command("sudo", "mkdir", "-p", ptsTarget)
	ptsMk.Stdout = runner.LogWriter()
	ptsMk.Stderr = runner.LogWriter()
	if err := runner.RunCmd(ptsMk); err != nil {
		return err
	}
	if err := syscall.Mount("devpts", ptsTarget, "devpts", 0, ""); err != nil {
		return fmt.Errorf("failed to mount devpts: %w", err)
	}

	return nil
}

// Unmount unmounts the special filesystems
func Unmount(target string) error {
	mounts := []string{
		filepath.Join(target, "dev", "pts"),
		filepath.Join(target, "sys"),
		filepath.Join(target, "proc"),
		filepath.Join(target, "dev"),
	}

	for _, m := range mounts {
		if err := syscall.Unmount(m, 0); err != nil {
			// Log error but try to continue unmounting others
			fmt.Fprintf(os.Stderr, "failed to unmount %s: %v\n", m, err)
		}
	}
	return nil
}

// Enter runs a command inside the chroot
func Enter(target string, command []string) error {
	if len(command) == 0 {
		command = []string{"/bin/bash"}
	}

	// We use the `chroot` command-line tool for simplicity as it handles setting up the env nicely.
	// Alternatively syscall.Chroot could be used but requires more setup (fork/exec).
	args := append([]string{target}, command...)
	cmd := exec.Command("chroot", args...)
	cmd.Stdin = os.Stdin
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr

	return cmd.Run()
}

// MountWithSudo mounts isolated /dev, /proc, /sys, and /dev/pts into the target chroot using sudo.
func MountWithSudo(target string) error {
	mounts := []struct {
		Source string
		Target string
		Args   []string
	}{
		{"proc", filepath.Join(target, "proc"), []string{"-t", "proc"}},
		{"sysfs", filepath.Join(target, "sys"), []string{"-t", "sysfs"}},
	}

	devTarget := filepath.Join(target, "dev")
	devMk := exec.Command("sudo", "mkdir", "-p", devTarget)
	devMk.Stdout = runner.LogWriter()
	devMk.Stderr = runner.LogWriter()
	if err := runner.RunCmd(devMk); err != nil {
		return fmt.Errorf("failed to create mount point %s: %w", devTarget, err)
	}
	if !isMounted(devTarget) {
		devCmd := exec.Command("sudo", "mount", "-t", "tmpfs", "tmpfs", devTarget, "-o", "mode=0755,nosuid")
		devCmd.Stdout = runner.LogWriter()
		devCmd.Stderr = runner.LogWriter()
		if err := runner.RunCmd(devCmd); err != nil {
			return fmt.Errorf("failed to mount tmpfs on %s: %w", devTarget, err)
		}
	}
	if err := ensureMinimalDevNodes(devTarget); err != nil {
		return err
	}

	for _, m := range mounts {
		mkCmd := exec.Command("sudo", "mkdir", "-p", m.Target)
		mkCmd.Stdout = runner.LogWriter()
		mkCmd.Stderr = runner.LogWriter()
		if err := runner.RunCmd(mkCmd); err != nil {
			return fmt.Errorf("failed to create mount point %s: %w", m.Target, err)
		}
		if isMounted(m.Target) {
			continue
		}

		args := []string{"mount"}
		args = append(args, m.Args...)
		args = append(args, m.Source, m.Target)

		cmd := exec.Command("sudo", args...)
		cmd.Stdout = runner.LogWriter()
		cmd.Stderr = runner.LogWriter()
		if err := runner.RunCmd(cmd); err != nil {
			return fmt.Errorf("failed to mount %s to %s: %w", m.Source, m.Target, err)
		}
	}

	ptsTarget := filepath.Join(target, "dev", "pts")
	ptsMk := exec.Command("sudo", "mkdir", "-p", ptsTarget)
	ptsMk.Stdout = runner.LogWriter()
	ptsMk.Stderr = runner.LogWriter()
	if err := runner.RunCmd(ptsMk); err != nil {
		return err
	}
	if !isMounted(ptsTarget) {
		ptsCmd := exec.Command(
			"sudo",
			"mount",
			"-t",
			"devpts",
			"devpts",
			ptsTarget,
			"-o",
			"newinstance,ptmxmode=0666,mode=620,gid=5",
		)
		ptsCmd.Stdout = runner.LogWriter()
		ptsCmd.Stderr = runner.LogWriter()
		if err := runner.RunCmd(ptsCmd); err != nil {
			return fmt.Errorf("failed to mount devpts: %w", err)
		}
	}

	return nil
}

// UnmountWithSudo unmounts the special filesystems using sudo.
func UnmountWithSudo(target string) error {
	mounts := []string{
		filepath.Join(target, "dev", "pts"),
		filepath.Join(target, "sys"),
		filepath.Join(target, "proc"),
		filepath.Join(target, "dev"),
	}

	for _, m := range mounts {
		if err := umountWithSudo(m); err != nil {
			fmt.Fprintf(os.Stderr, "failed to unmount %s: %v\n", m, err)
		}
	}
	return nil
}

// UnmountPathWithSudo unmounts a single path using sudo.
func UnmountPathWithSudo(path string) error {
	return umountWithSudo(path)
}

// EnterWithSudo runs a command inside the chroot using sudo.
func EnterWithSudo(target string, command []string) error {
	if len(command) == 0 {
		command = []string{"/bin/sh"}
	}

	args := append([]string{"chroot", target}, command...)
	cmd := exec.Command("sudo", args...)
	cmd.Stdin = os.Stdin
	cmd.Stdout = runner.LogWriter()
	cmd.Stderr = runner.LogWriter()

	return runner.RunCmd(cmd)
}

func isMounted(path string) bool {
	cmd := exec.Command("mountpoint", "-q", path)
	err := runner.RunCmd(cmd)
	return err == nil
}

func umountWithSudo(path string) error {
	cleanPath := filepath.Clean(path)
	if !filepath.IsAbs(cleanPath) {
		return fmt.Errorf("refusing to unmount non-absolute path: %s", path)
	}
	switch cleanPath {
	case "/", "/dev", "/dev/pts", "/proc", "/sys", "/run":
		return fmt.Errorf("refusing to unmount critical host path: %s", cleanPath)
	}
	if fi, err := os.Lstat(cleanPath); err == nil && fi.Mode()&os.ModeSymlink != 0 {
		return fmt.Errorf("refusing to unmount symlink path: %s", cleanPath)
	}

	// Skip quietly when the path is not currently a mountpoint.
	if err := exec.Command("mountpoint", "-q", cleanPath).Run(); err != nil {
		return nil
	}

	cmd := exec.Command("sudo", "umount", "-l", cleanPath)
	cmd.Stdout = runner.LogWriter()
	cmd.Stderr = runner.LogWriter()
	if err := runner.RunCmd(cmd); err == nil {
		return nil
	}
	return fmt.Errorf("failed to unmount %s", cleanPath)
}

func ensureMinimalDevNodes(devTarget string) error {
	cmd := exec.Command("sudo", "sh", "-c", fmt.Sprintf(`set -e
DEV=%q
mkdir -p "$DEV/pts" "$DEV/shm"
[ -e "$DEV/null" ] || mknod -m 666 "$DEV/null" c 1 3
[ -e "$DEV/zero" ] || mknod -m 666 "$DEV/zero" c 1 5
[ -e "$DEV/full" ] || mknod -m 666 "$DEV/full" c 1 7
[ -e "$DEV/random" ] || mknod -m 666 "$DEV/random" c 1 8
[ -e "$DEV/urandom" ] || mknod -m 666 "$DEV/urandom" c 1 9
[ -e "$DEV/tty" ] || mknod -m 666 "$DEV/tty" c 5 0
[ -e "$DEV/ptmx" ] || mknod -m 666 "$DEV/ptmx" c 5 2
`, devTarget))
	cmd.Stdout = runner.LogWriter()
	cmd.Stderr = runner.LogWriter()
	if err := runner.RunCmd(cmd); err != nil {
		return fmt.Errorf("failed to populate %s: %w", devTarget, err)
	}
	return nil
}
