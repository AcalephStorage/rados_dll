# -*- mode: ruby -*-
# vi: set ft=ruby :

# All Vagrant configuration is done below. The "2" in Vagrant.configure
# configures the configuration version (we support older styles for
# backwards compatibility). Please don't change it unless you know what
# you're doing.
Vagrant.configure(2) do |config|

  # Every Vagrant virtual environment requires a box to build off of.
  config.vm.box = 'ubuntu/trusty64'

  VM_IP = 41
  VM_NAME = "rados#{VM_IP}"

  config.vm.hostname = VM_NAME

  # Create a private network, which allows host-only access to the machine
  # using a specific IP.
  config.vm.network 'private_network', ip: "192.168.73.#{VM_IP}"

  # # Mount the "vagrant" folder at "/vagrant" with all permission bits open
  # config.vm.synced_folder '.', '/vagrant', owner: 'vagrant', group: 'vagrant', mount_options: ['dmode=777,fmode=777']

  config.vm.provider 'virtualbox' do |vbox|
    vbox.name = VM_NAME
    vbox.memory = 2048
    vbox.cpus = 2
  end

  config.vm.provision 'ansible' do |ansible|
    ansible.playbook = 'vagrant-provision.yml'
  end

  # If true, then any SSH connections made will enable agent forwarding.
  # Default value: false
  config.ssh.forward_agent = true

end
