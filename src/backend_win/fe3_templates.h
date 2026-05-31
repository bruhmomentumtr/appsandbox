/* fe3_templates.h -- SOAP request envelopes for Microsoft's FE3 delivery
 * service (the Windows Update / Microsoft Store package delivery backend).
 *
 * Used by d3dlayers.c to resolve, on the HOST, the download URL of the
 * Microsoft.D3DMappingLayers package (OpenGLOn12.dll / dxil.dll / ...)
 * straight from Microsoft's servers, then stage the DLLs into the guest.
 *
 * These are hand-authored minimal envelopes (Windows Update client protocol,
 * protocolVersion 1.40). Everything volatile is substituted at runtime via
 * tokens; nothing here is version- or host-specific:
 *
 *   %MSGID%    fresh WS-Addressing message UUID, generated per request
 *   %CREATED%  WS-Security timestamp (now, UTC), generated per request
 *   %EXPIRES%  WS-Security timestamp (now + 5 min), generated per request
 *   %COOKIE%   GetCookie's returned EncryptedData blob (SyncUpdates only)
 *   %CATID%    WuCategoryId, resolved at runtime from displaycatalog
 *   %UPDATEID% / %REVISION%  the chosen package's UpdateIdentity
 *
 * The SyncUpdates request carries only the 56 decade-stable Windows Update
 * "root" InstalledNonLeafUpdateIDs (the catalog tree roots). The large
 * OtherCachedUpdateIDs list some tooling uses is a caching hint only and is
 * intentionally omitted -- verified that the package leaf still resolves in
 * one round-trip. d3dlayers.c locates the package by identifier in the
 * response, so extra/changed entries do not break resolution.
 */

#ifndef FE3_TEMPLATES_H
#define FE3_TEMPLATES_H

#define FE3_WU_NS  "http://www.microsoft.com/SoftwareDistribution/Server/ClientWebService"

/* Generic Windows Update client device attributes (no host-specific data). */
#define FE3_DEVICE_ATTRS \
    "BranchReadinessLevel=CB;CurrentBranch=rs_prerelease;OEMModel=Virtual Machine;" \
    "FlightRing=Retail;AttrDataVer=21;SystemManufacturer=Microsoft Corporation;" \
    "InstallLanguage=en-US;OSUILocale=en-US;InstallationType=Client;" \
    "FlightingBranchName=external;FirmwareVersion=Hyper-V UEFI Release v2.5;" \
    "SystemProductName=Virtual Machine;OSSkuId=48;FlightContent=Branch;App=WU;" \
    "OEMName_Uncleaned=Microsoft Corporation;AppVer=10.0.22621.900;OSArchitecture=AMD64;" \
    "SystemSKU=None;UpdateManagementGroup=2;IsFlightingEnabled=1;IsDeviceRetailDemo=0;" \
    "TelemetryLevel=3;OSVersion=10.0.22621.900;DeviceFamily=Windows.Desktop;"

/* The 56 stable Windows Update catalog root (non-leaf) update IDs. */
#define FE3_NONLEAF_IDS \
    "<int>1</int><int>2</int><int>3</int><int>11</int><int>19</int><int>544</int>" \
    "<int>549</int><int>2359974</int><int>2359977</int><int>5169044</int><int>8788830</int>" \
    "<int>23110993</int><int>23110994</int><int>54341900</int><int>54343656</int>" \
    "<int>59830006</int><int>59830007</int><int>59830008</int><int>60484010</int>" \
    "<int>62450018</int><int>62450019</int><int>62450020</int><int>66027979</int>" \
    "<int>66053150</int><int>97657898</int><int>98822896</int><int>98959022</int>" \
    "<int>98959023</int><int>98959024</int><int>98959025</int><int>98959026</int>" \
    "<int>104433538</int><int>104900364</int><int>105489019</int><int>117765322</int>" \
    "<int>129905029</int><int>130040031</int><int>132387090</int><int>132393049</int>" \
    "<int>133399034</int><int>138537048</int><int>140377312</int><int>143747671</int>" \
    "<int>158941041</int><int>158941042</int><int>158941043</int><int>158941044</int>" \
    "<int>159123858</int><int>159130928</int><int>164836897</int><int>164847386</int>" \
    "<int>164848327</int><int>164852241</int><int>164852246</int><int>164852252</int>" \
    "<int>164852253</int>"

static const char *const FE3_TPL_GETCOOKIE =
"<Envelope xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" xmlns=\"http://www.w3.org/2003/05/soap-envelope\">"
  "<Header>"
    "<Action d3p1:mustUnderstand=\"1\" xmlns:d3p1=\"http://www.w3.org/2003/05/soap-envelope\" xmlns=\"http://www.w3.org/2005/08/addressing\">" FE3_WU_NS "/GetCookie</Action>"
    "<MessageID xmlns=\"http://www.w3.org/2005/08/addressing\">urn:uuid:%MSGID%</MessageID>"
    "<To d3p1:mustUnderstand=\"1\" xmlns:d3p1=\"http://www.w3.org/2003/05/soap-envelope\" xmlns=\"http://www.w3.org/2005/08/addressing\">https://fe3.delivery.mp.microsoft.com/ClientWebService/client.asmx</To>"
    "<Security d3p1:mustUnderstand=\"1\" xmlns:d3p1=\"http://www.w3.org/2003/05/soap-envelope\" xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">"
      "<Timestamp xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\"><Created>%CREATED%</Created><Expires>%EXPIRES%</Expires></Timestamp>"
      "<WindowsUpdateTicketsToken d4p1:id=\"ClientMSA\" xmlns:d4p1=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\" xmlns=\"http://schemas.microsoft.com/msus/2014/10/WindowsUpdateAuthorization\"><TicketType Name=\"MSA\" Version=\"1.0\" Policy=\"MBI_SSL\"><user></user></TicketType></WindowsUpdateTicketsToken>"
    "</Security>"
  "</Header>"
  "<Body>"
    "<GetCookie xmlns=\"" FE3_WU_NS "\"><oldCookie></oldCookie><lastChange>2015-10-21T17:01:07.1472913Z</lastChange><currentTime>%CREATED%</currentTime><protocolVersion>1.40</protocolVersion></GetCookie>"
  "</Body>"
"</Envelope>";

static const char *const FE3_TPL_SYNCUPDATES =
"<s:Envelope xmlns:a=\"http://www.w3.org/2005/08/addressing\" xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
  "<s:Header>"
    "<a:Action s:mustUnderstand=\"1\">" FE3_WU_NS "/SyncUpdates</a:Action>"
    "<a:MessageID>urn:uuid:%MSGID%</a:MessageID>"
    "<a:To s:mustUnderstand=\"1\">https://fe3.delivery.mp.microsoft.com/ClientWebService/client.asmx</a:To>"
    "<o:Security s:mustUnderstand=\"1\" xmlns:o=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">"
      "<Timestamp xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\"><Created>%CREATED%</Created><Expires>%EXPIRES%</Expires></Timestamp>"
      "<wuws:WindowsUpdateTicketsToken wsu:id=\"ClientMSA\" xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\" xmlns:wuws=\"http://schemas.microsoft.com/msus/2014/10/WindowsUpdateAuthorization\"><TicketType Name=\"MSA\" Version=\"1.0\" Policy=\"MBI_SSL\"><user></user></TicketType></wuws:WindowsUpdateTicketsToken>"
    "</o:Security>"
  "</s:Header>"
  "<s:Body>"
    "<SyncUpdates xmlns=\"" FE3_WU_NS "\">"
      "<cookie><Expiration>2050-01-01T00:00:00Z</Expiration><EncryptedData>%COOKIE%</EncryptedData></cookie>"
      "<parameters>"
        "<ExpressQuery>false</ExpressQuery>"
        "<InstalledNonLeafUpdateIDs>" FE3_NONLEAF_IDS "</InstalledNonLeafUpdateIDs>"
        "<OtherCachedUpdateIDs></OtherCachedUpdateIDs>"
        "<SkipSoftwareSync>false</SkipSoftwareSync>"
        "<NeedTwoGroupOutOfScopeUpdates>true</NeedTwoGroupOutOfScopeUpdates>"
        "<FilterAppCategoryIds><CategoryIdentifier><Id>%CATID%</Id></CategoryIdentifier></FilterAppCategoryIds>"
        "<TreatAppCategoryIdsAsInstalled>true</TreatAppCategoryIdsAsInstalled>"
        "<AlsoPerformRegularSync>false</AlsoPerformRegularSync>"
        "<ComputerSpec/>"
        "<ExtendedUpdateInfoParameters><XmlUpdateFragmentTypes><XmlUpdateFragmentType>Extended</XmlUpdateFragmentType></XmlUpdateFragmentTypes><Locales><string>en-US</string><string>en</string></Locales></ExtendedUpdateInfoParameters>"
        "<ClientPreferredLanguages><string>en-US</string></ClientPreferredLanguages>"
        "<ProductsParameters><SyncCurrentVersionOnly>false</SyncCurrentVersionOnly><DeviceAttributes>" FE3_DEVICE_ATTRS "</DeviceAttributes><CallerAttributes>Interactive=1;IsSeeker=0;</CallerAttributes><Products/></ProductsParameters>"
      "</parameters>"
    "</SyncUpdates>"
  "</s:Body>"
"</s:Envelope>";

static const char *const FE3_TPL_GETEXTENDEDINFO2 =
"<s:Envelope xmlns:a=\"http://www.w3.org/2005/08/addressing\" xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
  "<s:Header>"
    "<a:Action s:mustUnderstand=\"1\">" FE3_WU_NS "/GetExtendedUpdateInfo2</a:Action>"
    "<a:MessageID>urn:uuid:%MSGID%</a:MessageID>"
    "<a:To s:mustUnderstand=\"1\">https://fe3.delivery.mp.microsoft.com/ClientWebService/client.asmx/secured</a:To>"
    "<o:Security s:mustUnderstand=\"1\" xmlns:o=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">"
      "<Timestamp xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\"><Created>%CREATED%</Created><Expires>%EXPIRES%</Expires></Timestamp>"
      "<wuws:WindowsUpdateTicketsToken wsu:id=\"ClientMSA\" xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\" xmlns:wuws=\"http://schemas.microsoft.com/msus/2014/10/WindowsUpdateAuthorization\"><TicketType Name=\"MSA\" Version=\"1.0\" Policy=\"MBI_SSL\"><user></user></TicketType></wuws:WindowsUpdateTicketsToken>"
    "</o:Security>"
  "</s:Header>"
  "<s:Body>"
    "<GetExtendedUpdateInfo2 xmlns=\"" FE3_WU_NS "\">"
      "<updateIDs><UpdateIdentity><UpdateID>%UPDATEID%</UpdateID><RevisionNumber>%REVISION%</RevisionNumber></UpdateIdentity></updateIDs>"
      "<infoTypes><XmlUpdateFragmentType>FileUrl</XmlUpdateFragmentType><XmlUpdateFragmentType>FileDecryption</XmlUpdateFragmentType></infoTypes>"
      "<deviceAttributes>" FE3_DEVICE_ATTRS "</deviceAttributes>"
    "</GetExtendedUpdateInfo2>"
  "</s:Body>"
"</s:Envelope>";

#endif /* FE3_TEMPLATES_H */
