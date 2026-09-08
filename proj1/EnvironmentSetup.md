# Environment Setup

This guide provides detailed instructions for setting up and testing your project on Microsoft Azure.

## 1. Getting Started

Azure offers $100 in credits to students when they sign up with their university email. With efficient cost management (e.g., deallocation discussed below), this credit should suffice for testing your project. Sign up for Azure for Students here: [Azure for Students](https://azure.microsoft.com/en-us/free/students).

You will need a virtual machine with at least **4 CPUs and 16 GB RAM** that supports nested virtualization. Refer to the [Azure Compute Units (ACU)](https://docs.microsoft.com/en-us/azure/virtual-machines/acu) page which lists all Azure compute units, and machines supporting nested virtualization are marked with a triple asterisk. **Recommended instance type** is `D4s_v3`.


## 2. Setting Up the Instance

**Install Azure CLI**: Before creating your instance, you will need to have the Azure CLI installed on your computer. 

- Follow the [Azure CLI installation guide](https://learn.microsoft.com/en-us/cli/azure/install-azure-cli) for your system. 
- Once installed, check everything is working fine with `az --version`.
- Log into the Azure CLI with `az login`.
- If you have multiple subscriptions, use `az account set --subscription <subscription_id>`.

**Create a Resource Group**: Run the following command now:
```shell
az group create --name 'pr1-vm-rg' --location 'eastus'
```
If everything is fine, you will see an output like this on your console:
```json
{
  "id": "/subscriptions/ba7da08b-ce22-40dc-93fc-f2be207e0269/resourceGroups/pr1-vm-rg",
  "location": "eastus",
  "managedBy": null,
  "name": "pr1-vm-rg",
  "properties": {
    "provisioningState": "Succeeded"
  },
  "tags": null,
  "type": "Microsoft.Resources/resourceGroups"
}
```

**Create the VM**: You need to run the following command to create a standard `D4s_v3` instance. 

If you are on Mac or Linux use this:

```shell
az vm create \
--name pr1-vm \
--resource-group pr1-vm-rg \
--size Standard_D4s_v3 \
--location eastus \
--image Canonical:0001-com-ubuntu-server-focal:20_04-lts:latest \
--admin-username azureuser \
--generate-ssh-keys \
--public-ip-sku Standard \
--verbose
```

If you are on Windows and using Powershell or Windows Terminal use this:

```powershell
az vm create `
--name pr1-vm `
--resource-group pr1-vm-rg `
--size Standard_D4s_v3 `
--location eastus `
--image Canonical:0001-com-ubuntu-server-focal:20_04-lts:latest `
--admin-username azureuser `
--generate-ssh-keys `
--public-ip-sku Standard `
--verbose
```

If successful, you will get a JSON output like this:

```json
{
  "fqdns": "",
  "id": "/subscriptions/ba7da08b-ce22-40dc-93fc-f2be207e0269/resourceGroups/pr1-vm-rg/providers/Microsoft.Compute/virtualMachines/pr1-vm",
  "location": "eastus",
  "macAddress": "7C-1E-52-69-C3-50",
  "powerState": "VM running",
  "privateIpAddress": "10.0.0.4",
  "publicIpAddress": "172.191.59.113",
  "resourceGroup": "pr1-vm-rg",
  "zones": ""
}
```

**Note**: Depending on which region you are, sometimes Azure could throw an error saying `Message: Resource 'pr1-vm___' was disallowed by Azure: This policy maintains a set of best available regions where your subscription can deploy resources. The objective of this policy is to ensure that your subscription has full access to Azure services with optimal performance. Should you need additional or different regions, contact support.` In that use the following to find out the allowed regions: 

```bash
az policy assignment list --query "[?displayName=='Allowed resource deployment regions'].{Name:displayName, Parameters:parameters}" --output json
```

Alternatively, you could use Azure portal to check the available regions for you. You can go to Azure Policy -- Authoring -- Assignments here https://portal.azure.com/#view/Microsoft_Azure_Policy/PolicyMenuBlade/%7E/Assignments and then click on "Allowed resource deployment regions". Then click on "View Assignments" to see the allowed regions.


**Connect to the VM**: To connect to the VM, run the command below, replacing `PUBLIC-IP-ADDRESS` with the `publicIpAddress` in the JSON output from VM creation. Alternatively, you can go to the next step for setting up Visual Studio Code with Remote Development.

```shell
ssh azureuser@<PUBLIC-IP-ADDRESS>
```

You should now be logged in to the virtual machine, and can proceed to setup project 1 on it for testing.

## 3. Setting up Visual Studio Code
Visual Studio Code's Remote Development features allow you to develop directly on your Azure VM.

- Download and install Visual Studio Code: [Download VS Code](https://code.visualstudio.com/).

- Install the "Remote Development" extension from the VS Code marketplace. Check the documentation [here](https://code.visualstudio.com/docs/remote/ssh-tutorial).

- Connect to your VM:
   - Press `Ctrl+Shift+P` in VS Code and select `Remote-SSH: Connect to Host`.
   - Add a new SSH host and enter your SSH connection string when prompted:

      ```shell
      ssh azureuser@<PUBLIC-IP-ADDRESS>
      ```

   - The host will be saved and you will now be connected to your Azure VM in the VS Code editor.

- Open the terminal in VS Code and run necessary commands. 
- Now, you can use VS Code to edit your project files and run/debug directly on the VM.

## 4. Installing Dependencies
Now, we will install the necessary dependencies for our project. Run this command:

```shell
  sudo apt-get -y update && \
  DEBIAN_FRONTEND=noninteractive sudo apt-get -y -o Dpkg::Options::="--force-confdef" -o Dpkg::Options::="--force-confold" upgrade && \
  DEBIAN_FRONTEND=noninteractive sudo apt-get -y install \
  build-essential \
  qemu-kvm \
  uvtool \
  libvirt-dev \
  libvirt-daemon-system \
  python3-libvirt \
  python-is-python3 \
  python3-pandas \
  python3-matplotlib \
  virt-top \
  virt-manager \
  tmux \
  git \
  zip \
  unzip && \
  sudo uvt-simplestreams-libvirt sync release=bionic arch=amd64
```

## 5. Update SSH Configuration
Next, update the SSH key config so we can easily access the nested VMs when they are created. Run the following command:

```shell
  echo "Host 192.168.*" > $HOME/.ssh/config && \
  echo -e "\tStrictHostKeyChecking no" >> $HOME/.ssh/config && \
  echo -e "\tUserKnownHostsFile /dev/null" >> $HOME/.ssh/config && \
  echo -e "\tLogLevel ERROR" >> $HOME/.ssh/config && \
  ssh-keygen -t rsa -q -f "$HOME/.ssh/id_rsa" -N ""
```

## 6. Testing the project

To test your implementation on the instance, you need to download your code onto the instance. If you're already using a private Git repository for your work (which is highly recommended), you can clone your project on the instance with `git`. Otherwise, you can use `SFTP` or any other convenient method.

Once you have cloned your project, you can proceed to run the CPU and memory tests on it.

   
## 7. Deallocating an instance

In order to avoid burning our student credits (or personal funds) unnecessarily, we can deallocate our instance when it is not actively in use. Please note that on Azure, merely stopping an instance does not stop you from incurring costs on other instance resources such as storage. For this reason, we need to deallocate the instance so that we free these resources.

To deallocate an instance, run this command:

```shell
az vm deallocate --name pr1-vm --resource-group pr1-vm-rg
```

You can start the VM again with but be careful to check the public IP:

```shell
az vm start --name pr1-vm --resource-group pr1-vm-rg
```

## Creating Test VMs

The most convenient way to create test VMs for this project is via Ubuntu's `uvtool` utility. This tool provides pre-built Ubuntu VM images for use within a libvirt/KVM environment. The packages you installed in the previous section include everything needed to create, run, and connect to your virtual machines.

We will be running Ubuntu 18.04 VMs for testing purposes. The `uvt-simplestreams-libvirt sync` command from the previous section has already downloaded the 18.04 (bionic) image to your environment.


 1. Create a new virtual machine, where *aos_vm1* is the name of the VM you want to create: 
    ``` shell
    uvt-kvm create aos_vm1 release=bionic --memory=512
    ```
    **Note: Your VM names must start with *aos_* for testing purposes!**
 
 2. Wait for the virtual machine to boot up and start the SSH daemon:
    ``` shell
    uvt-kvm wait aos_vm1
    ```

 3. (Optional) Connect to the running VM:
    ``` shell
    uvt-kvm ssh aos_vm1 --insecure
    ```
## Other Useful Commands

 1. Delete a VM, where *aos_vm1* is the name of the VM you want to delete: 

    ``` shell
    uvt-kvm destroy aos_vm1
    ```
 
 2. List all your VMs: 
 
    ``` shell
    virsh list --all
    ```
    
 3. Track resource usage across VMs: 
 
    ``` shell
    virt-top
    ```
 ... and plenty more. These are just a few commands to get you started.
 

## References:

1. [Azure compute unit (ACU)](https://docs.microsoft.com/en-us/azure/virtual-machines/acu)
2. [Azure for Students](https://azure.microsoft.com/en-us/free/students)
3. [How to install the Azure CLI](https://learn.microsoft.com/en-us/cli/azure/install-azure-cli)
4. [Virtualization](https://help.ubuntu.com/lts/serverguide/virtualization.html)