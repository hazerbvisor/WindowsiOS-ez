// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

enum WindowsUnattend {
    /// Windows Setup handles installation and OOBE. This answer file only chooses
    /// the edition and partitions a primary disk the host has verified is blank.
    static func make(imageIndex: Int) throws -> Data {
        guard imageIndex > 0 else { throw VMError.message("Select a valid Windows image.") }
        return Data("""
        <?xml version="1.0" encoding="utf-8"?>
        <unattend xmlns="urn:schemas-microsoft-com:unattend" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State">
          <settings pass="windowsPE">
            <component name="Microsoft-Windows-Setup" processorArchitecture="arm64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
              <DiskConfiguration>
                <Disk wcm:action="add">
                  <DiskID>0</DiskID>
                  <WillWipeDisk>false</WillWipeDisk>
                  <CreatePartitions>
                    <CreatePartition wcm:action="add"><Order>1</Order><Type>EFI</Type><Size>260</Size></CreatePartition>
                    <CreatePartition wcm:action="add"><Order>2</Order><Type>MSR</Type><Size>16</Size></CreatePartition>
                    <CreatePartition wcm:action="add"><Order>3</Order><Type>Primary</Type><Extend>true</Extend></CreatePartition>
                  </CreatePartitions>
                  <ModifyPartitions>
                    <ModifyPartition wcm:action="add"><Order>1</Order><PartitionID>1</PartitionID><Format>FAT32</Format><Label>System</Label></ModifyPartition>
                    <ModifyPartition wcm:action="add"><Order>2</Order><PartitionID>3</PartitionID><Format>NTFS</Format><Label>Windows</Label></ModifyPartition>
                  </ModifyPartitions>
                </Disk>
                <WillShowUI>OnError</WillShowUI>
              </DiskConfiguration>
              <ImageInstall>
                <OSImage>
                  <InstallFrom>
                    <MetaData wcm:action="add"><Key>/IMAGE/INDEX</Key><Value>\(imageIndex)</Value></MetaData>
                  </InstallFrom>
                  <InstallTo><DiskID>0</DiskID><PartitionID>3</PartitionID></InstallTo>
                  <WillShowUI>OnError</WillShowUI>
                </OSImage>
              </ImageInstall>
            </component>
          </settings>
        </unattend>
        """.utf8)
    }
}
