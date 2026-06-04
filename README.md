<img width="1691" height="562" alt="App Sandbox" src="https://github.com/user-attachments/assets/d77e9d01-0bd9-48c6-9231-f35ff05b340b" />  

# App Sandbox
  
App Sandbox is a virtual machine app for Windows and macOS that's focused on performance and ease of use.

Windows features:
- Works on Windows 11 Home or Pro, without Hyper-V
- Windows 11 or Ubuntu 26.04 LTS VM Support
- Zero touch install
- Copy and Paste
- 2 Channel Audio
- GPU Acceleration via Paravirtualization (GPU-PV) with support for DirectX 12 (Windows only), OpenGL, Vulkan, CUDA, OpenCL
- GPU Hardware Video Decoder/Encoder support
- SSH via Hyper-V socket proxy
- Snapshots
- Fixed 1080P60 display
- Host to client hot-key support
- Provision and boot with / without internet
- Supports running Claude Cowork and Docker inside the VM thanks to Nested Virtualization Support

Mac features:
- macOS VM support
- Skips most of the install setup process
- Copy and Paste
- 2 Channel Audio
- GPU Acceleration via Paravirtualization with support for Metal
- SSH via 
- Dynamic display sizing
- Provision and boot with / without internet

Requirements:
Windows 11 with an x64 Processor or macOS Tahoe (M Series)

# Tips:
[Windows] Enable hotkeys or mute the VM audio: connect to the VM and right-click the connection title bar  
[Windows] Need a high performance remote desktop to remotely access your VM? [Phaze](https://phaze.app) works well


# Info
Host Disk Layout:
Windows:
```
%ProgramData%\AppSandbox\   ← default C:\ProgramData\AppSandbox\  (all-users, since it runs elevated under HCS)
├── vms.cfg                 ← VM list + settings (INI: name, RAM, CPU, disk, network, ssh…) — same format as macOS
├── Windows11\              ← one folder per VM (a Windows guest, here)
│   ├── disk.vhdx           ← the guest's disk (dynamic VHDX; built from your ISO by iso-patch.exe)
│   ├── resources.iso       ← "APPSETUP" ISO: autounattend.xml + guest agent + VDD/VAD drivers + setup scripts
│   ├── vm_state.json       ← per-VM state/metadata (install progress, etc.)
│   ├── vm.vmrs             ← HCS runtime-state file (saved RAM/device state for the VM)
│   └── snapshots\          ← snapshot tree (a Windows-only feature)
│       ├── tree.dat                ← snapshot/branch tree metadata
│       ├── snapshot_<guid>.vhdx    ← differencing disk captured at a snapshot point
│       └── branch_<guid>.vhdx      ← differencing disk for a branch (working copy off a snapshot)
├── Ubuntu\                 ← a Linux guest — identical file layout (disk.vhdx, resources.iso, vm_state.json, vm.vmrs, snapshots\)
│   └── …                       (the Ubuntu agent + asb_drm/dxgkrnl/mesa are staged *inside* disk.vhdx)
└── templates\              ← VMs created "as a template" live here (Windows-only)
    └── <template name>\        (same disk.vhdx / resources.iso / vm.vmrs / <name>.json layout)
```

macOS:
```
~/Library/Application Support/AppSandbox/
├── vms.cfg                 ← VM list + settings (INI: name, RAM, CPU, disk, network, ssh…)
├── restore.ipsw            ← cached macOS restore image, shared across VM creates (18 GB)
└── VMs/                    ← one folder per VM
    └── MyAppSandbox/
        ├── disk.img        ← the guest's disk (sparse, 64 GB apparent / 28 GB real)
        ├── aux.img         ← VZMacAuxiliaryStorage — the VM's NVRAM / EFI store (32 MB)
        ├── hardware.bin    ← VZMacHardwareModel (the virtual Mac model, 132 B)
        └── machine-id.bin  ← VZMacMachineIdentifier (the VM's unique identity, 60 B)
```

## License

AppSandbox is licensed under the [MIT License](LICENSE) — Copyright (c) 2026 James Stringer.

It bundles third-party components that are **not** covered by the MIT license and
retain their own terms — most notably the Microsoft WSL2 `dxgkrnl` GPU driver
(GPL-2.0) and the embedded XZ decoder (0BSD). See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for the full list. AppSandbox's
own Linux DRM kernel module (`tools/linux/asb_drm/`) is dual-licensed MIT OR
GPL-2.0 so it can resolve the kernel's GPL-only symbols.

## Acknowledgements

Beyond my own experience building [Easy-GPU-PV](https://github.com/jamesstringerparsec/Easy-GPU-PV), I found [NanaBox](https://github.com/M2Team/NanaBox) to be a really helpful resource for understanding HCS.

## Author

[James Stringer](https://www.linkedin.com/in/jamesstringerphotography/)
