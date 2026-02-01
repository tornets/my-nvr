#ifndef NVR_WIN32_SERVICE_H
#define NVR_WIN32_SERVICE_H

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <windows.h>
#include <winsvc.h>

class Win32Service {
public:
    // Service control callback type
    using ServiceMainFunction = std::function<void()>;
    using StopCallback = std::function<void()>;

    Win32Service(const std::string& serviceName, const std::string& serviceDisplayName);
    ~Win32Service();

    // Service installation and management
    bool install(const std::string& binaryPath, const std::vector<std::string>& extraArgs = {});
    bool uninstall();
    bool start();
    bool stop();
    bool restart();

    // Service mode entry point (called by SCM)
    void runAsService(ServiceMainFunction serviceMain);

    // Set stop callback
    void setServiceStopCallback(StopCallback callback);

    // Check if running as service
    static bool isRunningAsService();

    // Get error message
    std::string getLastError() const;

private:
    // Service callback functions
    static void WINAPI serviceMain(DWORD argc, LPWSTR* argv);
    static DWORD WINAPI serviceCtrlHandlerEx(DWORD ctrlCode, DWORD eventType,
                                             void* eventData, void* context);

    // Service status reporting
    void reportStatus(DWORD currentState, DWORD win32ExitCode = NO_ERROR,
                     DWORD waitHint = 0);

    // Internal implementation
    void serviceMainImpl(DWORD argc, LPWSTR* argv);
    void handleControlCode(DWORD ctrlCode);

    // Member variables
    std::string m_serviceName;
    std::string m_displayName;
    SERVICE_STATUS_HANDLE m_statusHandle;
    SERVICE_STATUS m_status;
    HANDLE m_stopEvent;

    static Win32Service* s_instance;  // Singleton for callbacks
    ServiceMainFunction m_serviceMainFunc;
    StopCallback m_stopCallback;
    std::string m_lastError;
};

#endif // NVR_WIN32_SERVICE_H
