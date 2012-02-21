#include "stdafx.h"
#include "SharedMemoryClient.h"
#include "SafeEnv.h"
#include "utils.h"

#include <assert.h>

using namespace std;

SharedMemoryClient::SharedMemoryClient(SharedMemoryClient_parameter_storage_t& o, IScriptEnvironment* env) :
    SharedMemoryClient_parameter_storage_t(o),
    _manager(get_shared_memory_key("LOCAL", _port)),
    _vi(_manager.header->clips[_clip_index].vi)
{
    if (_manager.header->object_state.shutdown)
    {
        env->ThrowError("The server has been shut down");
    }
    // we don't support audio
    _vi.audio_samples_per_second = 0;
}

PVideoFrame SharedMemoryClient::create_frame(int response_index, IScriptEnvironment* env)
{
    auto& clip = _manager.header->clips[_clip_index];
    const unsigned char* buffer = (const unsigned char*)_manager.header + clip.frame_buffer_offset[response_index];
    PVideoFrame frame = SafeNewVideoFrame(env, _vi);
    copy_plane(frame, buffer, clip.frame_pitch, _vi, PLANAR_Y);
    if (_vi.IsPlanar() && !_vi.IsY8())
    {
        copy_plane(frame, buffer + clip.frame_offset_u, clip.frame_pitch_uv, _vi, PLANAR_U);
        copy_plane(frame, buffer + clip.frame_offset_v, clip.frame_pitch_uv, _vi, PLANAR_V);
    }
    return frame;
}

PVideoFrame SharedMemoryClient::GetFrame(int n, IScriptEnvironment* env)
{
    SharedMemorySourceClientLockAcquire lock(_manager);
    if (_manager.header->object_state.shutdown)
    {
        _manager.request_cond->signal.signal_all();
        env->ThrowError("SharedMemoryClient: The server has been shut down.");
    }
    volatile auto& request = _manager.header->request;
    int response_index = get_response_index(n);
    volatile auto& resp = _manager.header->clips[_clip_index].frame_response[response_index];
    CondVar& cond = *_manager.sync_groups[_clip_index]->response_conds[response_index];
    if (cond.lock.try_lock(SPIN_LOCK_UNIT * 5))
    {
        SpinLockContext<> ctx(cond.lock);
        if (_manager.header->object_state.shutdown)
        {
            _manager.request_cond->signal.signal_all();
            env->ThrowError("SharedMemoryClient: The server has been shut down.");
        }
        if (resp.frame_number == n)
        {
            // prefetch hit
            // don't change client count here since we didn't requested it
            return create_frame(response_index, env);
        }
    }
    while (true)
    {
        _manager.request_cond->lock_short();
        {
            SpinLockContext<> ctx(_manager.request_cond->lock);
            if (_manager.header->object_state.shutdown)
            {
                _manager.request_cond->signal.signal_all();
                env->ThrowError("SharedMemoryClient: The server has been shut down.");
            }
            if (request.request_type == REQ_EMPTY)
            {
                request.request_type = REQ_GETFRAME;
                request.clip_index = _clip_index;
                request.frame_number = n;
                _manager.request_cond->signal.switch_to_other_side();
                break;
            }
        }
        _manager.request_cond->signal.wait_on_this_side(INFINITE);
    }
    while (true)
    {
        cond.lock_long();
        {
            SpinLockContext<> ctx(cond.lock);
            if (_manager.header->object_state.shutdown)
            {
                _manager.request_cond->signal.signal_all();
                env->ThrowError("SharedMemoryClient: The server has been shut down.");
            }
            if (resp.frame_number != request.frame_number)
            {
                if (resp.requested_client_count == 0)
                {
                    // no one needs this frame now, let server fetch new frame
                    cond.signal.switch_to_other_side();
                }
            } else {
                PVideoFrame frame = create_frame(response_index, env);
                _InterlockedDecrement(&resp.requested_client_count);
                cond.signal.switch_to_other_side();
                return frame;
            }
        }
    }
}

bool SharedMemoryClient::GetParity(int n)
{
    SharedMemorySourceClientLockAcquire lock(_manager);
    volatile auto& request = _manager.header->request;
    volatile long& result_reference = _manager.header->clips[_clip_index].parity_response[get_response_index(n)];
    while (true)
    {
        _manager.request_cond->lock_short();
        {
            SpinLockContext<> ctx(_manager.request_cond->lock);
            if (_manager.header->object_state.shutdown)
            {
                _manager.request_cond->signal.signal_all();
                return false;
            }
            if (request.request_type == REQ_EMPTY)
            {
                request.request_type = REQ_GETPARITY;
                request.clip_index = _clip_index;
                request.frame_number = n;
                while (_InterlockedCompareExchange(&result_reference, PARITY_WAITING_FOR_RESPONSE, PARITY_RESPONSE_EMPTY) != PARITY_RESPONSE_EMPTY)
                {
                    Sleep(0);
                }
                _manager.request_cond->signal.switch_to_other_side();
                break;
            }
        }
        _manager.request_cond->signal.wait_on_this_side(INFINITE);
    }
    while (true)
    {
        long result = result_reference;
        if (result == PARITY_WAITING_FOR_RESPONSE)
        {
            Sleep(0);
            if (_manager.header->object_state.shutdown)
            {
                return false;
            }
            continue;
        }
        assert((result & 0x7fffffff) == n);
        _InterlockedCompareExchange(&result_reference, PARITY_RESPONSE_EMPTY, result);
        return !!(result & 0x80000000);
    }
}

const VideoInfo& SharedMemoryClient::GetVideoInfo()
{
    return _vi;
}

void SharedMemoryClient::SetCacheHints(int cachehints,int frame_range)
{
    // we don't handle caching here
}

void SharedMemoryClient::GetAudio(void* buf, __int64 start, __int64 count, IScriptEnvironment* env)
{
    // this shouldn't be called since we disabled audio
    assert(false);
}