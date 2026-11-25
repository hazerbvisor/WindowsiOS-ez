// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
#if canImport(FoundationXML)
import FoundationXML
#endif

enum WindowsUnattend {
    /// Preselect the image while leaving disk selection and partitioning in Setup.
    /// Windows disk numbers can differ from the host PCI slot ordering.
    static func make(imageIndex: Int) throws -> Data {
        guard imageIndex > 0, imageIndex <= Int(UInt32.max) else { throw VMError.message("Select a valid Windows image.") }
        let answer = Data("""
        <?xml version="1.0" encoding="utf-8"?>
        <unattend xmlns="urn:schemas-microsoft-com:unattend" xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State">
          <settings pass="windowsPE">
            <component name="Microsoft-Windows-Setup" processorArchitecture="arm64" publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS">
              <ImageInstall>
                <OSImage>
                  <InstallFrom>
                    <MetaData wcm:action="add"><Key>/IMAGE/INDEX</Key><Value>\(imageIndex)</Value></MetaData>
                  </InstallFrom>
                  <WillShowUI>Always</WillShowUI>
                </OSImage>
              </ImageInstall>
            </component>
          </settings>
        </unattend>
        """.utf8)
        let parser = XMLParser(data: answer)
        parser.shouldProcessNamespaces = true
        guard parser.parse() else {
            throw VMError.message("The generated Windows Setup answer file is not valid XML.")
        }
        return answer
    }
}
