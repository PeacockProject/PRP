package runner

import (
	"bytes"
	"context"
	"io"
	"os"
	"os/exec"
	"sync"
	"syscall"
	"time"
)

var (
	logWriter io.Writer       = os.Stdout
	ctx       context.Context = context.Background()
	mu        sync.Mutex
)

// SetLogWriter sets the writer for command stdout/stderr.
func SetLogWriter(w io.Writer) {
	if w == nil {
		return
	}
	mu.Lock()
	defer mu.Unlock()
	logWriter = w
}

// LogWriter returns the current log writer.
func LogWriter() io.Writer {
	mu.Lock()
	defer mu.Unlock()
	return logWriter
}

// SetContext sets the context used for command execution.
func SetContext(c context.Context) {
	if c == nil {
		return
	}
	ctx = c
}

// Run runs the command and streams stdout/stderr to the log writer.
func Run(name string, args ...string) error {
	cmd := exec.Command(name, args...)
	cmd.Stdout = LogWriter()
	cmd.Stderr = LogWriter()
	return runCmd(cmd)
}

// RunCmd runs the provided command, wiring stdout/stderr to the log writer.
func RunCmd(cmd *exec.Cmd) error {
	if cmd.Stdout == nil {
		cmd.Stdout = LogWriter()
	}
	if cmd.Stderr == nil {
		cmd.Stderr = LogWriter()
	}
	return runCmd(cmd)
}

// RunOutput runs a command and returns its stdout while logging it too.
func RunOutput(cmd *exec.Cmd) (string, error) {
	var buf bytes.Buffer
	cmd.Stdout = io.MultiWriter(&buf, LogWriter())
	if cmd.Stderr == nil {
		cmd.Stderr = LogWriter()
	}
	err := runCmd(cmd)
	return buf.String(), err
}

func runCmd(cmd *exec.Cmd) error {
	// Ensure separate process group for signal cleanup.
	if cmd.SysProcAttr == nil {
		cmd.SysProcAttr = &syscall.SysProcAttr{}
	}
	cmd.SysProcAttr.Setpgid = true

	if err := cmd.Start(); err != nil {
		return err
	}

	done := make(chan error, 1)
	go func() {
		done <- cmd.Wait()
	}()

	select {
	case err := <-done:
		return err
	case <-ctx.Done():
		_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGTERM)
		select {
		case err := <-done:
			return err
		case <-time.After(2 * time.Second):
			_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
			return <-done
		}
	}
}
