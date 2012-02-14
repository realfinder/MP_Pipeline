#pragma once

#include "SharedMemoryServer.def.h"
#include "FrameFetcher.h"
#include "SharedMemoryCommon.h"
#include "Handle.h"

class SharedMemoryServer : private SharedMemoryServer_parameter_storage_t, public GenericVideoFilter
{
public:
    SharedMemoryServer(PClip clips[], int clip_count, const VideoInfo vi_array[], SharedMemoryServer_parameter_storage_t& o, IScriptEnvironment* env);
    ~SharedMemoryServer();

    PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env);
    bool __stdcall GetParity(int n);


private:
    unsigned thread_proc();
    static unsigned __stdcall thread_stub(void* self);
    
    void initiate_shutdown();
    bool is_shutting_down() const;

    IScriptEnvironment* _env;
    FrameFetcher _fetcher;
    SharedMemorySourceManager _manager;
    OwnedHandle _thread_handle;
    bool _shutdown;
};