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
  config.vm.box = "fedora/37-cloud-base"

  config.vm.provider "virtualbox" do |virtualbox, override|
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
  end

  config.vm.provision "ansible" do |ansible|
    ansible.playbook = "tools/vagrant/cd-to-vagrant.yml"
  end

  config.vm.provision "ansible" do |ansible|
    ansible.playbook = "tools/vagrant/dev-tools.yml"
  end

end
