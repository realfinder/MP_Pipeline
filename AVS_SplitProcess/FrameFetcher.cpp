#include "stdafx.h"
#include "FrameFetcher.h"

#include <process.h>
#include <assert.h>

using namespace std;

void wait_and_reset_event(OwnedHandle& event)
{
    event.wait();
    ResetEvent(event.get());
}

unsigned __stdcall FrameFetcher::thread_stub(void* fetcher)
{
    return ((FrameFetcher*)fetcher)->thread_proc();
}

unsigned FrameFetcher::thread_proc()
{
    int fetch_info_version;

    { // lock start
        CSLockAcquire lock(_lock);
        fetch_info_version = _fetch_info.version;
    } // lock end

    try
    {
        while (!_shutdown)
        {
            ClipInfo* clip_to_fetch = NULL;
            int requested_frame = -1;
            bool is_requested_fetch = false;
            function<void (void)> callback(nullptr);

            { // lock start
                CSLockAcquire lock(_lock);
                if (_worker_callback) {
                    callback = _worker_callback;
                    _worker_callback = nullptr;
                } else if (_fetch_info.version != fetch_info_version) {
                    clip_to_fetch = &_clips[_fetch_info.clip_index];
                    assert(clip_to_fetch->error_msg.empty());

                    requested_frame = _fetch_info.frame_number;
                    fetch_info_version = _fetch_info.version;
                    is_requested_fetch = true;
                } else {
                    int max_cache_space = 0;
                    for (size_t i = 0; i < (int)_clips.size(); i++)
                    {
                        ClipInfo& clip = _clips[i];
                        int cache_space = _max_cache_frames - (int)clip.frame_cache.size();
                        cache_space -= _cache_behind - (clip.last_requested_frame - clip.cache_frame_start);
                        if (cache_space <= 0) 
                        {
                            continue;
                        }
                        if (!clip.error_msg.empty())
                        {
                            continue;
                        }
                        if (cache_space > max_cache_space)
                        {
                            clip_to_fetch = &clip;
                            requested_frame = clip.cache_frame_start + (int)clip.frame_cache.size();
                            max_cache_space = cache_space;
                            assert(requested_frame >= 0);
                        }
                    }
                }
            } // lock end

            if (callback)
            {
                callback();
                work_item_completed(10);
                continue;
            }

            if (!clip_to_fetch)
            {
                work_item_completed(500);
                continue;
            }

            assert(requested_frame >= 0);

            fetch_frame(*clip_to_fetch, requested_frame);

            if (is_requested_fetch)
            {
                CSLockAcquire lock(_lock);
                assert(_fetch_info.is_fetching);
                assert(_fetch_info.version == fetch_info_version);
                _fetch_info.is_fetching = false;
            }

            work_item_completed(10);
        }
    } catch (...) {
        __debugbreak();
        throw;
    }
    return 0;
}

void FrameFetcher::work_item_completed(DWORD wait_time)
{
    SetEvent(_worker_workitem_completed_event.get());
    _worker_waiting_for_work_event.wait(wait_time);
}

void FrameFetcher::fetch_frame(ClipInfo& clip, int n)
{
    int fetch_start = -1;

    { // lock start
        CSLockAcquire lock(_lock);
        if (n >= clip.cache_frame_start)
        {
            while (clip.frame_cache.size() > 0 && n - clip.cache_frame_start > _cache_behind)
            {
                clip.frame_cache.pop_front();
                clip.cache_frame_start++;
            }
        } else {
            clip.frame_cache.clear();
        }
        if (clip.frame_cache.size() == 0) 
        {
            clip.cache_frame_start = n;
            fetch_start = n;
        } else {
            fetch_start = clip.cache_frame_start + (int)clip.frame_cache.size();
        }
    } // lock end
    
    // sanity check
    assert(fetch_start >= 0);
    assert(n - fetch_start < _cache_behind);

    while (fetch_start <= n)
    {
        PVideoFrame frame = try_get_frame(clip, fetch_start);
        if (!frame)
        {
            return;
        }
        { // lock start
            CSLockAcquire lock(_lock);
            clip.frame_cache.push_back(frame);
        } // lock end
        fetch_start++;
    }
}

PVideoFrame FrameFetcher::try_get_frame(ClipInfo& clip, int n)
{
    assert(clip.error_msg.empty());
    PVideoFrame frame = NULL;
    try
    {
        frame = clip.clip->GetFrame(n, _env);
    } catch (AvisynthError& ex) {
        CSLockAcquire lock(_lock);
        clip.error_msg.assign(ex.msg);
        clip.frame_cache.clear();
        frame = NULL;
    }
    return frame;
}

void FrameFetcher::invoke_in_worker_thread(function<void (void)> func)
{
    bool completed = false;
    bool stub_set = false;
    auto function_stub = [&func, &completed] {
        func();
        completed = true;
    };
    while (!completed)
    {
        { // lock start
            if (!_worker_callback)
            {
                assert(!stub_set);
                _worker_callback = function_stub;
                stub_set = true;
                SetEvent(_worker_waiting_for_work_event.get());
            }
        } // lock end
        wait_for_work_item_complete();
    }
}

void FrameFetcher::wait_for_work_item_complete()
{
    wait_and_reset_event(_worker_workitem_completed_event);
}

bool FrameFetcher::GetParity(int clip_index, int n)
{
    ClipInfo& clip = _clips[clip_index];
    bool result;
    invoke_in_worker_thread([&clip, &result, n] {
        result = clip.clip->GetParity(n);
    });
    return result;
}

void FrameFetcher::GetAudio(int clip_index, void* buf, __int64 start, __int64 count, IScriptEnvironment* env)
{
    assert(env == _env);
    ClipInfo& clip = _clips[clip_index];
    string error_msg;
    invoke_in_worker_thread([&clip, buf, start, count, env, &error_msg] {
        try
        {
            clip.clip->GetAudio(buf, start, count, env);
        } catch (AvisynthError& e) {
            error_msg.assign(e.msg);
        }
    });
    if (!error_msg.empty())
    {
        env->ThrowError(error_msg.c_str());
    }
}

const VideoInfo& FrameFetcher::GetVideoInfo(int clip_index)
{
    ClipInfo& clip = _clips[clip_index];
    if (!clip.has_video_info)
    {
        invoke_in_worker_thread([&clip, this] {
            { // lock start
                CSLockAcquire lock(_lock);
                if (clip.has_video_info)
                {
                    return;
                }
                clip.vi = clip.clip->GetVideoInfo();
                MemoryBarrier();
                clip.has_video_info = true;
            } // lock end
        });
    }

    return clip.vi;
}

PVideoFrame FrameFetcher::GetFrame(int clip_index, int n, IScriptEnvironment* env)
{
    assert(env == _env);
    while (true)
    {
        { // lock start
            CSLockAcquire lock(_lock);
            if (_shutdown)
            {
                env->ThrowError("FrameFetcher: Already shut down!");
            }
            ClipInfo& clip = _clips[clip_index];
            if (!clip.error_msg.empty())
            {
                env->ThrowError(clip.error_msg.c_str());
            }
            if (n >= clip.cache_frame_start && n < clip.cache_frame_start + (int)clip.frame_cache.size())
            {
                clip.last_requested_frame = n;
                return clip.frame_cache.at(n - clip.cache_frame_start);
            }
            if (!_fetch_info.is_fetching)
            {
                _fetch_info.is_fetching = true;
                _fetch_info.clip_index = clip_index;
                _fetch_info.frame_number = n;
                _fetch_info.version++;
                SetEvent(_worker_waiting_for_work_event.get());
            }
        } // lock end
        wait_for_work_item_complete();
    }
}

FrameFetcher::FrameFetcher(PClip clips[], int max_cache_frames, int cache_behind, IScriptEnvironment* env)
    : _max_cache_frames(max_cache_frames <= cache_behind ? cache_behind + 1 : max_cache_frames),  
      _cache_behind(cache_behind),
      _env(env),
      _clips(),
      _lock(),
      _worker_callback(),
      _worker_thread(NULL),
      _worker_workitem_completed_event(CreateEvent(NULL, TRUE, FALSE, NULL)), // needs to be manual reset, since we want all threads waiting on this event to be released when it is signaled
      _worker_waiting_for_work_event(CreateEvent(NULL, FALSE, FALSE, NULL)),
      _shutdown(false)
{
    memset((void*)&_fetch_info, 0, sizeof(_fetch_info));

    if (!_worker_workitem_completed_event.is_valid() || !_worker_waiting_for_work_event.is_valid())
    {
        env->ThrowError("FrameFetcher: Unable to create event.");
    }

    int i = 0;
    while (clips[i])
    {
        _clips.push_back(ClipInfo(clips[i]));
        i++;
    }
    if (_clips.size() == 0)
    {
        env->ThrowError("FrameFetcher: Unexpected exception: No input clip.");
    }
    _worker_thread.replace((HANDLE)_beginthreadex(NULL, 0, FrameFetcher::thread_stub, this, 0, NULL));
    if (!_worker_thread.is_valid())
    {
        env->ThrowError("FrameFetcher: Can't start worker thread.");
    }
}

FrameFetcher::~FrameFetcher()
{
    _shutdown = true;
    SetEvent(_worker_waiting_for_work_event.get());
    _worker_thread.wait();
    _env = NULL;
}