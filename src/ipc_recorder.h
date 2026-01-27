//
// Created by wention on 2026/1/27.
//

#ifndef NVR_IPC_RECORDER_H
#define NVR_IPC_RECORDER_H

#include <string>

class IPCRecorder {
public:
    explicit IPCRecorder(std::string& stream_url);
    ~IPCRecorder();

    void start();
    void stop();

private:
    std::string m_stream_url;
};


#endif //NVR_IPC_RECORDER_H
