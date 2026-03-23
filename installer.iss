; NVR Service Installer Script (内置中文支持)
; Requires Inno Setup 6.x

#define MyAppName "NVR Service"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Your Company"
#define MyAppURL "https://github.com/yourusername/nvr"
#define MyAppExeName "nvr.exe"
#define MyAppServiceName "NVRService"
#define MyAppDisplayName "NVR Video Recorder Service"

[Setup]
AppId={{A1B2C3D4-E5F6-4A5B-8C7D-9E0F1A2B3C4D5}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=installer_output
OutputBaseFilename=nvr-service-setup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=admin
MinVersion=6.1sp1
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
ShowLanguageDialog=yes
Uninstallable=yes
CreateAppDir=yes
WizardStyle=modern

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "Create desktop icon"; GroupDescription: "Additional icons:"; Flags: unchecked
Name: "quicklaunchicon"; Description: "Create quick launch icon"; GroupDescription: "Additional icons:"; Flags: unchecked; OnlyBelowVersion: 6.1

[Files]
Source: "build\src\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "config.yaml.example"; DestDir: "{app}"; Flags: ignoreversion; Attribs: readonly
Source: "INSTALL.md"; DestDir: "{app}"; Flags: ignoreversion; Attribs: readonly
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion; Attribs: readonly

[Dirs]
Name: "{app}\recordings"
Name: "{app}\recordings\.temp"

[Run]
Filename: "{app}\{#MyAppExeName}"; Parameters: "service install -- --config ""{app}\config.yaml"" "; StatusMsg: "Installing Windows Service..."

[UninstallRun]
Filename: "{app}\{#MyAppExeName}"; Parameters: "service stop"; RunOnceId: "StopService"; StatusMsg: "Stopping service..."
Filename: "{app}\{#MyAppExeName}"; Parameters: "service uninstall"; RunOnceId: "UninstallService"; StatusMsg: "Uninstalling service..."

[Registry]
Root: HKLM; Subkey: "Software\{#MyAppPublisher}\{#MyAppName}"; ValueType: none; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\{#MyAppPublisher}\{#MyAppName}\Settings"; ValueType: none; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\{#MyAppPublisher}\{#MyAppName}\Settings"; ValueType: string; ValueName: "InstallPath"; ValueData: "{app}"
Root: HKLM; Subkey: "Software\{#MyAppPublisher}\{#MyAppName}\Settings"; ValueType: string; ValueName: "Version"; ValueData: "{#MyAppVersion}"

[Code]
function InitializeSetup(): Boolean;
var
  ResultCode: Integer;
begin
  if RegKeyExists(HKLM, 'Software\{#MyAppPublisher}\{#MyAppName}') then begin
    if MsgBox('An older version is installed. Uninstall it?', mbInformation, MB_YESNO) = IDYES then begin
      if Exec(ExpandConstant('{uninstallexe}'), '/SILENT /NORESTART /SUPPRESSMSGBOXES', '', SW_SHOW, ewWaitUntilTerminated, ResultCode) then begin
        ResultCode := 1;
      end;
    end;
  end;

  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ConfigFile, RecordingsDir: String;
begin
  if CurUninstallStep = usUninstall then begin
    ConfigFile := ExpandConstant('{app}\config.yaml');
    RecordingsDir := ExpandConstant('{app}\recordings');

    if FileExists(ConfigFile) then
      if MsgBox('删除配置文件?', mbConfirmation, MB_YESNO) = IDYES then
        DeleteFile(ConfigFile);

    if DirExists(RecordingsDir) then
      if MsgBox('删除录像文件?', mbConfirmation, IDNO) = IDYES then
        DelTree(RecordingsDir, True, True, True);
  end;
end;
