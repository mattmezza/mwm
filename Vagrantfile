Vagrant.configure("2") do |config|
  config.vm.box = "generic/arch"

  config.vm.provider :libvirt do |lv|
    lv.memory = 2048
    lv.cpus = 2
    lv.graphics_type = "spice"
    lv.video_type = "qxl"
    lv.video_vram = 256

    # Correct array formatting for passing multi-monitor arguments to QEMU
    lv.qemuargs :value => ["-device", "qxl-vga,id=video0,max_outputs=2"]
  end

  config.vm.provision "shell", inline: <<-SHELL
    # 1. Fix the outdated keyring first without updating packages yet
    echo "Updating Arch keyring..."
    pacman -Sy --noconfirm archlinux-keyring

    # 2. Now safe to do a full system upgrade and install requested packages
    echo "Installing development dependencies..."
    pacman -Syu --noconfirm
    pacman -S --noconfirm base-devel git zsh neovim xorg-server xorg-xinit openssh nodejs npm \
      libx11 libxft libxinerama libxext fontconfig \
      xorg-xrandr arandr xorg-xprop xorg-xsetroot xterm sxhkd picom dmenu ttf-dejavu \
      spice-vdagent

    # 3. Ensure SSHD and the SPICE guest agent are running
    systemctl enable --now sshd
    systemctl enable --now spice-vdagentd

    # 4. Install Pi Agent by bypassing the TTY check (passing standard input)
    echo "Installing Pi Agent..."
    curl -fsSL https://pi.dev/install.sh | env CI=true sh
  SHELL

  # Sync the current directory to /vagrant inside the VM using 9p protocol
  config.vm.synced_folder ".", "/vagrant", type: "9p", disabled: false, accessmode: "passthrough"
end
