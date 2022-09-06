# -*- mode: ruby -*-
# vi: set ft=ruby :

CPUS = 4
MEMORY = 1024
USB_IDS = [
  # nzxt-grid3
  { :vendor => "0x1e71", :product => "0x1711" },
  { :vendor => "0x1e71", :product => "0x1714" },
  # nzxt-kraken2
  { :vendor => "0x1e71", :product => "0x170e" },
  # nzxt-kraken3
  { :vendor => "0x1e71", :product => "0x2007" },
  # nzxt-smart2
  { :vendor => "0x1e71", :product => "0x2006" },
  { :vendor => "0x1e71", :product => "0x200d" },
  { :vendor => "0x1e71", :product => "0x2009" },
  { :vendor => "0x1e71", :product => "0x200e" },
  { :vendor => "0x1e71", :product => "0x200f" },
  { :vendor => "0x1e71", :product => "0x2010" },
]

Vagrant.configure("2") do |config|
  config.vm.box = "Fedora-Cloud-Base-Vagrant-34"
  config.vm.box_download_checksum_type = "sha256"

  config.vm.provider "virtualbox" do |virtualbox, override|
    override.vm.box_url = "https://download.fedoraproject.org/pub/fedora/linux/releases/34/Cloud/x86_64/images/Fedora-Cloud-Base-Vagrant-34-1.2.x86_64.vagrant-virtualbox.box"
    override.vm.box_download_checksum = "e72d9987c61d58108910fab700e8bdf349e69d2e158037a10b07706a68446fda"

    virtualbox.cpus = CPUS
    virtualbox.memory = MEMORY
    virtualbox.default_nic_type = "virtio"

    virtualbox.customize ["modifyvm", :id, "--usbxhci", "on"]

    # HACK: only create usb filters once, if vm doesn't exist yet
    if not File.exists? File.join(".vagrant", "machines", "default", "virtualbox", "id")
      USB_IDS.each_with_index do |usb_id, index|
        virtualbox.customize ["usbfilter", "add", index.to_s, "--target", :id, "--name", "dev-#{usb_id[:vendor]}-#{usb_id[:product]}", "--vendorid", usb_id[:vendor], "--productid", usb_id[:product]]
      end
    end
  end

  config.vm.provider "libvirt" do |libvirt, override|
    override.vm.box_url = "https://download.fedoraproject.org/pub/fedora/linux/releases/34/Cloud/x86_64/images/Fedora-Cloud-Base-Vagrant-34-1.2.x86_64.vagrant-libvirt.box"
    override.vm.box_download_checksum = "3d9c00892253c869bffcf2e84ddd308e90d5c7a5928b3bc00e0563a4bec55849"

    libvirt.cpus = CPUS
    libvirt.memory = MEMORY

    libvirt.usb_controller :model => "qemu-xhci"
    libvirt.redirdev :type => "spicevmc"

    USB_IDS.each do |usb_id|
      libvirt.usb usb_id.merge :startupPolicy => "optional"
    end
  end

  config.vm.provision "ansible" do |ansible|
    ansible.playbook = "tools/vagrant/cd-to-vagrant.yml"
  end

  config.vm.provision "ansible" do |ansible|
    ansible.playbook = "tools/vagrant/dev-tools.yml"
  end

end
