package image

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"os"
)

// BootImgHeader represents Android boot image header (v1/v2 simplified)
// See: https://android.googlesource.com/platform/system/tools/mkbootimg/+/refs/heads/master/include/bootimg/bootimg.h
type BootImgHeader struct {
	Magic          [8]uint8
	KernelSize     uint32
	KernelAddr     uint32
	RamdiskSize    uint32
	RamdiskAddr    uint32
	SecondSize     uint32
	SecondAddr     uint32
	TagsAddr       uint32
	PageSize       uint32
	HeaderVersion  uint32
	OSVersion      uint32
	Name           [16]uint8
	Cmdline        [512]uint8
	ID             [32]uint8
	ExtraCmdline   [1024]uint8
}

const (
    BOOT_MAGIC = "ANDROID!"
    PAGE_SIZE  = 2048
)

// CreateBootImage creates a simple Android boot image
func CreateBootImage(outPath string, kernelPath, ramdiskPath, cmdline string, baseAddr, kernelOffset, ramdiskOffset, secondOffset, tagsOffset uint32, pageSize uint32) error {
    kernel, err := os.ReadFile(kernelPath)
    if err != nil {
        return fmt.Errorf("read kernel: %w", err)
    }
    
    ramdisk, err := os.ReadFile(ramdiskPath)
    if err != nil {
        return fmt.Errorf("read ramdisk: %w", err)
    }
    
    // Calculate addresses based on base address and offsets
    kernelAddr := baseAddr + kernelOffset
    ramdiskAddr := baseAddr + ramdiskOffset
    secondAddr := baseAddr + secondOffset
    tagsAddr := baseAddr + tagsOffset
    
    hdr := BootImgHeader{
        KernelSize: uint32(len(kernel)),
        KernelAddr: kernelAddr,
        RamdiskSize: uint32(len(ramdisk)),
        RamdiskAddr: ramdiskAddr,
        SecondSize: 0,
        SecondAddr: secondAddr,
        TagsAddr: tagsAddr,
        PageSize: pageSize,
        HeaderVersion: 0, // Legacy/v0 is safest for S4
    }
    copy(hdr.Magic[:], BOOT_MAGIC)
    copy(hdr.Cmdline[:], cmdline)
    
    // Create File
    f, err := os.Create(outPath)
    if err != nil {
        return err
    }
    defer f.Close()
    
    // Write Header
    if err := binary.Write(f, binary.LittleEndian, &hdr); err != nil {
        return err
    }
    
    // Padding after header
    if err := padToPageSize(f, uint32(binary.Size(hdr)), pageSize); err != nil { return err }
    
    // Write Kernel
    if _, err := f.Write(kernel); err != nil { return err }
    if err := padToPageSize(f, uint32(len(kernel)), pageSize); err != nil { return err }

    // Write Ramdisk
    if _, err := f.Write(ramdisk); err != nil { return err }
    if err := padToPageSize(f, uint32(len(ramdisk)), pageSize); err != nil { return err }
    
    return nil
}

func padToPageSize(f *os.File, actualSize, pageSize uint32) error {
    rem := actualSize % pageSize
    if rem == 0 {
        return nil
    }
    pad := pageSize - rem
    _, err := f.Write(bytes.Repeat([]byte{0}, int(pad)))
    return err
}
