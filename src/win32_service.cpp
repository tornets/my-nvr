#include "win32_service.h"
#include <spdlog/spdlog.h>
#include <iostream>
#include <sstream>
#include <vector>
#include <thread>

// Static member initialization
Win32Service* Win32Service::s_instance = nullptr;

Win32Service::Win32Service(const std::string& serviceName, const std::string& serviceDisplayName)
    : m_serviceName(serviceName)
    , m_displayName(serviceDisplayName)
    , m_statusHandle(nullptr)
    , m_stopEvent(nullptr)
    , m_serviceMainFunc(nullptr)
    , m_stopCallback(nullptr)
{
    // Initialize SERVICE_STATUS structure
    ZeroMemory(&m_status, sizeof(m_status));
}

Win32Service::~Win32Service() {
    if (m_stopEvent) {
        CloseHandle(m_stopEvent);
    }
}

bool Win32Service::install(const std::string& binaryPath, const std::vector<std::string>& extraArgs) {
    // Get full path to executable
    char szPath[MAX_PATH];
    if (!GetModuleFileNameA(NULL, szPath, MAX_PATH)) {
        m_lastError = "GetModuleFileName failed";
        return false;
    }

    // Build command line: "binary_path" svr -- extra_args
    // Quote each argument if it contains spaces
    std::string cmdLine = "\"" + std::string(szPath) + "\" svr";
    for (const auto& arg : extraArgs) {
        cmdLine += " ";
        // Check if argument contains spaces or is empty
        if (arg.empty() || arg.find(' ') != std::string::npos) {
            cmdLine += "\"" + arg + "\"";
        } else {
            cmdLine += arg;
        }
    }

    // Open Service Control Manager
    SC_HANDLE scManager = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scManager) {
        m_lastError = "OpenSCManager failed (error code: " + std::to_string(GetLastError()) + ")";
        return false;
    }

    // Check if service already exists
    SC_HANDLE service = OpenServiceA(scManager, m_serviceName.c_str(), SERVICE_ALL_ACCESS);
    if (service) {
        CloseServiceHandle(service);
        CloseServiceHandle(scManager);
        m_lastError = "Service already exists";
        return false;
    }

    // Create service
    service = CreateServiceA(
        scManager,
        m_serviceName.c_str(),
        m_displayName.c_str(),
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        cmdLine.c_str(),
        NULL, NULL, NULL, NULL, NULL
    );

    if (!service) {
        DWORD error = GetLastError();
        CloseServiceHandle(scManager);
        m_lastError = "CreateService failed (error code: " + std::to_string(error) + ")";
        return false;
    }

    // Set service description
    SERVICE_DESCRIPTIONA desc = {(char*)"NVR - Network Video Recorder Service"};
    ChangeServiceConfig2A(service, SERVICE_CONFIG_DESCRIPTION, &desc);

    CloseServiceHandle(service);
    CloseServiceHandle(scManager);

    spdlog::info("Service '{}' installed successfully", m_serviceName);
    return true;
}

bool Win32Service::uninstall() {
    SC_HANDLE scManager = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scManager) {
        m_lastError = "OpenSCManager failed";
        return false;
    }

    SC_HANDLE service = OpenServiceA(scManager, m_serviceName.c_str(), SERVICE_ALL_ACCESS | DELETE);
    if (!service) {
        DWORD error = GetLastError();
        CloseServiceHandle(scManager);
        m_lastError = "OpenService failed (error code: " + std::to_string(error) + ")";
        return false;
    }

    // Stop service if running
    SERVICE_STATUS status;
    if (QueryServiceStatus(service, &status)) {
        if (status.dwCurrentState == SERVICE_RUNNING) {
            spdlog::info("Stopping service before uninstall...");
            ControlService(service, SERVICE_CONTROL_STOP, &status);
            Sleep(1000);
        }
    }

    // Delete service
    if (!DeleteService(service)) {
        DWORD error = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(scManager);
        m_lastError = "DeleteService failed (error code: " + std::to_string(error) + ")";
        return false;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scManager);

    spdlog::info("Service '{}' uninstalled successfully", m_serviceName);
    return true;
}

bool Win32Service::start() {
    SC_HANDLE scManager = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scManager) {
        m_lastError = "OpenSCManager failed";
        return false;
    }

    SC_HANDLE service = OpenServiceA(scManager, m_serviceName.c_str(), SERVICE_START);
    if (!service) {
        DWORD error = GetLastError();
        CloseServiceHandle(scManager);
        m_lastError = "OpenService failed (error code: " + std::to_string(error) + ")";
        return false;
    }

    if (!StartServiceA(service, 0, NULL)) {
        DWORD error = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(scManager);
        if (error == ERROR_SERVICE_ALREADY_RUNNING) {
            m_lastError = "Service is already running";
        } else {
            m_lastError = "StartService failed (error code: " + std::to_string(error) + ")";
        }
        return false;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scManager);

    spdlog::info("Service '{}' started successfully", m_serviceName);
    return true;
}

bool Win32Service::stop() {
    SC_HANDLE scManager = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scManager) {
        m_lastError = "OpenSCManager failed";
        return false;
    }

    SC_HANDLE service = OpenServiceA(scManager, m_serviceName.c_str(), SERVICE_STOP);
    if (!service) {
        DWORD error = GetLastError();
        CloseServiceHandle(scManager);
        m_lastError = "OpenService failed (error code: " + std::to_string(error) + ")";
        return false;
    }

    SERVICE_STATUS status;
    if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        DWORD error = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(scManager);
        if (error == ERROR_SERVICE_NOT_ACTIVE) {
            m_lastError = "Service is not running";
        } else {
            m_lastError = "ControlService failed (error code: " + std::to_string(error) + ")";
        }
        return false;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scManager);

    spdlog::info("Service '{}' stopped successfully", m_serviceName);
    return true;
}

bool Win32Service::restart() {
    spdlog::info("Restarting service '{}'...", m_serviceName);
    if (!stop()) {
        // If not running, try to start
        std::string err = m_lastError;
        if (err.find("not running") != std::string::npos ||
            err.find("ERROR_SERVICE_NOT_ACTIVE") != std::string::npos) {
            spdlog::info("Service was not running, starting...");
            return start();
        }
        return false;
    }

    // Wait for service to stop
    Sleep(2000);
    return start();
}

void Win32Service::runAsService(ServiceMainFunction serviceMain) {
    m_serviceMainFunc = serviceMain;
    s_instance = this;

    // Convert service name to wide string
    std::wstring wServiceName(m_serviceName.begin(), m_serviceName.end());

    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { (LPWSTR)wServiceName.c_str(), (LPSERVICE_MAIN_FUNCTIONW)Win32Service::serviceMain },
        { NULL, NULL }
    };

    // This call blocks until service stops
    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        DWORD error = GetLastError();
        m_lastError = "StartServiceCtrlDispatcher failed (error code: " + std::to_string(error) + ")";
        spdlog::error("{}", m_lastError);
    }
}

bool Win32Service::isRunningAsService() {
    // Check if running in service context
    // This is a simple heuristic - can be improved
    return (GetConsoleWindow() == NULL);
}

std::string Win32Service::getLastError() const {
    return m_lastError;
}

void Win32Service::setServiceStopCallback(StopCallback callback) {
    m_stopCallback = callback;
}

void WINAPI Win32Service::serviceMain(DWORD argc, LPWSTR* argv) {
    if (s_instance) {
        s_instance->serviceMainImpl(argc, argv);
    }
}

void Win32Service::serviceMainImpl(DWORD argc, LPWSTR* argv) {
    // Register control handler
    std::wstring wServiceName(m_serviceName.begin(), m_serviceName.end());
    m_statusHandle = RegisterServiceCtrlHandlerExW(
        wServiceName.c_str(),
        serviceCtrlHandlerEx,
        this
    );

    if (!m_statusHandle) {
        m_lastError = "RegisterServiceCtrlHandler failed";
        return;
    }

    // Report start pending
    reportStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    // Create stop event
    m_stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!m_stopEvent) {
        reportStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    // Report running
    reportStatus(SERVICE_RUNNING, NO_ERROR, 0);
    spdlog::info("Service '{}' started", m_serviceName);

    // Start the application in a separate thread
    std::thread* appThread = nullptr;
    if (m_serviceMainFunc) {
        try {
            appThread = new std::thread([this]() {
                __try {
                    m_serviceMainFunc();
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    spdlog::error("Exception in service thread");
                }
            });
        } catch (const std::exception& e) {
            spdlog::error("Service start error: {}", e.what());
            m_lastError = e.what();
        }
    }

    // Wait for stop signal
    WaitForSingleObject(m_stopEvent, INFINITE);
    spdlog::info("Stop signal received, waiting for application thread...");

    // Give the application thread a moment to finish gracefully
    if (appThread && appThread->joinable()) {
        // Wait with timeout by checking in a loop
        for (int i = 0; i < 30; i++) {  // 3 seconds total
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Try a timed join using native handle
            HANDLE hThread = appThread->native_handle();
            DWORD waitResult = WaitForSingleObject(hThread, 0);
            if (waitResult == WAIT_OBJECT_0) {
                // Thread finished
                appThread->join();
                break;
            }
        }

        // Force cleanup if thread hasn't finished
        if (appThread->joinable()) {
            spdlog::warn("Application thread still running, detaching");
            appThread->detach();
        }

        delete appThread;
    }

    CloseHandle(m_stopEvent);
    m_stopEvent = nullptr;

    spdlog::info("Service '{}' stopped", m_serviceName);

    // Report stopped
    reportStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

DWORD WINAPI Win32Service::serviceCtrlHandlerEx(DWORD ctrlCode, DWORD eventType,
                                                 void* eventData, void* context) {
    Win32Service* service = static_cast<Win32Service*>(context);
    if (service) {
        service->handleControlCode(ctrlCode);
    }
    return 0;
}

void Win32Service::handleControlCode(DWORD ctrlCode) {
    switch (ctrlCode) {
        case SERVICE_CONTROL_STOP:
            spdlog::info("Received SERVICE_CONTROL_STOP");
            reportStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);

            // Signal application to stop
            if (m_stopCallback) {
                m_stopCallback();
            }

            if (m_stopEvent) {
                SetEvent(m_stopEvent);
            }
            break;

        case SERVICE_CONTROL_PAUSE:
        case SERVICE_CONTROL_CONTINUE:
        case SERVICE_CONTROL_INTERROGATE:
            reportStatus(m_status.dwCurrentState, NO_ERROR, 0);
            break;

        case SERVICE_CONTROL_SHUTDOWN:
            spdlog::info("Received SERVICE_CONTROL_SHUTDOWN");
            reportStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
            if (m_stopCallback) {
                m_stopCallback();
            }
            if (m_stopEvent) {
                SetEvent(m_stopEvent);
            }
            break;

        default:
            break;
    }
}

void Win32Service::reportStatus(DWORD currentState, DWORD win32ExitCode, DWORD waitHint) {
    static DWORD checkPoint = 1;

    m_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    m_status.dwCurrentState = currentState;
    m_status.dwWin32ExitCode = win32ExitCode;
    m_status.dwWaitHint = waitHint;

    if (currentState == SERVICE_START_PENDING) {
        m_status.dwControlsAccepted = 0;
    } else {
        m_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    }

    if ((currentState == SERVICE_RUNNING) || (currentState == SERVICE_STOPPED)) {
        m_status.dwCheckPoint = 0;
    } else {
        m_status.dwCheckPoint = checkPoint++;
    }

    SetServiceStatus(m_statusHandle, &m_status);
}
