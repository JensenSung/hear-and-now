/*
 * hear&now - a simple interactive audio mixer for cool kids
 * copyright (c) 2012 Colin Bayer & Rob Hanlon
 */

#include "audio-darwin.h"

#define AudioQueueBuffer_NUM_BUFFERS 10
#define AudioQueueBuffer_SIZE 2048

typedef struct CallbackWithContext
{
    void (*callback)(void *, uint32_t);
    void *context;
} CallbackWithContext;

typedef struct
{
    const void *pVtbl;

    AudioQueueRef pQueue;
    CFMutableSetRef pBuffersSet;
    CFMutableSetRef pPlayingBuffersSet;

    uint8_t *pAudioData;
    uint32_t audioLength;
    uint32_t currentPosition;

    uint32_t buffersPending;

    bool started;

    CFMutableArrayRef pCallbacksWithContexts;

    HnMutex *pMutex;
} HnAudio_Darwin;

const HnAudio_vtbl _HnAudio_Darwin_vtbl =
{
    hn_darwin_audio_format,
    hn_darwin_audio_watch,
    hn_darwin_audio_write,
    hn_darwin_audio_samples_pending,
    hn_darwin_audio_close,
};

static void signal_applier(const void* value, void *context) {
    CallbackWithContext *pCallbackWithContext = (CallbackWithContext *)value;
    pCallbackWithContext->callback(pCallbackWithContext->context, ((HnAudio_Darwin *)context)->buffersPending);
}

static void signal_pending_all(HnAudio_Darwin *pImpl)
{
    CFMutableArrayRef pCallbacksWithContexts = pImpl->pCallbacksWithContexts;
    CFRange range = CFRangeMake(0, CFArrayGetCount(pCallbacksWithContexts));
    CFArrayApplyFunction(pCallbacksWithContexts, range, signal_applier, pImpl);
}

static void buffer_complete_callback(void *context, AudioQueueRef pQueue, AudioQueueBufferRef pBuffer)
{
    HnAudio_Darwin *pImpl = (HnAudio_Darwin *)context;

    if (pImpl->currentPosition < pImpl->audioLength)
    {
        if (!CFSetContainsValue(pImpl->pPlayingBuffersSet, pBuffer))
        {
            pImpl->buffersPending++;
            CFSetAddValue(pImpl->pPlayingBuffersSet, pBuffer);
        }

        uint32_t amountLeft = pImpl->audioLength - pImpl->currentPosition;

        uint32_t numBytes = amountLeft <= AudioQueueBuffer_SIZE ? amountLeft : AudioQueueBuffer_SIZE;

        memcpy((uint8_t *) pBuffer->mAudioData, &pImpl->pAudioData[pImpl->currentPosition], numBytes);
        pImpl->currentPosition += numBytes;

        hn_mutex_unlock(pImpl->pMutex);

        pBuffer->mAudioDataByteSize = numBytes;
        AudioQueueEnqueueBuffer(pQueue, pBuffer, 0, NULL);
    }
    else
    {
        if (CFSetContainsValue(pImpl->pPlayingBuffersSet, pBuffer))
        {
            pImpl->buffersPending--;
            CFSetRemoveValue(pImpl->pPlayingBuffersSet, pBuffer);
        }

        hn_mutex_unlock(pImpl->pMutex);
    }

    signal_pending_all(pImpl);
}

static AudioStreamBasicDescription convert_format(HnAudioFormat *pFormat) {
    AudioStreamBasicDescription result;

    result.mSampleRate = pFormat->samplesPerSecond;
    result.mFormatID = kAudioFormatLinearPCM;
    result.mFramesPerPacket = 1;
    result.mChannelsPerFrame = pFormat->numberOfChannels;
    result.mBytesPerPacket = (pFormat->sampleResolution / 8) * result.mChannelsPerFrame;
    result.mBytesPerFrame = result.mFramesPerPacket * result.mBytesPerPacket;
    result.mBitsPerChannel = pFormat->sampleResolution;
    result.mReserved = 0;
    result.mFormatFlags = 0;

    return result;
}

HnAudio *hn_darwin_audio_open(HnAudioFormat *pFormat)
{
    HnAudio_Darwin *pImpl = (HnAudio_Darwin *)malloc(sizeof(HnAudio_Darwin));

    pImpl->pVtbl = &_HnAudio_Darwin_vtbl;

    AudioStreamBasicDescription description = convert_format(pFormat);

    AudioQueueNewOutput(&description, buffer_complete_callback, pImpl,
            NULL, kCFRunLoopCommonModes, 0, &pImpl->pQueue);

    pImpl->pBuffersSet = CFSetCreateMutable(NULL, 0, NULL);
    for (int i = 0; i < AudioQueueBuffer_NUM_BUFFERS; i++)
    {
        AudioQueueBufferRef pBuffer;
        AudioQueueAllocateBuffer(pImpl->pQueue, AudioQueueBuffer_SIZE, &pBuffer);
        CFSetAddValue(pImpl->pBuffersSet, pBuffer);
    }

    pImpl->pPlayingBuffersSet = CFSetCreateMutable(NULL, 0, NULL);

    pImpl->pCallbacksWithContexts = CFArrayCreateMutable(NULL, 0, NULL);

    pImpl->pMutex = hn_mutex_create();

    return (HnAudio *)pImpl;
}

HnAudioFormat *hn_darwin_audio_format(HnAudio *pAudio)
{
    // TODO (rob): unstub
    return NULL;
}

void hn_darwin_audio_watch(HnAudio *pAudio, void (*callback)(void *, uint32_t), void *context)
{
    HnAudio_Darwin *pImpl = (HnAudio_Darwin *)pAudio;

    CallbackWithContext *pCallbackWithContext = (CallbackWithContext *)
        calloc(1, sizeof(CallbackWithContext));
    pCallbackWithContext->callback = callback;
    pCallbackWithContext->context = context;
    CFArrayAppendValue(pImpl->pCallbacksWithContexts, pCallbackWithContext);
}

static void reset_audio(HnAudio_Darwin *pImpl)
{
    hn_mutex_lock(pImpl->pMutex);

    if (pImpl->pAudioData != NULL) {
        free(pImpl->pAudioData);
        pImpl->pAudioData = NULL;
        pImpl->audioLength = 0;
        pImpl->currentPosition = 0;
    }

    hn_mutex_unlock(pImpl->pMutex);
}

static void set_audio(HnAudio_Darwin *pImpl, uint8_t *pData, uint32_t len)
{
    reset_audio(pImpl);

    hn_mutex_lock(pImpl->pMutex);

    pImpl->pAudioData = pData;
    pImpl->audioLength = len;
    pImpl->currentPosition = 0;

    hn_mutex_unlock(pImpl->pMutex);
}

static void audio_write_applier(const void *value, void *context) {
    HnAudio_Darwin *pImpl = (HnAudio_Darwin *) context;
    AudioQueueBufferRef pBuffer = (AudioQueueBufferRef) value;

    if (CFSetContainsValue(pImpl->pPlayingBuffersSet, pBuffer))
    {
        return;
    }

    buffer_complete_callback(pImpl, pImpl->pQueue, pBuffer);
}

void hn_darwin_audio_write(HnAudio *pAudio, uint8_t *pData, uint32_t len)
{
    HnAudio_Darwin *pImpl = (HnAudio_Darwin *)pAudio;

    set_audio(pImpl, pData, len);

    CFSetApplyFunction(pImpl->pBuffersSet, audio_write_applier, pImpl);

    hn_mutex_lock(pImpl->pMutex);
    if (!pImpl->started)
    {
        pImpl->started = true;

        hn_mutex_unlock(pImpl->pMutex);

        AudioQueuePrime(pImpl->pQueue, 0, NULL);
        AudioQueueStart(pImpl->pQueue, NULL);
    }
    else
    {
        hn_mutex_unlock(pImpl->pMutex);
    }
}

uint32_t hn_darwin_audio_samples_pending(HnAudio *pAudio)
{
    return ((HnAudio_Darwin *)pAudio)->buffersPending;
}

void hn_darwin_audio_close(HnAudio *pAudio)
{
    HnAudio_Darwin *pImpl = (HnAudio_Darwin *)pAudio;

    reset_audio(pImpl);

    hn_mutex_lock(pImpl->pMutex);

    CFRelease(pImpl->pBuffersSet);
    CFRelease(pImpl->pPlayingBuffersSet);
    AudioQueueDispose(pImpl->pQueue, true);
    CFRelease(pImpl->pCallbacksWithContexts);

    hn_mutex_unlock(pImpl->pMutex);

    hn_mutex_destroy(pImpl->pMutex);

    free(pAudio);
}
