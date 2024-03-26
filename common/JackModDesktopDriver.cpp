/*
*/

#include "JackAudioDriver.h"
#include "JackDriverLoader.h"
#include "JackEngineControl.h"
#include "JackLockedEngine.h"
#include "JackMidiPort.h"
#include "JackTools.h"

#ifndef _WIN32
# include <cerrno>
# include <fcntl.h>
# include <sys/mman.h>
# ifdef __APPLE__
#  include <dispatch/dispatch.h>
extern "C" {
int __ulock_wait(uint32_t operation, void* addr, uint64_t value, uint32_t timeout_us);
int __ulock_wake(uint32_t operation, void* addr, uint64_t value);
}
# else
#  include <syscall.h>
#  include <sys/prctl.h>
#  include <sys/time.h>
#  include <linux/futex.h>
# endif
#endif

namespace Jack
{

// -----------------------------------------------------------------------------------------------------------

#ifdef __APPLE__
static void terminateHandler(void*)
{
    printf("MOD Desktop driver parent has died, terminating ourselves now\n");
    fflush(stdout);
    kill(getpid(), SIGTERM);
}
#endif

static void terminateOnParentExit() noexcept
{
#if defined(__APPLE__)
    const dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC,
                                                            getppid(),
                                                            DISPATCH_PROC_EXIT,
                                                            nullptr);

    dispatch_source_set_event_handler_f(source, terminateHandler);

    dispatch_resume(source);
#elif !defined(_WIN32)
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
}

// -----------------------------------------------------------------------------------------------------------

class ModDesktopAudioDriver : public JackAudioDriver
{
    struct Data {
        uint32_t magic;
        int32_t padding1;
       #ifdef _WIN32
        HANDLE sem1;
        HANDLE sem2;
       #else
        int32_t sem1;
        int32_t sem2;
       #endif
        uint16_t midiEventCount;
        uint16_t midiFrames[511];
        uint8_t midiData[511 * 4];
        uint8_t padding2[4];
        float audio[];
    };

    static constexpr const size_t kDataSize = sizeof(Data) + sizeof(float) * 128 * 2;

    Data* fShmData;
    const unsigned int fShmPort;

   #ifdef _WIN32
    HANDLE fShm;
   #else
    int fShmFd;
   #endif

   #ifdef _WIN32
    void post()
    {
        ReleaseSemaphore(fShmData->sem2, 1, nullptr);
    }

    bool wait()
    {
        return WaitForSingleObject(fShmData->sem1, 1000) == WAIT_OBJECT_0;
    }
   #else
    void post()
    {
        const bool unlocked = __sync_bool_compare_and_swap(&fShmData->sem2, 0, 1);
        if (! unlocked)
            return;
       #ifdef __APPLE__
        __ulock_wake(0x1000003, &fShmData->sem2, 0);
       #else
        syscall(__NR_futex, &fShmData->sem2, FUTEX_WAKE, 1, nullptr, nullptr, 0);
       #endif
    }

    bool wait()
    {
       #ifdef __APPLE__
        const uint32_t timeout = 1000000;
       #else
        const timespec timeout = { 1, 0 };
       #endif

        for (;;)
        {
            if (__sync_bool_compare_and_swap(&fShmData->sem1, 1, 0))
                return true;

           #ifdef __APPLE__
            if (__ulock_wait(0x3, &fShmData->sem1, 0, timeout) != 0)
           #else
            if (syscall(__NR_futex, &fShmData->sem1, FUTEX_WAIT, 0, &timeout, nullptr, 0) != 0)
           #endif
                if (errno != EAGAIN && errno != EINTR)
                    return false;
        }
    }
   #endif

    jack_native_thread_t fProcessThread;
    int fCaptureMidiPort;
    int fPlaybackMidiPort;
    bool fIsProcessing, fIsRunning;

    static void* on_process(void* const arg)
    {
        static_cast<ModDesktopAudioDriver*>(arg)->_process();
        return nullptr;
    }

    void _process()
    {
        if (set_threaded_log_function())
        {
        }

        while (fIsProcessing && fIsRunning)
        {
            if (! wait())
                continue;

            if (fShmData->magic == 7331)
            {
                fIsProcessing = false;
                post();
                return;
            }

            CycleTakeBeginTime();

            if (Process() != 0)
                fIsProcessing = false;

            post();
        }
    }

public:
    ModDesktopAudioDriver(const char* name, const char* alias, JackLockedEngine* engine, JackSynchro* table, unsigned int shmport)
        : JackAudioDriver(name, alias, engine, table),
          fShmData(nullptr),
          fShmPort(shmport),
         #ifdef _WIN32
          fShm(nullptr),
         #else
          fShmFd(-1),
         #endif
          fCaptureMidiPort(0),
          fPlaybackMidiPort(0),
          fIsProcessing(false),
          fIsRunning(false)
    {
        printf("%03d:%s\n", __LINE__, __FUNCTION__);
        fflush(stdout);
    }

    ~ModDesktopAudioDriver() override
    {
        printf("%03d:%s\n", __LINE__, __FUNCTION__);
        fflush(stdout);
    }

    int Open(jack_nframes_t buffersize,
             jack_nframes_t samplerate,
             bool capturing,
             bool playing,
             int chan_in,
             int chan_out,
             bool monitor,
             const char* capture_driver_name,
             const char* playback_driver_name,
             jack_nframes_t capture_latency,
             jack_nframes_t playback_latency) override
    {
        printf("%03d:%s\n", __LINE__, __FUNCTION__);
        fflush(stdout);
        if (JackAudioDriver::Open(buffersize, samplerate, capturing, playing, chan_in, chan_out, monitor,
            capture_driver_name, playback_driver_name, capture_latency, playback_latency) != 0) {
            return -1;
        }

        void* ptr;
        char shmName[32] = {};

      #ifdef _WIN32
        std::snprintf(shmName, 31, "Local\\mod-desktop-shm-%d", fShmPort);

        fShm = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shmName);
        if (fShm == nullptr)
        {
            Close();
            jack_error("Can't open default MOD Desktop driver 1");
            return -1;
        }

        ptr = MapViewOfFile(fShm, FILE_MAP_ALL_ACCESS, 0, 0, kDataSize);
        if (ptr == nullptr)
        {
            Close();
            jack_error("Can't open default MOD Desktop driver 2");
            return -1;
        }

        VirtualLock(ptr, kDataSize);
      #else
        std::snprintf(shmName, 31, "/mod-desktop-shm-%d", fShmPort);

        fShmFd = shm_open(shmName, O_RDWR, 0);
        if (fShmFd < 0)
        {
            Close();
            jack_error("Can't open default MOD Desktop driver 1");
            return -1;
        }

       #ifdef MAP_LOCKED
        ptr = mmap(nullptr, kDataSize, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_LOCKED, fShmFd, 0);
        if (ptr == nullptr || ptr == MAP_FAILED)
       #endif
        {
            ptr = mmap(nullptr, kDataSize, PROT_READ|PROT_WRITE, MAP_SHARED, fShmFd, 0);
        }

        if (ptr == nullptr || ptr == MAP_FAILED)
        {
            Close();
            jack_error("Can't open default MOD Desktop driver 2");
            return -1;
        }

       #ifndef MAP_LOCKED
        mlock(ptr, kDataSize);
       #endif
      #endif

        fShmData = static_cast<Data*>(ptr);

        if (fShmData->magic != 1337)
        {
            Close();
            jack_error("Can't open default MOD Desktop driver 3");
            return -1;
        }

        return 0;
    }

    int Close() override
    {
        printf("%03d:%s\n", __LINE__, __FUNCTION__);
        fflush(stdout);

        JackAudioDriver::Close();

       #ifdef _WIN32
        if (fShmData != nullptr)
        {
            UnmapViewOfFile(fShmData);
            fShmData = nullptr;
        }

        if (fShm != nullptr)
        {
            CloseHandle(fShm);
            fShm = nullptr;
        }
       #else
        if (fShmData != nullptr)
        {
            munmap(fShmData, kDataSize);
            fShmData = nullptr;
        }

        if (fShmFd >= 0)
        {
            close(fShmFd);
            fShmFd = -1;
        }
       #endif

        return 0;
    }

    int Attach() override
    {
        printf("%03d:%s | %u\n", __LINE__, __FUNCTION__, fShmData->magic);
        fflush(stdout);

        if (JackAudioDriver::Attach() != 0)
            return -1;

        jack_port_id_t port_index;
        JackPort* port;
        if (fEngine->PortRegister(fClientControl.fRefNum, "system:midi_capture_1", JACK_DEFAULT_MIDI_TYPE,
                                  CaptureDriverFlags, fEngineControl->fBufferSize, &port_index) < 0)
        {
            Close();
            jack_error("Can't open default MOD Desktop driver 6");
            return -1;
        }
        fCaptureMidiPort = port_index;
        port = fGraphManager->GetPort(port_index);
        port->SetAlias("MOD Desktop MIDI Capture");

        if (fEngine->PortRegister(fClientControl.fRefNum, "system:midi_playback_1", JACK_DEFAULT_MIDI_TYPE,
                                  PlaybackDriverFlags, fEngineControl->fBufferSize, &port_index) < 0)
        {
        {
            Close();
            jack_error("Can't open default MOD Desktop driver 7");
            return -1;
        }
        }
        fPlaybackMidiPort = port_index;
        port = fGraphManager->GetPort(port_index);
        port->SetAlias("MOD Desktop MIDI Playback");

        return 0;
    }

    int Detach() override
    {
        printf("%03d:%s | %u\n", __LINE__, __FUNCTION__, fShmData->magic);
        fflush(stdout);

        if (JackAudioDriver::Detach() != 0)
            return -1;

        fEngine->PortUnRegister(fClientControl.fRefNum, fCaptureMidiPort);
        fEngine->PortUnRegister(fClientControl.fRefNum, fPlaybackMidiPort);
        return 0;
    }

    int Start() override
    {
        printf("%03d:%s\n", __LINE__, __FUNCTION__);
        fflush(stdout);
        if (JackAudioDriver::Start() != 0)
            return -1;

        fIsProcessing = fIsRunning = true;

        if (JackThread::StartImp(&fProcessThread, 80, 1, on_process, this) == 0)
        {
           #ifdef __APPLE__
            fEngineControl->fPeriod = fEngineControl->fConstraint = fEngineControl->fPeriodUsecs * 1000;
            fEngineControl->fComputation = JackTools::ComputationMicroSec(fEngineControl->fBufferSize) * 1000;

            JackThread::AcquireRealTimeImp(fProcessThread,
                                           fEngineControl->fPeriod,
                                           fEngineControl->fComputation,
                                           fEngineControl->fConstraint);
           #endif
            return 0;
        }

        return JackThread::StartImp(&fProcessThread, 0, 0, on_process, this);
    }

    int Stop() override
    {
        printf("%03d:%s | %u\n", __LINE__, __FUNCTION__, fShmData->magic);
        fflush(stdout);

        fIsProcessing = false;

        if (fIsRunning)
        {
            fIsRunning = false;
            JackThread::StopImp(fProcessThread);
        }

        return JackAudioDriver::Stop();
    }

    int Read() override
    {
        memcpy(GetInputBuffer(0), fShmData->audio, sizeof(float) * fEngineControl->fBufferSize);
        memcpy(GetInputBuffer(1), fShmData->audio + fEngineControl->fBufferSize, sizeof(float) * fEngineControl->fBufferSize);

        JackMidiBuffer* cbuf = (JackMidiBuffer*)fGraphManager->GetBuffer(fCaptureMidiPort, fEngineControl->fBufferSize);
        JackMidiBuffer* pbuf = (JackMidiBuffer*)fGraphManager->GetBuffer(fPlaybackMidiPort, fEngineControl->fBufferSize);
        pbuf->Reset(fEngineControl->fBufferSize);

        for (uint16_t i = 0; i < fShmData->midiEventCount; ++i)
        {
            if (jack_midi_data_t* const data = cbuf->ReserveEvent(fShmData->midiFrames[i], 4))
            {
                memcpy(data, fShmData->midiData + (i * 4), 4);
                continue;
            }
            break;
        }

        return 0;
    }

    int Write() override
    {
        memcpy(fShmData->audio, GetOutputBuffer(0), sizeof(float) * fEngineControl->fBufferSize);
        memcpy(fShmData->audio + fEngineControl->fBufferSize, GetOutputBuffer(1), sizeof(float) * fEngineControl->fBufferSize);

        JackMidiBuffer* cbuf = (JackMidiBuffer*)fGraphManager->GetBuffer(fCaptureMidiPort, fEngineControl->fBufferSize);
        JackMidiBuffer* pbuf = (JackMidiBuffer*)fGraphManager->GetBuffer(fPlaybackMidiPort, fEngineControl->fBufferSize);
        cbuf->Reset(fEngineControl->fBufferSize);

        uint16_t mec = 0;
        for (uint32_t i = 0; i < pbuf->event_count; ++i)
        {
            JackMidiEvent& ev(pbuf->events[i]);

            if (ev.size > 4)
                continue;

            fShmData->midiFrames[mec] = ev.time;
            memcpy(fShmData->midiData + (mec * 4), ev.GetData(pbuf), ev.size);

            for (uint8_t j = ev.size; j < 4; ++j)
                fShmData->midiData[mec * 4 + j] = 0;

            if (++mec == 511)
                break;
        }
        fShmData->midiEventCount = mec;

        return 0;
    }

    bool IsFixedBufferSize() override
    {
        printf("%03d:%s\n", __LINE__, __FUNCTION__);
        return true;
    }
};

} // end of namespace

extern "C" {

#include "JackCompilerDeps.h"

SERVER_EXPORT jack_driver_desc_t* driver_get_descriptor()
{
    printf("%03d:%s\n", __LINE__, __FUNCTION__);
    jack_driver_desc_filler_t filler;
    jack_driver_param_value_t value;

    jack_driver_desc_t* const desc = jack_driver_descriptor_construct("mod-desktop", JackDriverMaster, "MOD Desktop plugin audio backend", &filler);

    value.ui = 48000;
    jack_driver_descriptor_add_parameter(desc, &filler, "rate", 'r', JackDriverParamUInt, &value, nullptr, "Sample rate", nullptr);

    value.ui = 128;
    jack_driver_descriptor_add_parameter(desc, &filler, "period", 'p', JackDriverParamUInt, &value, nullptr, "Frames per period", nullptr);

    value.ui = 0;
    jack_driver_descriptor_add_parameter(desc, &filler, "shmport", 's', JackDriverParamUInt, &value, nullptr, "Shared memory port number", nullptr);

    return desc;
}

SERVER_EXPORT Jack::JackDriverClientInterface* driver_initialize(Jack::JackLockedEngine* engine, Jack::JackSynchro* table, const JSList* params)
{
    jack_nframes_t rate = 48000;
    jack_nframes_t period = 128;
    unsigned int shmport = 0;

    for (const JSList* node = params; node; node = jack_slist_next(node))
    {
        const jack_driver_param_t* const param = (const jack_driver_param_t *) node->data;

        switch (param->character)
        {
        case 'r':
            rate = param->value.ui;
            break;
        case 'p':
            period = param->value.ui;
            break;
        case 's':
            shmport = param->value.ui;
            break;
        }
    }

    if (shmport == 0)
    {
        jack_error("Missing or invalid shared memory port number");
        return nullptr;
    }

    Jack::JackDriverClientInterface* driver = new Jack::ModDesktopAudioDriver("system", "mod-desktop", engine, table, shmport);

    if (driver->Open(period, rate, true, true, 2, 2, false, "", "", 0, 0) == 0)
    {
        printf("%03d:%s OK\n", __LINE__, __FUNCTION__);
        Jack::terminateOnParentExit();
        return driver;
    }

    printf("%03d:%s FAIL\n", __LINE__, __FUNCTION__);
    delete driver;
    return nullptr;
}

} // extern "C"
