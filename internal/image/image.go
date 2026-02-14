package image

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"peacock/internal/runner"
)

// CreateBlank creates a blank image file of the specified size
func CreateBlank(path string, sizeMB int) error {
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return err
	}
	cmd := exec.Command("truncate", "-s", fmt.Sprintf("%dM", sizeMB), path)
	return runner.RunCmd(cmd)
}

// Partition creates partitions on the image file.
// For simplicity, we assume a standard layout: Boot (100MB) + Root (Rest).
// We use sfdisk.
func Partition(imagePath string) error {
	// Layout:
	// start=2048, size=204800, type=L, bootable (100MB)
	// start=206848, type=L (Rest)
	layout := `
label: dos
device: %s
unit: sectors

start=2048, size=204800, type=83, bootable
start=206848, type=83
`
	layout = fmt.Sprintf(layout, imagePath)

	cmd := exec.Command("sfdisk", imagePath)
	cmd.Stdin = strings.NewReader(layout)
	// sfdisk writes to stdout/stderr
	cmd.Stdout = runner.LogWriter()
	cmd.Stderr = runner.LogWriter()
	return runner.RunCmd(cmd)
}

// MountLoop mounts the image as a loop device and returns the loop device path
func MountLoop(imagePath string) (string, error) {
	// losetup -P -f --show imagePath
	cmd := exec.Command("sudo", "losetup", "-P", "-f", "--show", imagePath)
	out, err := runner.RunOutput(cmd)
	if err != nil {
		return "", err
	}
	return strings.TrimSpace(out), nil
}

// UnmountLoop detaches the loop device
func UnmountLoop(loopDevice string) error {
	cmd := exec.Command("sudo", "losetup", "-d", loopDevice)
	return runner.RunCmd(cmd)
}

// Format formats the partitions
func Format(partitionPath string, fsType string, label string) error {
	var cmd *exec.Cmd
	switch fsType {
	case "ext4":
		cmd = exec.Command(
			"sudo", "mkfs.ext4",
			"-L", label,
			"-O", "^metadata_csum,^64bit",
			"-E", "lazy_itable_init=0,lazy_journal_init=0",
			partitionPath,
		)
	case "vfat":
		cmd = exec.Command("sudo", "mkfs.vfat", "-n", label, partitionPath)
	default:
		return fmt.Errorf("unsupported filesystem: %s", fsType)
	}

	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}
