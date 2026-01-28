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
  { :vendor => "0x1e71", :product => "0x2014" },
  { :vendor => "0x1e71", :product => "0x3008" },
  # nzxt-smart2
  { :vendor => "0x1e71", :product => "0x2006" },
  { :vendor => "0x1e71", :product => "0x200d" },
  { :vendor => "0x1e71", :product => "0x2009" },
  { :vendor => "0x1e71", :product => "0x200e" },
  { :vendor => "0x1e71", :product => "0x200f" },
  { :vendor => "0x1e71", :product => "0x2010" },
  { :vendor => "0x1e71", :product => "0x2011" },
  { :vendor => "0x1e71", :product => "0x2019" },
]

Vagrant.configure("2") do |config|

  config.vm.provider "virtualbox" do |virtualbox, override|
    override.vm.box = "fedora/43-cloud-base"
    override.vm.box_url = "https://download.fedoraproject.org/pub/fedora/linux/releases/43/Cloud/x86_64/images/Fedora-Cloud-Base-Vagrant-VirtualBox-43-1.6.x86_64.vagrant.virtualbox.box"

    virtualbox.cpus = CPUS
    virtualbox.memory = MEMORY
    virtualbox.default_nic_type = "virtio"

    virtualbox.customize ["modifyvm", :id, "--usbxhci", "on"]

    # HACK: only create usb filters once, if vm doesn't exist yet
    if not File.exist? File.join(".vagrant", "machines", "default", "virtualbox", "id")
      USB_IDS.each_with_index do |usb_id, index|
        virtualbox.customize ["usbfilter", "add", index.to_s, "--target", :id, "--name", "dev-#{usb_id[:vendor]}-#{usb_id[:product]}", "--vendorid", usb_id[:vendor], "--productid", usb_id[:product]]
      end
    end
  end

  config.vm.provider "libvirt" do |libvirt, override|
    override.vm.box = "fedora/43-cloud-base"
    override.vm.box_url = "https://download.fedoraproject.org/pub/fedora/linux/releases/43/Cloud/x86_64/images/Fedora-Cloud-Base-Vagrant-libvirt-43-1.6.x86_64.vagrant.libvirt.box"

    libvirt.cpus = CPUS
    libvirt.memory = MEMORY

    # Error while creating domain: Error saving the server: Call to virDomainDefineXML failed:
    # unsupported configuration: chardev 'spicevmc' not supported without spice graphics
    libvirt.graphics_type = "spice"
    libvirt.channel :type => "spicevmc", :target_name => "com.redhat.spice.0", :target_type => "virtio"

    libvirt.usb_controller :model => "qemu-xhci"
    libvirt.redirdev :type => "spicevmc"

    USB_IDS.each do |usb_id|
      libvirt.usb usb_id.merge :startupPolicy => "optional"
    end

    if Vagrant.has_plugin?("vagrant-libvirt", "> 0.5.3")
      libvirt.channel :type => "unix", :target_name => "org.qemu.guest_agent.0", :target_type => "virtio"
      libvirt.qemu_use_agent = true
    end
  end

  config.vm.provision "ansible" do |ansible|
    ansible.playbook = "tools/vagrant/cd-to-vagrant.yml"
  end

  config.vm.provision "ansible" do |ansible|
    ansible.playbook = "tools/vagrant/dev-tools.yml"
  end

end
