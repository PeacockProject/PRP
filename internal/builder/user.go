package builder

import (
	"fmt"
	"os/exec"
	"path/filepath"
	"strings"

	"peacock/internal/runner"
)

// CreateUserInRootfs creates a user inside the target rootfs.
// Requires shadow (useradd/chpasswd) to be present in the rootfs.
func (b *Builder) CreateUserInRootfs(imageChrootRoot, rootfsPath, username, password string) error {
	if strings.TrimSpace(username) == "" {
		return nil
	}
	if strings.TrimSpace(password) == "" {
		return fmt.Errorf("password required for user '%s'", username)
	}

	passHash, hashErr := hashPasswordSHA512(password)
	if hashErr != nil {
		return hashErr
	}
	if userExistsInRootfs(rootfsPath, username) {
		return setShadowPassword(rootfsPath, username, passHash)
	}

	hostRootfsMount := filepath.Join(imageChrootRoot, "mnt", "rootfs")
	if err := runner.RunCmd(exec.Command("sudo", "mkdir", "-p", hostRootfsMount)); err != nil {
		return fmt.Errorf("failed to create rootfs mount dir: %w", err)
	}
	if err := runner.RunCmd(exec.Command("sudo", "mount", "--bind", rootfsPath, hostRootfsMount)); err != nil {
		return fmt.Errorf("failed to bind mount rootfs: %w", err)
	}
	defer exec.Command("sudo", "umount", hostRootfsMount).Run()

	// Ensure "users" group exists to satisfy useradd default group configuration.
	groupCheck := exec.Command("sudo", "chroot", imageChrootRoot, "getent", "group", "users")
	if err := runner.RunCmd(groupCheck); err != nil {
		_ = runner.RunCmd(exec.Command("sudo", "chroot", imageChrootRoot, "groupadd", "-g", "100", "users"))
	}

	useraddPaths := []string{"/usr/bin/useradd", "/usr/sbin/useradd"}
	var useraddErr error
	for _, p := range useraddPaths {
		useradd := exec.Command("sudo", "chroot", imageChrootRoot,
			p, "--root", "/mnt/rootfs", "-m", "-s", "/bin/bash", "-p", passHash, username,
		)
		useradd.Stdout = runner.LogWriter()
		useradd.Stderr = runner.LogWriter()
		if err := runner.RunCmd(useradd); err == nil {
			useraddErr = nil
			break
		} else {
			useraddErr = err
		}
	}
	if useraddErr != nil {
		for _, p := range useraddPaths {
			useradd := exec.Command("sudo", "chroot", imageChrootRoot,
				"/usr/bin/qemu-arm-static", "-L", "/mnt/rootfs", "/mnt/rootfs"+p, "-m", "-s", "/bin/bash", "-p", passHash, username,
			)
			useradd.Stdout = runner.LogWriter()
			useradd.Stderr = runner.LogWriter()
			if err := runner.RunCmd(useradd); err == nil {
				useraddErr = nil
				break
			} else {
				useraddErr = err
			}
		}
	}
	if useraddErr != nil {
		if userExistsInRootfs(rootfsPath, username) {
			return setShadowPassword(rootfsPath, username, passHash)
		}
		return fmt.Errorf("useradd failed: %w", useraddErr)
	}

	return nil
}

func hashPasswordSHA512(password string) (string, error) {
	py := `import crypt,sys; print(crypt.crypt(sys.argv[1], crypt.mksalt(crypt.METHOD_SHA512)))`
	out, err := exec.Command("python3", "-c", py, password).CombinedOutput()
	if err == nil {
		return strings.TrimSpace(string(out)), nil
	}
	opensslOut, err2 := exec.Command("openssl", "passwd", "-6", password).CombinedOutput()
	if err2 == nil {
		return strings.TrimSpace(string(opensslOut)), nil
	}
	return "", fmt.Errorf("password hash failed (python3: %v, openssl: %v)", err, err2)
}

func userExistsInRootfs(rootfsPath, username string) bool {
	script := `
import os,sys
root = sys.argv[1]
user = sys.argv[2]
passwd = os.path.join(root, "etc", "passwd")
try:
    with open(passwd, "r") as f:
        for line in f:
            if line.startswith(user + ":"):
                sys.exit(0)
except FileNotFoundError:
    pass
sys.exit(1)
`
	err := exec.Command("sudo", "python3", "-c", script, rootfsPath, username).Run()
	return err == nil
}

func setShadowPassword(rootfsPath, username, hash string) error {
	script := `
import os,sys,tempfile
root = sys.argv[1]
user = sys.argv[2]
phash = sys.argv[3]
shadow = os.path.join(root, "etc", "shadow")
with open(shadow, "r") as f:
    lines = f.readlines()
found = False
out = []
for line in lines:
    if line.startswith(user + ":"):
        parts = line.rstrip("\n").split(":")
        if len(parts) < 2:
            raise SystemExit("invalid shadow entry for user")
        parts[1] = phash
        out.append(":".join(parts) + "\n")
        found = True
    else:
        out.append(line)
if not found:
    raise SystemExit("user not found in shadow")
dirpath = os.path.dirname(shadow)
fd, tmp = tempfile.mkstemp(dir=dirpath, prefix=".shadow.", text=True)
with os.fdopen(fd, "w") as f:
    f.writelines(out)
os.replace(tmp, shadow)
`
	cmd := exec.Command("sudo", "python3", "-c", script, rootfsPath, username, hash)
	cmd.Stdout = runner.LogWriter()
	cmd.Stderr = runner.LogWriter()
	if err := runner.RunCmd(cmd); err != nil {
		return fmt.Errorf("shadow update failed: %w", err)
	}
	return nil
}
