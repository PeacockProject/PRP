package builder

import (
	"fmt"
	"os/exec"
	"path/filepath"
)

// EnableOpenRCService adds a service to the specified runlevel inside rootfs.
func (b *Builder) EnableOpenRCService(imageChrootRoot, rootfsPath, service, runlevel string) error {
	if service == "" {
		return nil
	}
	if runlevel == "" {
		runlevel = "default"
	}

	servicePath := filepath.Join(rootfsPath, "etc", "init.d", service)
	if err := exec.Command("test", "-x", servicePath).Run(); err != nil {
		return fmt.Errorf("openrc service not found or not executable: %s", servicePath)
	}

	runlevelDir := filepath.Join(rootfsPath, "etc", "runlevels", runlevel)
	if err := exec.Command("sudo", "mkdir", "-p", runlevelDir).Run(); err != nil {
		return fmt.Errorf("failed to create runlevel dir: %w", err)
	}

	linkPath := filepath.Join(runlevelDir, service)
	if err := exec.Command("sudo", "ln", "-sf", filepath.Join("/etc", "init.d", service), linkPath).Run(); err != nil {
		return fmt.Errorf("failed to link service into runlevel: %w", err)
	}

	return nil
}

// DisableOpenRCService removes a service from all runlevels inside rootfs.
func (b *Builder) DisableOpenRCService(rootfsPath, service string) error {
	if service == "" {
		return nil
	}

	links, err := filepath.Glob(filepath.Join(rootfsPath, "etc", "runlevels", "*", service))
	if err != nil {
		return fmt.Errorf("failed to glob runlevel links for %s: %w", service, err)
	}
	for _, link := range links {
		if err := exec.Command("sudo", "rm", "-f", link).Run(); err != nil {
			return fmt.Errorf("failed to remove runlevel link %s: %w", link, err)
		}
	}
	return nil
}
