[Run]
Filename: "{app}\bin\nvr.exe"; Parameters: "service install -- --config ""{app}\config.yaml"" "; StatusMsg: "安装服务..."

[UninstallRun]
Filename: "{app}\bin\nvr.exe"; Parameters: "service stop"; RunOnceId: "StopService"; StatusMsg: "停止服务..."
Filename: "{app}\bin\nvr.exe"; Parameters: "service uninstall"; RunOnceId: "UninstallService"; StatusMsg: "删除服务..."

;[Registry]
;Root: HKLM; Subkey: "Software\{#AppPublisher}\{#AppName}"; ValueType: none; Flags: uninsdeletekey
;Root: HKLM; Subkey: "Software\{#AppPublisher}\{#AppName}\Settings"; ValueType: none; Flags: uninsdeletekey
;Root: HKLM; Subkey: "Software\{#AppPublisher}\{#AppName}\Settings"; ValueType: string; ValueName: "InstallPath"; ValueData: "{app}"
;Root: HKLM; Subkey: "Software\{#AppPublisher}\{#AppName}\Settings"; ValueType: string; ValueName: "Version"; ValueData: "{#AppVersion}"