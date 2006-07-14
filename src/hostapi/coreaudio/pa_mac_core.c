/*
 * This is the AUHAL implementation of portaudio.
 *
 * Written by Bjorn Roche of XO Audio LLC, from PA skeleton code.
 * Portions copied from code by Dominic Mazzoni (who wrote a HAL implementation)
 *
 * Dominic's code was based on code by Phil Burk, Darren Gibbs,
 * Gord Peters, Stephane Letz, and Greg Pfiel.
 *
 * Bjorn Roche and XO Audio LLC reserve no rights to this code.
 * The maintainers of PortAudio may redistribute and modify the code and
 * licenses as they deam appropriate.
 *
 * The following people also deserve acknowledgements:
 *
 * Olivier Tristan for feedback and testing
 * Glenn Zelniker and Z-Systems engineering for sponsoring the Blocking I/O
 * interface.
 * 
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2002 Ross Bencina, Phil Burk
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * Any person wishing to distribute modifications to the Software is
 * requested to send the modifications to the original developer so that
 * they can be incorporated into the canonical version.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 @file pa_mac_core
 @author Bjorn Roche
 @brief AUHAL implementation of PortAudio
*/

/* FIXME: not all error conditions call PaUtil_SetLastHostErrorInfo()
 * PaMacCore_SetError() will do this.
 */

#include "pa_mac_core_internal.h"

#include <string.h> /* strlen(), memcmp() etc. */

#include "pa_mac_core.h"
#include "pa_mac_core_utilities.h"
#include "pa_mac_core_blocking.h"

/* prototypes for functions declared in this file */

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

PaError PaMacCore_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex index );

#ifdef __cplusplus
}
#endif /* __cplusplus */

#define RING_BUFFER_ADVANCE_DENOMINATOR (4)

static void Terminate( struct PaUtilHostApiRepresentation *hostApi );
static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate );
static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
                           const PaStreamParameters *inputParameters,
                           const PaStreamParameters *outputParameters,
                           double sampleRate,
                           unsigned long framesPerBuffer,
                           PaStreamFlags streamFlags,
                           PaStreamCallback *streamCallback,
                           void *userData );
static PaError CloseStream( PaStream* stream );
static PaError StartStream( PaStream *stream );
static PaError StopStream( PaStream *stream );
static PaError AbortStream( PaStream *stream );
static PaError IsStreamStopped( PaStream *s );
static PaError IsStreamActive( PaStream *stream );
static PaTime GetStreamTime( PaStream *stream );
static void setStreamStartTime( PaStream *stream );
static OSStatus AudioIOProc( void *inRefCon,
                               AudioUnitRenderActionFlags *ioActionFlags,
                               const AudioTimeStamp *inTimeStamp,
                               UInt32 inBusNumber,
                               UInt32 inNumberFrames,
                               AudioBufferList *ioData );
static double GetStreamCpuLoad( PaStream* stream );

static PaError GetChannelInfo( PaMacAUHAL *auhalHostApi,
                               PaDeviceInfo *deviceInfo,
                               AudioDeviceID macCoreDeviceId,
                               int isInput);

static PaError OpenAndSetupOneAudioUnit(
                                   const PaStreamParameters *inStreamParams,
                                   const PaStreamParameters *outStreamParams,
                                   const unsigned long requestedFramesPerBuffer,
                                   unsigned long *actualInputFramesPerBuffer,
                                   unsigned long *actualOutputFramesPerBuffer,
                                   const PaMacAUHAL *auhalHostApi,
                                   AudioUnit *audioUnit,
                                   AudioConverterRef *srConverter,
                                   AudioDeviceID *audioDevice,
                                   const double sampleRate,
                                   void *refCon );

/* for setting errors. */
#define PA_AUHAL_SET_LAST_HOST_ERROR( errorCode, errorText ) \
    PaUtil_SetLastHostErrorInfo( paInDevelopment, errorCode, errorText )


/*currently, this is only used in initialization, but it might be modified
  to be used when the list of devices changes.*/
static PaError gatherDeviceInfo(PaMacAUHAL *auhalHostApi)
{
    UInt32 size;
    UInt32 propsize;
    VVDBUG(("gatherDeviceInfo()\n"));
    /* -- free any previous allocations -- */
    if( auhalHostApi->devIds )
        PaUtil_GroupFreeMemory(auhalHostApi->allocations, auhalHostApi->devIds);
    auhalHostApi->devIds = NULL;

    /* -- figure out how many devices there are -- */
    AudioHardwareGetPropertyInfo( kAudioHardwarePropertyDevices,
                                  &propsize,
                                  NULL );
    auhalHostApi->devCount = propsize / sizeof( AudioDeviceID );

    VDBUG( ( "Found %ld device(s).\n", auhalHostApi->devCount ) );

    /* -- copy the device IDs -- */
    auhalHostApi->devIds = (AudioDeviceID *)PaUtil_GroupAllocateMemory(
                             auhalHostApi->allocations,
                             propsize );
    if( !auhalHostApi->devIds )
        return paInsufficientMemory;
    AudioHardwareGetProperty( kAudioHardwarePropertyDevices,
                                  &propsize,
                                  auhalHostApi->devIds );
#ifdef MAC_CORE_VERBOSE_DEBUG
    {
       int i;
       for( i=0; i<auhalHostApi->devCount; ++i )
          printf( "Device %d\t: %ld\n", i, auhalHostApi->devIds[i] );
    }
#endif

    size = sizeof(AudioDeviceID);
    auhalHostApi->defaultIn  = kAudioDeviceUnknown;
    auhalHostApi->defaultOut = kAudioDeviceUnknown;

    /* determine the default device. */
    /* I am not sure how these calls to AudioHardwareGetProperty()
       could fail, but in case they do, we use the first available
       device as the default. */
    if( 0 != AudioHardwareGetProperty(kAudioHardwarePropertyDefaultInputDevice,
                     &size,
                     &auhalHostApi->defaultIn) ) {
       int i;
       auhalHostApi->defaultIn  = kAudioDeviceUnknown;
       VDBUG(("Failed to get default input device from OS."));
       VDBUG((" I will substitute the first available input Device."));
       for( i=0; i<auhalHostApi->devCount; ++i ) {
          PaDeviceInfo devInfo;
          if( 0 != GetChannelInfo( auhalHostApi, &devInfo,
                                   auhalHostApi->devIds[i], TRUE ) )
             if( devInfo.maxInputChannels ) {
                auhalHostApi->defaultIn = auhalHostApi->devIds[i];
                break;
             }
       }
    }   
    if( 0 != AudioHardwareGetProperty(kAudioHardwarePropertyDefaultOutputDevice,
                     &size,
                     &auhalHostApi->defaultOut) ) {
       int i;
       auhalHostApi->defaultIn  = kAudioDeviceUnknown;
       VDBUG(("Failed to get default output device from OS."));
       VDBUG((" I will substitute the first available output Device."));
       for( i=0; i<auhalHostApi->devCount; ++i ) {
          PaDeviceInfo devInfo;
          if( 0 != GetChannelInfo( auhalHostApi, &devInfo,
                                   auhalHostApi->devIds[i], FALSE ) )
             if( devInfo.maxOutputChannels ) {
                auhalHostApi->defaultOut = auhalHostApi->devIds[i];
                break;
             }
       }
    }   

    VDBUG( ( "Default in : %ld\n", auhalHostApi->defaultIn  ) );
    VDBUG( ( "Default out: %ld\n", auhalHostApi->defaultOut ) );

    return paNoError;
}

static PaError GetChannelInfo( PaMacAUHAL *auhalHostApi,
                               PaDeviceInfo *deviceInfo,
                               AudioDeviceID macCoreDeviceId,
                               int isInput)
{
    UInt32 propSize;
    PaError err = paNoError;
    UInt32 i;
    int numChannels = 0;
    AudioBufferList *buflist;
    UInt32 frameLatency;

    VVDBUG(("GetChannelInfo()\n"));

    /* Get the number of channels from the stream configuration.
       Fail if we can't get this. */

    err = ERR(AudioDeviceGetPropertyInfo(macCoreDeviceId, 0, isInput, kAudioDevicePropertyStreamConfiguration, &propSize, NULL));
    if (err)
        return err;

    buflist = PaUtil_AllocateMemory(propSize);
    err = ERR(AudioDeviceGetProperty(macCoreDeviceId, 0, isInput, kAudioDevicePropertyStreamConfiguration, &propSize, buflist));
    if (err)
        return err;

    /*FIXME: dealocate buflist*/

    for (i = 0; i < buflist->mNumberBuffers; ++i)
        numChannels += buflist->mBuffers[i].mNumberChannels;

    if (isInput)
        deviceInfo->maxInputChannels = numChannels;
    else
        deviceInfo->maxOutputChannels = numChannels;
      
    if (numChannels > 0) /* do not try to retrieve the latency if there is no channels. */
    {
       /* Get the latency.  Don't fail if we can't get this. */
       /* default to something reasonable */
       deviceInfo->defaultLowInputLatency = .01;
       deviceInfo->defaultHighInputLatency = .10;
       deviceInfo->defaultLowOutputLatency = .01;
       deviceInfo->defaultHighOutputLatency = .10;
       propSize = sizeof(UInt32);
       err = WARNING(AudioDeviceGetProperty(macCoreDeviceId, 0, isInput, kAudioDevicePropertyLatency, &propSize, &frameLatency));
       if (!err)
       {
          double secondLatency = frameLatency / deviceInfo->defaultSampleRate;
          if (isInput)
          {
             deviceInfo->defaultLowInputLatency = 3 * secondLatency;
             deviceInfo->defaultHighInputLatency = 3 * 10 * secondLatency;
          }
          else
          {
             deviceInfo->defaultLowOutputLatency = 3 * secondLatency;
             deviceInfo->defaultHighOutputLatency = 3 * 10 * secondLatency;
          }
       }
    }
    return paNoError;
}

static PaError InitializeDeviceInfo( PaMacAUHAL *auhalHostApi,
                                     PaDeviceInfo *deviceInfo,
                                     AudioDeviceID macCoreDeviceId,
                                     PaHostApiIndex hostApiIndex )
{
    Float64 sampleRate;
    char *name;
    PaError err = paNoError;
    UInt32 propSize;

    VVDBUG(("InitializeDeviceInfo(): macCoreDeviceId=%ld\n", macCoreDeviceId));

    memset(deviceInfo, 0, sizeof(deviceInfo));

    deviceInfo->structVersion = 2;
    deviceInfo->hostApi = hostApiIndex;

    /* Get the device name.  Fail if we can't get it. */
    err = ERR(AudioDeviceGetPropertyInfo(macCoreDeviceId, 0, 0, kAudioDevicePropertyDeviceName, &propSize, NULL));
    if (err)
        return err;

    name = PaUtil_GroupAllocateMemory(auhalHostApi->allocations,propSize);
    if ( !name )
        return paInsufficientMemory;
    err = ERR(AudioDeviceGetProperty(macCoreDeviceId, 0, 0, kAudioDevicePropertyDeviceName, &propSize, name));
    if (err)
        return err;
    deviceInfo->name = name;

    /* Try to get the default sample rate.  Don't fail if we can't get this. */
    propSize = sizeof(Float64);
    err = ERR(AudioDeviceGetProperty(macCoreDeviceId, 0, 0, kAudioDevicePropertyNominalSampleRate, &propSize, &sampleRate));
    if (err)
        deviceInfo->defaultSampleRate = 0.0;
    else
        deviceInfo->defaultSampleRate = sampleRate;

    /* Get the maximum number of input and output channels.  Fail if we can't get this. */

    err = GetChannelInfo(auhalHostApi, deviceInfo, macCoreDeviceId, 1);
    if (err)
        return err;

    err = GetChannelInfo(auhalHostApi, deviceInfo, macCoreDeviceId, 0);
    if (err)
        return err;

    return paNoError;
}

PaError PaMacCore_Initialize( PaUtilHostApiRepresentation **hostApi, PaHostApiIndex hostApiIndex )
{
    PaError result = paNoError;
    int i;
    PaMacAUHAL *auhalHostApi;
    PaDeviceInfo *deviceInfoArray;

    VVDBUG(("PaMacCore_Initialize(): hostApiIndex=%d\n", hostApiIndex));

    auhalHostApi = (PaMacAUHAL*)PaUtil_AllocateMemory( sizeof(PaMacAUHAL) );
    if( !auhalHostApi )
    {
        result = paInsufficientMemory;
        goto error;
    }

    auhalHostApi->allocations = PaUtil_CreateAllocationGroup();
    if( !auhalHostApi->allocations )
    {
        result = paInsufficientMemory;
        goto error;
    }

    auhalHostApi->devIds = NULL;
    auhalHostApi->devCount = 0;

    /* get the info we need about the devices */
    result = gatherDeviceInfo( auhalHostApi );
    if( result != paNoError )
       goto error;

    *hostApi = &auhalHostApi->inheritedHostApiRep;
    (*hostApi)->info.structVersion = 1;
    (*hostApi)->info.type = paCoreAudio;
    (*hostApi)->info.name = "Core Audio";

    (*hostApi)->info.defaultInputDevice = paNoDevice;
    (*hostApi)->info.defaultOutputDevice = paNoDevice;

    (*hostApi)->info.deviceCount = 0;  

    if( auhalHostApi->devCount > 0 )
    {
        (*hostApi)->deviceInfos = (PaDeviceInfo**)PaUtil_GroupAllocateMemory(
                auhalHostApi->allocations, sizeof(PaDeviceInfo*) * auhalHostApi->devCount);
        if( !(*hostApi)->deviceInfos )
        {
            result = paInsufficientMemory;
            goto error;
        }

        /* allocate all device info structs in a contiguous block */
        deviceInfoArray = (PaDeviceInfo*)PaUtil_GroupAllocateMemory(
                auhalHostApi->allocations, sizeof(PaDeviceInfo) * auhalHostApi->devCount );
        if( !deviceInfoArray )
        {
            result = paInsufficientMemory;
            goto error;
        }

        for( i=0; i < auhalHostApi->devCount; ++i )
        {
            int err;
            err = InitializeDeviceInfo( auhalHostApi, &deviceInfoArray[i],
                                      auhalHostApi->devIds[i],
                                      hostApiIndex );
            if (err == paNoError)
            { /* copy some info and set the defaults */
                (*hostApi)->deviceInfos[(*hostApi)->info.deviceCount] = &deviceInfoArray[i];
                if (auhalHostApi->devIds[i] == auhalHostApi->defaultIn)
                    (*hostApi)->info.defaultInputDevice = (*hostApi)->info.deviceCount;
                if (auhalHostApi->devIds[i] == auhalHostApi->defaultOut)
                    (*hostApi)->info.defaultOutputDevice = (*hostApi)->info.deviceCount;
                (*hostApi)->info.deviceCount++;
            }
            else
            { /* there was an error. we need to shift the devices down, so we ignore this one */
                int j;
                auhalHostApi->devCount--;
                for( j=i; j<auhalHostApi->devCount; ++j )
                   auhalHostApi->devIds[j] = auhalHostApi->devIds[j+1];
                i--;
            }
        }
    }

    (*hostApi)->Terminate = Terminate;
    (*hostApi)->OpenStream = OpenStream;
    (*hostApi)->IsFormatSupported = IsFormatSupported;

    PaUtil_InitializeStreamInterface( &auhalHostApi->callbackStreamInterface,
                                      CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped,
                                      IsStreamActive,
                                      GetStreamTime, GetStreamCpuLoad,
                                      PaUtil_DummyRead, PaUtil_DummyWrite,
                                      PaUtil_DummyGetReadAvailable,
                                      PaUtil_DummyGetWriteAvailable );

    PaUtil_InitializeStreamInterface( &auhalHostApi->blockingStreamInterface,
                                      CloseStream, StartStream,
                                      StopStream, AbortStream, IsStreamStopped,
                                      IsStreamActive,
                                      GetStreamTime, PaUtil_DummyGetCpuLoad,
                                      ReadStream, WriteStream,
                                      GetStreamReadAvailable,
                                      GetStreamWriteAvailable );

    return result;

error:
    if( auhalHostApi )
    {
        if( auhalHostApi->allocations )
        {
            PaUtil_FreeAllAllocations( auhalHostApi->allocations );
            PaUtil_DestroyAllocationGroup( auhalHostApi->allocations );
        }
                
        PaUtil_FreeMemory( auhalHostApi );
    }
    return result;
}


static void Terminate( struct PaUtilHostApiRepresentation *hostApi )
{
    PaMacAUHAL *auhalHostApi = (PaMacAUHAL*)hostApi;

    VVDBUG(("Terminate()\n"));

    /*
        IMPLEMENT ME:
            - clean up any resources not handled by the allocation group
        TODO: Double check that everything is handled by alloc group
    */

    if( auhalHostApi->allocations )
    {
        PaUtil_FreeAllAllocations( auhalHostApi->allocations );
        PaUtil_DestroyAllocationGroup( auhalHostApi->allocations );
    }

    PaUtil_FreeMemory( auhalHostApi );
}


static PaError IsFormatSupported( struct PaUtilHostApiRepresentation *hostApi,
                                  const PaStreamParameters *inputParameters,
                                  const PaStreamParameters *outputParameters,
                                  double sampleRate )
{
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;

    VVDBUG(("IsFormatSupported(): in chan=%d, in fmt=%ld, out chan=%d, out fmt=%ld sampleRate=%g\n",
                inputParameters  ? inputParameters->channelCount  : -1,
                inputParameters  ? inputParameters->sampleFormat  : -1,
                outputParameters ? outputParameters->channelCount : -1,
                outputParameters ? outputParameters->sampleFormat : -1,
                (float) sampleRate ));
 
    /** These first checks are standard PA checks. We do some fancier checks
        later. */
    if( inputParameters )
    {
        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;

        /* all standard sample formats are supported by the buffer adapter,
            this implementation doesn't support any custom sample formats */
        if( inputSampleFormat & paCustomFormat )
            return paSampleFormatNotSupported;
            
        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */

        if( inputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        /* check that input device can support inputChannelCount */
        if( inputChannelCount > hostApi->deviceInfos[ inputParameters->device ]->maxInputChannels )
            return paInvalidChannelCount;
    }
    else
    {
        inputChannelCount = 0;
    }

    if( outputParameters )
    {
        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;

        /* all standard sample formats are supported by the buffer adapter,
            this implementation doesn't support any custom sample formats */
        if( outputSampleFormat & paCustomFormat )
            return paSampleFormatNotSupported;
            
        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */

        if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        /* check that output device can support outputChannelCount */
        if( outputChannelCount > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels )
            return paInvalidChannelCount;

    }
    else
    {
        outputChannelCount = 0;
    }
 
    /* FEEDBACK */
    /*        I think the only way to check a given format SR combo is     */
    /*        to try opening it. This could be disruptive, is that Okay?   */
    /*        The alternative is to just read off available sample rates,  */
    /*        but this will not work %100 of the time (eg, a device that   */
    /*        supports N output at one rate but only N/2 at a higher rate.)*/

    /* The following code opens the device with the requested parameters to
       see if it works. */
    {
       PaError err;
       PaStream *s;
       err = OpenStream( hostApi, &s, inputParameters, outputParameters,
                           sampleRate, 1024, 0, (PaStreamCallback *)1, NULL );
       if( err != paNoError && err != paInvalidSampleRate )
          DBUG( ( "OpenStream @ %g returned: %d: %s\n",
                  (float) sampleRate, err, Pa_GetErrorText( err ) ) );
       if( err ) 
          return err;
       err = CloseStream( s );
       if( err ) {
          /* FEEDBACK: is this more serious? should we assert? */
          DBUG( ( "WARNING: could not close Stream. %d: %s\n",
                  err, Pa_GetErrorText( err ) ) );
       }
    }

    return paFormatIsSupported;
}

static PaError OpenAndSetupOneAudioUnit(
                                   const PaStreamParameters *inStreamParams,
                                   const PaStreamParameters *outStreamParams,
                                   const unsigned long requestedFramesPerBuffer,
                                   unsigned long *actualInputFramesPerBuffer,
                                   unsigned long *actualOutputFramesPerBuffer,
                                   const PaMacAUHAL *auhalHostApi,
                                   AudioUnit *audioUnit,
                                   AudioConverterRef *srConverter,
                                   AudioDeviceID *audioDevice,
                                   const double sampleRate,
                                   void *refCon )
{
    ComponentDescription desc;
    Component comp;
    /*An Apple TN suggests using CAStreamBasicDescription, but that is C++*/
    AudioStreamBasicDescription desiredFormat;
    OSErr result = noErr;
    PaError paResult = paNoError;
    int line = 0;
    UInt32 callbackKey;
    AURenderCallbackStruct rcbs;
    unsigned long macInputStreamFlags  = paMacCorePlayNice;
    unsigned long macOutputStreamFlags = paMacCorePlayNice;

    VVDBUG(("OpenAndSetupOneAudioUnit(): in chan=%d, in fmt=%ld, out chan=%d, out fmt=%ld, requestedFramesPerBuffer=%ld\n",
                inStreamParams  ? inStreamParams->channelCount  : -1,
                inStreamParams  ? inStreamParams->sampleFormat  : -1,
                outStreamParams ? outStreamParams->channelCount : -1,
                outStreamParams ? outStreamParams->sampleFormat : -1,
                requestedFramesPerBuffer ));

    /* -- handle the degenerate case  -- */
    if( !inStreamParams && !outStreamParams ) {
       *audioUnit = NULL;
       *audioDevice = kAudioDeviceUnknown;
       return paNoError;
    }

    /* -- get the user's api specific info, if they set any -- */
    if( inStreamParams && inStreamParams->hostApiSpecificStreamInfo )
       macInputStreamFlags=
            ((paMacCoreStreamInfo*)inStreamParams->hostApiSpecificStreamInfo)
                  ->flags;
    if( outStreamParams && outStreamParams->hostApiSpecificStreamInfo )
       macOutputStreamFlags=
            ((paMacCoreStreamInfo*)outStreamParams->hostApiSpecificStreamInfo)
                  ->flags;
    /* Override user's flags here, if desired for testing. */

    /*
     * The HAL AU is a Mac OS style "component".
     * the first few steps deal with that.
     * Later steps work on a combination of Mac OS
     * components and the slightly lower level
     * HAL.
     */

    /* -- describe the output type AudioUnit -- */
    /*  Note: for the default AudioUnit, we could use the
     *  componentSubType value kAudioUnitSubType_DefaultOutput;
     *  but I don't think that's relevant here.
     */
    desc.componentType         = kAudioUnitType_Output;
    desc.componentSubType      = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags        = 0;
    desc.componentFlagsMask    = 0;
    /* -- find the component -- */
    comp = FindNextComponent( NULL, &desc );
    if( !comp )
    {
       DBUG( ( "AUHAL component not found." ) );
       *audioUnit = NULL;
       *audioDevice = kAudioDeviceUnknown;
       return paUnanticipatedHostError;
    }
    /* -- open it -- */
    result = OpenAComponent( comp, audioUnit );
    if( result )
    {
       DBUG( ( "Failed to open AUHAL component." ) );
       *audioUnit = NULL;
       *audioDevice = kAudioDeviceUnknown;
       return ERR( result );
    }
    /* -- prepare a little error handling logic / hackery -- */
#define ERR_WRAP(mac_err) do { result = mac_err ; line = __LINE__ ; if ( result != noErr ) goto error ; } while(0)

    /* -- if there is input, we have to explicitly enable input -- */
    if( inStreamParams )
    {
       UInt32 enableIO;
       enableIO = 1;
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                 kAudioOutputUnitProperty_EnableIO,
                 kAudioUnitScope_Input,
                 INPUT_ELEMENT,
                 &enableIO,
                 sizeof(enableIO) ) );
    }
    /* -- if there is no output, we must explicitly disable output -- */
    if( !outStreamParams )
    {
       UInt32 enableIO;
       enableIO = 0;
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                 kAudioOutputUnitProperty_EnableIO,
                 kAudioUnitScope_Output,
                 OUTPUT_ELEMENT,
                 &enableIO,
                 sizeof(enableIO) ) );
    }
    /* -- set the devices -- */
    /* make sure input and output are the same device if we are doing input and
       output. */
    if( inStreamParams && outStreamParams )
       assert( outStreamParams->device == inStreamParams->device );
    if( inStreamParams )
    {
       *audioDevice = auhalHostApi->devIds[inStreamParams->device] ;
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                    kAudioOutputUnitProperty_CurrentDevice,
                    kAudioUnitScope_Global,
                    INPUT_ELEMENT,
                    audioDevice,
                    sizeof(AudioDeviceID) ) );
    }
    if( outStreamParams )
    {
       *audioDevice = auhalHostApi->devIds[outStreamParams->device] ;
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                    kAudioOutputUnitProperty_CurrentDevice,
                    kAudioUnitScope_Global,
                    OUTPUT_ELEMENT,
                    audioDevice,
                    sizeof(AudioDeviceID) ) );
    }

    /* -- set format -- */
    bzero( &desiredFormat, sizeof(desiredFormat) );
    desiredFormat.mFormatID         = kAudioFormatLinearPCM ;
    desiredFormat.mFormatFlags      = kAudioFormatFlagsNativeFloatPacked;
    desiredFormat.mFramesPerPacket  = 1;
    desiredFormat.mBitsPerChannel   = sizeof( float ) * 8;

    result = 0;
    /*  set device format first, but only touch the device if the user asked */
    if( inStreamParams ) {
       /*The callback never calls back if we don't set the FPB */
       /*This seems wierd, because I would think setting anything on the device
         would be disruptive.*/
       paResult = setBestFramesPerBuffer( *audioDevice, FALSE,
                                          requestedFramesPerBuffer,
                                          actualInputFramesPerBuffer );
       if( paResult ) goto error;
       if( macInputStreamFlags & paMacCore_ChangeDeviceParameters ) {
          bool requireExact;
          requireExact=macInputStreamFlags&paMacCore_FailIfConversionRequired;
          paResult = setBestSampleRateForDevice( *audioDevice, FALSE,
                                                 requireExact, sampleRate );
          if( paResult ) goto error;
       }
       if( actualInputFramesPerBuffer && actualOutputFramesPerBuffer )
          *actualOutputFramesPerBuffer = *actualInputFramesPerBuffer ;
    }
    if( outStreamParams && !inStreamParams ) {
       /*The callback never calls back if we don't set the FPB */
       /*This seems wierd, because I would think setting anything on the device
         would be disruptive.*/
       paResult = setBestFramesPerBuffer( *audioDevice, TRUE,
                                          requestedFramesPerBuffer,
                                          actualOutputFramesPerBuffer );
       if( paResult ) goto error;
       if( macOutputStreamFlags & paMacCore_ChangeDeviceParameters ) {
          bool requireExact;
          requireExact=macOutputStreamFlags&paMacCore_FailIfConversionRequired;
          paResult = setBestSampleRateForDevice( *audioDevice, TRUE,
                                                 requireExact, sampleRate );
          if( paResult ) goto error;
       }
    }

    /* -- set the quality of the output converter -- */
    if( outStreamParams ) {
       UInt32 value = kAudioConverterQuality_Max;
       switch( macOutputStreamFlags & 0x0700 ) {
       case 0x0100: /*paMacCore_ConversionQualityMin:*/
          value=kRenderQuality_Min;
          break;
       case 0x0200: /*paMacCore_ConversionQualityLow:*/
          value=kRenderQuality_Low;
          break;
       case 0x0300: /*paMacCore_ConversionQualityMedium:*/
          value=kRenderQuality_Medium;
          break;
       case 0x0400: /*paMacCore_ConversionQualityHigh:*/
          value=kRenderQuality_High;
          break;
       }
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                    kAudioUnitProperty_RenderQuality,
                    kAudioUnitScope_Global,
                    OUTPUT_ELEMENT,
                    &value,
                    sizeof(value) ) );
    }
    /* now set the format on the Audio Units. */
    if( outStreamParams )
    {
       desiredFormat.mSampleRate    =sampleRate;
       desiredFormat.mBytesPerPacket=sizeof(float)*outStreamParams->channelCount;
       desiredFormat.mBytesPerFrame =sizeof(float)*outStreamParams->channelCount;
       desiredFormat.mChannelsPerFrame = outStreamParams->channelCount;
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                            kAudioUnitProperty_StreamFormat,
                            kAudioUnitScope_Input,
                            OUTPUT_ELEMENT,
                            &desiredFormat,
                            sizeof(AudioStreamBasicDescription) ) );
    }
    if( inStreamParams )
    {
       AudioStreamBasicDescription sourceFormat;
       UInt32 size = sizeof( AudioStreamBasicDescription );

       /* keep the sample rate of the device, or we confuse AUHAL */
       ERR_WRAP( AudioUnitGetProperty( *audioUnit,
                            kAudioUnitProperty_StreamFormat,
                            kAudioUnitScope_Input,
                            INPUT_ELEMENT,
                            &sourceFormat,
                            &size ) );
       desiredFormat.mSampleRate = sourceFormat.mSampleRate;
       desiredFormat.mBytesPerPacket=sizeof(float)*inStreamParams->channelCount;
       desiredFormat.mBytesPerFrame =sizeof(float)*inStreamParams->channelCount;
       desiredFormat.mChannelsPerFrame = inStreamParams->channelCount;
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                            kAudioUnitProperty_StreamFormat,
                            kAudioUnitScope_Output,
                            INPUT_ELEMENT,
                            &desiredFormat,
                            sizeof(AudioStreamBasicDescription) ) );
    }
    /* set the maximumFramesPerSlice */
    /* not doing this causes real problems
       (eg. the callback might not be called). The idea of setting both this
       and the frames per buffer on the device is that we'll be most likely
       to actually get the frame size we requested in the callback with the
       minimum latency. */
    if( outStreamParams ) {
       UInt32 size = sizeof( *actualOutputFramesPerBuffer );
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                            kAudioUnitProperty_MaximumFramesPerSlice,
                            kAudioUnitScope_Input,
                            OUTPUT_ELEMENT,
                            actualOutputFramesPerBuffer,
                            sizeof(unsigned long) ) );
       ERR_WRAP( AudioUnitGetProperty( *audioUnit,
                            kAudioUnitProperty_MaximumFramesPerSlice,
                            kAudioUnitScope_Global,
                            OUTPUT_ELEMENT,
                            actualOutputFramesPerBuffer,
                            &size ) );
    }
    if( inStreamParams ) {
       /*UInt32 size = sizeof( *actualInputFramesPerBuffer );*/
       ERR_WRAP( AudioUnitSetProperty( *audioUnit,
                            kAudioUnitProperty_MaximumFramesPerSlice,
                            kAudioUnitScope_Output,
                            INPUT_ELEMENT,
                            actualInputFramesPerBuffer,
                            sizeof(unsigned long) ) );
/* Don't know why this causes problems
       ERR_WRAP( AudioUnitGetProperty( *audioUnit,
                            kAudioUnitProperty_MaximumFramesPerSlice,
                            kAudioUnitScope_Global, //Output,
                            INPUT_ELEMENT,
                            actualInputFramesPerBuffer,
                            &size ) );
*/
    }

    /* -- if we have input, we may need to setup an SR converter -- */
    /* even if we got the sample rate we asked for, we need to do
       the conversion in case another program changes the underlying SR. */
    /* FIXME: I think we need to monitor stream and change the converter if the incoming format changes. */
    if( inStreamParams ) {
       AudioStreamBasicDescription desiredFormat;
       AudioStreamBasicDescription sourceFormat;
       UInt32 sourceSize = sizeof( sourceFormat );
       bzero( &desiredFormat, sizeof(desiredFormat) );
       desiredFormat.mSampleRate       = sampleRate;
       desiredFormat.mFormatID         = kAudioFormatLinearPCM ;
       desiredFormat.mFormatFlags      = kAudioFormatFlagsNativeFloatPacked;
       desiredFormat.mFramesPerPacket  = 1;
       desiredFormat.mBitsPerChannel   = sizeof( float ) * 8;
       desiredFormat.mBytesPerPacket=sizeof(float)*inStreamParams->channelCount;
       desiredFormat.mBytesPerFrame =sizeof(float)*inStreamParams->channelCount;
       desiredFormat.mChannelsPerFrame = inStreamParams->channelCount;

       /* get the source format */
       ERR_WRAP( AudioUnitGetProperty(
                         *audioUnit,
                         kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Output,
                         INPUT_ELEMENT,
                         &sourceFormat,
                         &sourceSize ) );

       if( desiredFormat.mSampleRate != sourceFormat.mSampleRate )
       {
          UInt32 value = kAudioConverterQuality_Max;
          switch( macInputStreamFlags & 0x0700 ) {
          case 0x0100: /*paMacCore_ConversionQualityMin:*/
             value=kAudioConverterQuality_Min;
             break;
          case 0x0200: /*paMacCore_ConversionQualityLow:*/
             value=kAudioConverterQuality_Low;
             break;
          case 0x0300: /*paMacCore_ConversionQualityMedium:*/
             value=kAudioConverterQuality_Medium;
             break;
          case 0x0400: /*paMacCore_ConversionQualityHigh:*/
             value=kAudioConverterQuality_High;
             break;
          }
          VDBUG(( "Creating sample rate converter for input"
                  " to convert from %g to %g\n",
                  (float)sourceFormat.mSampleRate,
                  (float)desiredFormat.mSampleRate ) );
          /* create our converter */
          ERR_WRAP( AudioConverterNew( 
                             &sourceFormat,
                             &desiredFormat,
                             srConverter ) );
          /* Set quality */
          ERR_WRAP( AudioConverterSetProperty(
                             *srConverter,
                             kAudioConverterSampleRateConverterQuality,
                             sizeof( value ),
                             &value ) );
       }
    }
    /* -- set IOProc (callback) -- */
    callbackKey = outStreamParams ? kAudioUnitProperty_SetRenderCallback
                                  : kAudioOutputUnitProperty_SetInputCallback ;
    rcbs.inputProc = AudioIOProc;
    rcbs.inputProcRefCon = refCon;
    ERR_WRAP( AudioUnitSetProperty(
                               *audioUnit,
                               callbackKey,
                               kAudioUnitScope_Output,
                               outStreamParams ? OUTPUT_ELEMENT : INPUT_ELEMENT,
                               &rcbs,
                               sizeof(rcbs)) );

    if( inStreamParams && outStreamParams && *srConverter )
           ERR_WRAP( AudioUnitSetProperty(
                               *audioUnit,
                               kAudioOutputUnitProperty_SetInputCallback,
                               kAudioUnitScope_Output,
                               INPUT_ELEMENT,
                               &rcbs,
                               sizeof(rcbs)) );

    /*IMPLEMENTME: may need to worry about channel mapping.*/

    /* initialize the audio unit */
    ERR_WRAP( AudioUnitInitialize(*audioUnit) );

    if( inStreamParams && outStreamParams )
       VDBUG( ("Opened device %ld for input and output.\n", *audioDevice ) );
    else if( inStreamParams )
       VDBUG( ("Opened device %ld for input.\n", *audioDevice ) );
    else if( outStreamParams )
       VDBUG( ("Opened device %ld for output.\n", *audioDevice ) );
    return paNoError;
#undef ERR_WRAP

    error:
       CloseComponent( *audioUnit );
       *audioUnit = NULL;
       if( result )
          return PaMacCore_SetError( result, line, 1 );
       return paResult;
}

/* see pa_hostapi.h for a list of validity guarantees made about OpenStream parameters */
static PaError OpenStream( struct PaUtilHostApiRepresentation *hostApi,
                           PaStream** s,
                           const PaStreamParameters *inputParameters,
                           const PaStreamParameters *outputParameters,
                           double sampleRate,
                           unsigned long framesPerBuffer,
                           PaStreamFlags streamFlags,
                           PaStreamCallback *streamCallback,
                           void *userData )
{
    PaError result = paNoError;
    PaMacAUHAL *auhalHostApi = (PaMacAUHAL*)hostApi;
    PaMacCoreStream *stream = 0;
    int inputChannelCount, outputChannelCount;
    PaSampleFormat inputSampleFormat, outputSampleFormat;
    PaSampleFormat hostInputSampleFormat, hostOutputSampleFormat;
    VVDBUG(("OpenStream(): in chan=%d, in fmt=%ld, out chan=%d, out fmt=%ld SR=%g, FPB=%ld\n",
                inputParameters  ? inputParameters->channelCount  : -1,
                inputParameters  ? inputParameters->sampleFormat  : -1,
                outputParameters ? outputParameters->channelCount : -1,
                outputParameters ? outputParameters->sampleFormat : -1,
                (float) sampleRate,
                framesPerBuffer ));
    VDBUG( ("Opening Stream.\n") );

    /*These first few bits of code are from paSkeleton with few modifications.*/
    if( inputParameters )
    {
        inputChannelCount = inputParameters->channelCount;
        inputSampleFormat = inputParameters->sampleFormat;

        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */

        if( inputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        /* check that input device can support inputChannelCount */
        if( inputChannelCount > hostApi->deviceInfos[ inputParameters->device ]->maxInputChannels )
            return paInvalidChannelCount;

        /* Host supports interleaved float32 */
        hostInputSampleFormat = paFloat32;
    }
    else
    {
        inputChannelCount = 0;
        inputSampleFormat = hostInputSampleFormat = paFloat32; /* Surpress 'uninitialised var' warnings. */
    }

    if( outputParameters )
    {
        outputChannelCount = outputParameters->channelCount;
        outputSampleFormat = outputParameters->sampleFormat;
        
        /* unless alternate device specification is supported, reject the use of
            paUseHostApiSpecificDeviceSpecification */

        if( outputParameters->device == paUseHostApiSpecificDeviceSpecification )
            return paInvalidDevice;

        /* check that output device can support inputChannelCount */
        if( outputChannelCount > hostApi->deviceInfos[ outputParameters->device ]->maxOutputChannels )
            return paInvalidChannelCount;

        /* Host supports interleaved float32 */
        hostOutputSampleFormat = paFloat32;
    }
    else
    {
        outputChannelCount = 0;
        outputSampleFormat = hostOutputSampleFormat = paFloat32; /* Surpress 'uninitialized var' warnings. */
    }

    /* validate platform specific flags */
    if( (streamFlags & paPlatformSpecificFlags) != 0 )
        return paInvalidFlag; /* unexpected platform specific flag */

    stream = (PaMacCoreStream*)PaUtil_AllocateMemory( sizeof(PaMacCoreStream) );
    if( !stream )
    {
        result = paInsufficientMemory;
        goto error;
    }

    /* If we fail after this point, we my be left in a bad state, with
       some data structures setup and others not. So, first thing we
       do is initialize everything so that if we fail, we know what hasn't
       been touched.
     */

    stream->inputAudioBufferList.mBuffers[0].mData = NULL;
    stream->inputRingBuffer.buffer = NULL;
    bzero( &stream->blio, sizeof( PaMacBlio ) );
/*
    stream->blio.inputRingBuffer.buffer = NULL;
    stream->blio.outputRingBuffer.buffer = NULL;
    stream->blio.inputSampleFormat = inputParameters?inputParameters->sampleFormat:0;
    stream->blio.inputSampleSize = computeSampleSizeFromFormat(stream->blio.inputSampleFormat);
    stream->blio.outputSampleFormat=outputParameters?outputParameters->sampleFormat:0;
    stream->blio.outputSampleSize = computeSampleSizeFromFormat(stream->blio.outputSampleFormat);
*/
    stream->inputSRConverter = NULL;
    stream->inputUnit = NULL;
    stream->outputUnit = NULL;
    stream->inputFramesPerBuffer = 0;
    stream->outputFramesPerBuffer = 0;
    stream->bufferProcessorIsInitialized = FALSE;

    /* assert( streamCallback ) ; */ /* only callback mode is implemented */
    if( streamCallback )
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                        &auhalHostApi->callbackStreamInterface,
                                        streamCallback, userData );
    }
    else
    {
        PaUtil_InitializeStreamRepresentation( &stream->streamRepresentation,
                                        &auhalHostApi->blockingStreamInterface,
					BlioCallback, &stream->blio );
    }

    PaUtil_InitializeCpuLoadMeasurer( &stream->cpuLoadMeasurer, sampleRate );

    /* -- handle paFramesPerBufferUnspecified -- */
    if( framesPerBuffer == paFramesPerBufferUnspecified ) {
       long requested = 64;
       if( inputParameters )
          requested = MAX( requested, inputParameters->suggestedLatency * sampleRate / 2 );
       if( outputParameters )
          requested = MAX( requested, outputParameters->suggestedLatency *sampleRate / 2 );
       VDBUG( ("Block Size unspecified. Based on Latency, the user wants a Block Size near: %ld.\n",
              requested ) );
       if( requested <= 64 ) {
          /*requested a realtively low latency. make sure this is in range of devices */
          /*try to get the device's min natural buffer size and use that (but no smaller than 64).*/
          AudioValueRange audioRange;
          size_t size = sizeof( audioRange );
          if( inputParameters ) {
             WARNING( result = AudioDeviceGetProperty( auhalHostApi->devIds[inputParameters->device],
                                          0,
                                          false,
                                          kAudioDevicePropertyBufferFrameSizeRange,
                                          &size, &audioRange ) );
             if( result )
                requested = MAX( requested, audioRange.mMinimum );
          }
          if( outputParameters ) {
             WARNING( result = AudioDeviceGetProperty( auhalHostApi->devIds[outputParameters->device],
                                          0,
                                          false,
                                          kAudioDevicePropertyBufferFrameSizeRange,
                                          &size, &audioRange ) );
             if( result )
                requested = MAX( requested, audioRange.mMinimum );
          }
       } else {
          /* requested a realtively high latency. make sure this is in range of devices */
          /*try to get the device's max natural buffer size and use that (but no larger than 1024).*/
          AudioValueRange audioRange;
          size_t size = sizeof( audioRange );
          requested = MIN( requested, 1024 );
          if( inputParameters ) {
             WARNING( result = AudioDeviceGetProperty( auhalHostApi->devIds[inputParameters->device],
                                          0,
                                          false,
                                          kAudioDevicePropertyBufferFrameSizeRange,
                                          &size, &audioRange ) );
             if( result )
                requested = MIN( requested, audioRange.mMaximum );
          }
          if( outputParameters ) {
             WARNING( result = AudioDeviceGetProperty( auhalHostApi->devIds[outputParameters->device],
                                          0,
                                          false,
                                          kAudioDevicePropertyBufferFrameSizeRange,
                                          &size, &audioRange ) );
             if( result )
                requested = MIN( requested, audioRange.mMaximum );
          }
       }
       /* -- double check ranges -- */
       if( requested > 1024 ) requested = 1024;
       if( requested < 64 ) requested = 64;
       VDBUG(("After querying hardware, setting block size to %ld.\n", requested));
       framesPerBuffer = requested;
    }

    /* -- Now we actually open and setup streams. -- */
    if( inputParameters && outputParameters && outputParameters->device == inputParameters->device )
    { /* full duplex. One device. */
       result = OpenAndSetupOneAudioUnit( inputParameters,
                                          outputParameters,
                                          framesPerBuffer,
                                          &(stream->inputFramesPerBuffer),
                                          &(stream->outputFramesPerBuffer),
                                          auhalHostApi,
                                          &(stream->inputUnit),
                                          &(stream->inputSRConverter),
                                          &(stream->inputDevice),
                                          sampleRate,
                                          stream );
       stream->outputUnit = stream->inputUnit;
       stream->outputDevice = stream->inputDevice;
       if( result != paNoError )
           goto error;
    }
    else
    { /* full duplex, different devices OR simplex */
       result = OpenAndSetupOneAudioUnit( NULL,
                                          outputParameters,
                                          framesPerBuffer,
                                          NULL,
                                          &(stream->outputFramesPerBuffer),
                                          auhalHostApi,
                                          &(stream->outputUnit),
                                          NULL,
                                          &(stream->outputDevice),
                                          sampleRate,
                                          stream );
       if( result != paNoError )
           goto error;
       result = OpenAndSetupOneAudioUnit( inputParameters,
                                          NULL,
                                          framesPerBuffer,
                                          &(stream->inputFramesPerBuffer),
                                          NULL,
                                          auhalHostApi,
                                          &(stream->inputUnit),
                                          &(stream->inputSRConverter),
                                          &(stream->inputDevice),
                                          sampleRate,
                                          stream );
       if( result != paNoError )
           goto error;
    }

    if( stream->inputUnit ) {
       const size_t szfl = sizeof(float);
       /* setup the AudioBufferList used for input */
       bzero( &stream->inputAudioBufferList, sizeof( AudioBufferList ) );
       stream->inputAudioBufferList.mNumberBuffers = 1;
       stream->inputAudioBufferList.mBuffers[0].mNumberChannels
                 = inputChannelCount;
       stream->inputAudioBufferList.mBuffers[0].mDataByteSize
                 = stream->inputFramesPerBuffer*inputChannelCount*szfl;
       stream->inputAudioBufferList.mBuffers[0].mData
                 = (float *) calloc(
                               stream->inputFramesPerBuffer*inputChannelCount,
                               szfl );
       if( !stream->inputAudioBufferList.mBuffers[0].mData )
       {
          result = paInsufficientMemory;
          goto error;
       }
        
       /*
        * If input and output devs are different or we are doing SR conversion,
        * we also need a
        * ring buffer to store inpt data while waiting for output
        * data.
        */
       if( (stream->outputUnit && stream->inputUnit != stream->outputUnit)
           || stream->inputSRConverter )
       {
          /* May want the ringSize ot initial position in
             ring buffer to depend somewhat on sample rate change */

          void *data;
          long ringSize;

          ringSize = computeRingBufferSize( inputParameters,
                                            outputParameters,
                                            stream->inputFramesPerBuffer,
                                            stream->outputFramesPerBuffer,
                                            sampleRate );
          /*ringSize <<= 4; *//*16x bigger, for testing */


          /*now, we need to allocate memory for the ring buffer*/
          data = calloc( ringSize, szfl );
          if( !data )
          {
             result = paInsufficientMemory;
             goto error;
          }

          /* now we can initialize the ring buffer */
          assert( 0 ==
            RingBuffer_Init( &stream->inputRingBuffer,
                             ringSize*szfl, data ) );
          /* advance the read point a little, so we are reading from the
             middle of the buffer */
          if( stream->outputUnit )
             RingBuffer_AdvanceWriteIndex( &stream->inputRingBuffer, ringSize*szfl / RING_BUFFER_ADVANCE_DENOMINATOR );
       }
    }

    /* -- initialize Blio Buffer Processors -- */
    if( !streamCallback )
    {
       long ringSize;

       ringSize = computeRingBufferSize( inputParameters,
                                         outputParameters,
                                         stream->inputFramesPerBuffer,
                                         stream->outputFramesPerBuffer,
                                         sampleRate );
       result = initializeBlioRingBuffers( &stream->blio,
              inputParameters?inputParameters->sampleFormat:0 ,
              outputParameters?outputParameters->sampleFormat:0 ,
              MAX(stream->inputFramesPerBuffer,stream->outputFramesPerBuffer),
              ringSize,
              inputParameters?inputChannelCount:0 ,
              outputParameters?outputChannelCount:0 ) ;
       if( result != paNoError )
          goto error;
    }

    /* -- initialize Buffer Processor -- */
    {
       unsigned long maxHostFrames = stream->inputFramesPerBuffer;
       if( stream->outputFramesPerBuffer > maxHostFrames )
          maxHostFrames = stream->outputFramesPerBuffer;
       result = PaUtil_InitializeBufferProcessor( &stream->bufferProcessor,
                 inputChannelCount, inputSampleFormat,
                 hostInputSampleFormat,
                 outputChannelCount, outputSampleFormat,
                 hostOutputSampleFormat,
                 sampleRate,
                 streamFlags,
                 framesPerBuffer,
                 /* If sample rate conversion takes place, the buffer size
                    will not be known. */
                 maxHostFrames,
                 stream->inputSRConverter
                              ? paUtilUnknownHostBufferSize
                              : paUtilBoundedHostBufferSize,
                 streamCallback ? streamCallback : BlioCallback,
                 streamCallback ? userData : &stream->blio );
       if( result != paNoError )
           goto error;
    }
    stream->bufferProcessorIsInitialized = TRUE;

    /*
        IMPLEMENT ME: initialise the following fields with estimated or actual
        values.
        I think this is okay the way it is br 12/1/05
        maybe need to change input latency estimate if IO devs differ
    */
    stream->streamRepresentation.streamInfo.inputLatency =
            PaUtil_GetBufferProcessorInputLatency(&stream->bufferProcessor);
    stream->streamRepresentation.streamInfo.outputLatency =
            PaUtil_GetBufferProcessorOutputLatency(&stream->bufferProcessor);
    stream->streamRepresentation.streamInfo.sampleRate = sampleRate;

    stream->sampleRate  = sampleRate;
    stream->userInChan  = inputChannelCount;
    stream->userOutChan = outputChannelCount;

    stream->isTimeSet   = FALSE;
    stream->state = STOPPED;
    stream->xrunFlags = 0;

    *s = (PaStream*)stream;

    return result;

error:
    CloseStream( stream );
    return result;
}

PaTime GetStreamTime( PaStream *s )
{
   /* FIXME: I am not at all sure this timing info stuff is right.
             patest_sine_time reports negative latencies, which is wierd.*/
    PaMacCoreStream *stream = (PaMacCoreStream*)s;
    AudioTimeStamp timeStamp;

    VVDBUG(("GetStreamTime()\n"));

    if ( !stream->isTimeSet )
        return (PaTime)0;

    if ( stream->outputDevice )
        AudioDeviceGetCurrentTime( stream->outputDevice, &timeStamp);
    else if ( stream->inputDevice )
        AudioDeviceGetCurrentTime( stream->inputDevice, &timeStamp);
    else
        return (PaTime)0;

    return (PaTime)(timeStamp.mSampleTime - stream->startTime.mSampleTime)/stream->sampleRate;
}

static void setStreamStartTime( PaStream *stream )
{
   /* FIXME: I am not at all sure this timing info stuff is right.
             patest_sine_time reports negative latencies, which is wierd.*/
   PaMacCoreStream *s = (PaMacCoreStream *) stream;
   VVDBUG(("setStreamStartTime()\n"));
   if( s->outputDevice )
      AudioDeviceGetCurrentTime( s->outputDevice, &s->startTime);
   else if( s->inputDevice )
      AudioDeviceGetCurrentTime( s->inputDevice, &s->startTime);
   else
      bzero( &s->startTime, sizeof( s->startTime ) );

   //FIXME: we need a memory barier here

   s->isTimeSet = TRUE;
}


static PaTime TimeStampToSecs(PaMacCoreStream *stream, const AudioTimeStamp* timeStamp)
{
    VVDBUG(("TimeStampToSecs()\n"));
    if (timeStamp->mFlags & kAudioTimeStampSampleTimeValid)
        return (timeStamp->mSampleTime / stream->sampleRate);
    else
        return 0;
}

#define RING_BUFFER_EMPTY (1000)

static OSStatus ringBufferIOProc( AudioConverterRef inAudioConverter, 
                             UInt32*ioDataSize, 
                             void** outData, 
                             void*inUserData )
{
   void *dummyData;
   long dummySize;
   RingBuffer *rb = (RingBuffer *) inUserData;

   VVDBUG(("ringBufferIOProc()\n"));

   assert( sizeof( UInt32 ) == sizeof( long ) );
   if( RingBuffer_GetReadAvailable( rb ) == 0 ) {
      *outData = NULL;
      *ioDataSize = 0;
      return RING_BUFFER_EMPTY;
   }
   RingBuffer_GetReadRegions( rb, *ioDataSize,
                              outData, (long *)ioDataSize, 
                              &dummyData, &dummySize );
      
   assert( *ioDataSize );
   RingBuffer_AdvanceReadIndex( rb, *ioDataSize );

   return noErr;
}

/*
 * Called by the AudioUnit API to process audio from the sound card.
 * This is where the magic happens.
 */
/* FEEDBACK: there is a lot of redundant code here because of how all the cases differ. This makes it hard to maintain, so if there are suggestinos for cleaning it up, I'm all ears. */
static OSStatus AudioIOProc( void *inRefCon,
                               AudioUnitRenderActionFlags *ioActionFlags,
                               const AudioTimeStamp *inTimeStamp,
                               UInt32 inBusNumber,
                               UInt32 inNumberFrames,
                               AudioBufferList *ioData )
{
   unsigned long framesProcessed     = 0;
   PaStreamCallbackTimeInfo timeInfo = {0,0,0};
   PaMacCoreStream *stream           = (PaMacCoreStream*)inRefCon;
   const bool isRender               = inBusNumber == OUTPUT_ELEMENT;
   int callbackResult                = paContinue ;

   VVDBUG(("AudioIOProc()\n"));

   PaUtil_BeginCpuLoadMeasurement( &stream->cpuLoadMeasurer );

   /* -----------------------------------------------------------------*\
      This output may be useful for debugging,
      But printing durring the callback is a bad enough idea that
      this is not enabled by enableing the usual debugging calls.
   \* -----------------------------------------------------------------*/
   /*
   static int renderCount = 0;
   static int inputCount = 0;
   printf( "-------------------  starting reder/input\n" );
   if( isRender )
      printf("Render callback (%d):\t", ++renderCount);
   else
      printf("Input callback  (%d):\t", ++inputCount);
   printf( "Call totals: %d (input), %d (render)\n", inputCount, renderCount );

   printf( "--- inBusNumber: %lu\n", inBusNumber );
   printf( "--- inNumberFrames: %lu\n", inNumberFrames );
   printf( "--- %x ioData\n", (unsigned) ioData );
   if( ioData )
   {
      int i=0;
      printf( "--- ioData.mNumBuffers %lu: \n", ioData->mNumberBuffers );
      for( i=0; i<ioData->mNumberBuffers; ++i )
         printf( "--- ioData buffer %d size: %lu.\n", i, ioData->mBuffers[i].mDataByteSize );
   }
      ----------------------------------------------------------------- */

   if( !stream->isTimeSet )
      setStreamStartTime( stream );

   if( isRender ) {
      AudioTimeStamp currentTime;
      timeInfo.outputBufferDacTime = TimeStampToSecs(stream, inTimeStamp);
      AudioDeviceGetCurrentTime(stream->outputDevice, &currentTime);
      timeInfo.currentTime = TimeStampToSecs(stream, &currentTime);
   }
   if( isRender && stream->inputUnit == stream->outputUnit )
      timeInfo.inputBufferAdcTime = TimeStampToSecs(stream, inTimeStamp);
   if( !isRender ) {
      AudioTimeStamp currentTime;
      timeInfo.inputBufferAdcTime = TimeStampToSecs(stream, inTimeStamp);
      AudioDeviceGetCurrentTime(stream->inputDevice, &currentTime);
      timeInfo.currentTime = TimeStampToSecs(stream, &currentTime);
   }


   if( isRender && stream->inputUnit == stream->outputUnit
                && !stream->inputSRConverter )
   {
      /* --------- Full Duplex, One Device, no SR Conversion -------
       *
       * This is the lowest latency case, and also the simplest.
       * Input data and output data are available at the same time.
       * we do not use the input SR converter or the input ring buffer.
       *
       */
      OSErr err = 0;
      unsigned long frames;

      /* -- start processing -- */
      PaUtil_BeginBufferProcessing( &(stream->bufferProcessor),
                                    &timeInfo,
                                    stream->xrunFlags );
      stream->xrunFlags = 0;

      /* -- compute frames. do some checks -- */
      assert( ioData->mNumberBuffers == 1 );
      assert( ioData->mBuffers[0].mNumberChannels == stream->userOutChan );
      frames = ioData->mBuffers[0].mDataByteSize;
      frames /= sizeof( float ) * ioData->mBuffers[0].mNumberChannels;
      /* -- copy and process input data -- */
      err= AudioUnitRender(stream->inputUnit,
                    ioActionFlags,
                    inTimeStamp,
                    INPUT_ELEMENT,
                    inNumberFrames,
                    &stream->inputAudioBufferList );
      /* FEEDBACK: I'm not sure what to do when this call fails */
      assert( !err );

      PaUtil_SetInputFrameCount( &(stream->bufferProcessor), frames );
      PaUtil_SetInterleavedInputChannels( &(stream->bufferProcessor),
                          0,
                          stream->inputAudioBufferList.mBuffers[0].mData,
                          stream->inputAudioBufferList.mBuffers[0].mNumberChannels);
      /* -- Copy and process output data -- */
      PaUtil_SetOutputFrameCount( &(stream->bufferProcessor), frames );
      PaUtil_SetInterleavedOutputChannels( &(stream->bufferProcessor),
                                        0,
                                        ioData->mBuffers[0].mData,
                                        ioData->mBuffers[0].mNumberChannels);
      /* -- complete processing -- */
      framesProcessed =
                 PaUtil_EndBufferProcessing( &(stream->bufferProcessor),
                                             &callbackResult );
   }
   else if( isRender )
   {
      /* -------- Output Side of Full Duplex (Separate Devices or SR Conversion)
       *       -- OR Simplex Output
       *
       * This case handles output data as in the full duplex case,
       * and, if there is input data, reads it off the ring buffer 
       * and into the PA buffer processor. If sample rate conversion
       * is required on input, that is done here as well.
       */
      unsigned long frames;

      /* Sometimes, when stopping a duplex stream we get erroneous
         xrun flags, so if this is our last run, clear the flags. */
      int xrunFlags = stream->xrunFlags;
/*
      if( xrunFlags & paInputUnderflow )
         printf( "input underflow.\n" );
      if( xrunFlags & paInputOverflow )
         printf( "input overflow.\n" );
*/
      if( stream->state == STOPPING || stream->state == CALLBACK_STOPPED )
         xrunFlags = 0;

      /* -- start processing -- */
      PaUtil_BeginBufferProcessing( &(stream->bufferProcessor),
                                    &timeInfo,
                                    xrunFlags );
      stream->xrunFlags = 0; /* FEEDBACK: we only send flags to Buf Proc once */

      /* -- Copy and process output data -- */
      assert( ioData->mNumberBuffers == 1 );
      frames = ioData->mBuffers[0].mDataByteSize;
      frames /= sizeof( float ) * ioData->mBuffers[0].mNumberChannels;
      assert( ioData->mBuffers[0].mNumberChannels == stream->userOutChan );
      PaUtil_SetOutputFrameCount( &(stream->bufferProcessor), frames );
      PaUtil_SetInterleavedOutputChannels( &(stream->bufferProcessor),
                                     0,
                                     ioData->mBuffers[0].mData,
                                     ioData->mBuffers[0].mNumberChannels);

      /* -- copy and process input data, and complete processing -- */
      if( stream->inputUnit ) {
         const int flsz = sizeof( float );
         /* Here, we read the data out of the ring buffer, through the
            audio converter. */
         int inChan = stream->inputAudioBufferList.mBuffers[0].mNumberChannels;
         if( stream->inputSRConverter )
         {
               OSStatus err;
               UInt32 size;
               float data[ inChan * frames ];
               size = sizeof( data );
               err = AudioConverterFillBuffer( 
                             stream->inputSRConverter,
                             ringBufferIOProc,
                             &stream->inputRingBuffer,
                             &size,
                             (void *)&data );
               if( err == RING_BUFFER_EMPTY )
               { /*the ring buffer callback underflowed */
                  err = 0;
                  bzero( ((char *)data) + size, sizeof(data)-size );
                  stream->xrunFlags |= paInputUnderflow;
               }
               ERR( err );
               assert( !err );
               
               PaUtil_SetInputFrameCount( &(stream->bufferProcessor), frames );
               PaUtil_SetInterleavedInputChannels( &(stream->bufferProcessor),
                                   0,
                                   data,
                                   inChan );
               framesProcessed =
                    PaUtil_EndBufferProcessing( &(stream->bufferProcessor),
                                                &callbackResult );
         }
         else
         {
            /* Without the AudioConverter is actually a bit more complex
               because we have to do a little buffer processing that the
               AudioConverter would otherwise handle for us. */
            void *data1, *data2;
            long size1, size2;
            RingBuffer_GetReadRegions( &stream->inputRingBuffer,
                                       inChan*frames*flsz,
                                       &data1, &size1,
                                       &data2, &size2 );
            if( size1 / ( flsz * inChan ) == frames ) {
               /* simplest case: all in first buffer */
               PaUtil_SetInputFrameCount( &(stream->bufferProcessor), frames );
               PaUtil_SetInterleavedInputChannels( &(stream->bufferProcessor),
                                   0,
                                   data1,
                                   inChan );
               framesProcessed =
                    PaUtil_EndBufferProcessing( &(stream->bufferProcessor),
                                                &callbackResult );
               RingBuffer_AdvanceReadIndex(&stream->inputRingBuffer, size1 );
            } else if( ( size1 + size2 ) / ( flsz * inChan ) < frames ) {
               /*we underflowed. take what data we can, zero the rest.*/
               float data[frames*inChan];
               if( size1 )
                  memcpy( data, data1, size1 );
               if( size2 )
                  memcpy( data+size1, data2, size2 );
               bzero( data+size1+size2, frames*flsz*inChan - size1 - size2 );

               PaUtil_SetInputFrameCount( &(stream->bufferProcessor), frames );
               PaUtil_SetInterleavedInputChannels( &(stream->bufferProcessor),
                                   0,
                                   data,
                                   inChan );
               framesProcessed =
                    PaUtil_EndBufferProcessing( &(stream->bufferProcessor),
                                                &callbackResult );
               RingBuffer_AdvanceReadIndex( &stream->inputRingBuffer,
                                            size1+size2 );
               /* flag underflow */
               stream->xrunFlags |= paInputUnderflow;
            } else {
               /*we got all the data, but split between buffers*/
               PaUtil_SetInputFrameCount( &(stream->bufferProcessor),
                                          size1 / ( flsz * inChan ) );
               PaUtil_SetInterleavedInputChannels( &(stream->bufferProcessor),
                                   0,
                                   data1,
                                   inChan );
               PaUtil_Set2ndInputFrameCount( &(stream->bufferProcessor),
                                             size2 / ( flsz * inChan ) );
               PaUtil_Set2ndInterleavedInputChannels( &(stream->bufferProcessor),
                                   0,
                                   data2,
                                   inChan );
               framesProcessed =
                    PaUtil_EndBufferProcessing( &(stream->bufferProcessor),
                                                &callbackResult );
               RingBuffer_AdvanceReadIndex(&stream->inputRingBuffer, size1+size2 );
            }
         }
      } else {
         framesProcessed =
                 PaUtil_EndBufferProcessing( &(stream->bufferProcessor),
                                             &callbackResult );
      }

   }
   else
   {
      /* ------------------ Input
       *
       * First, we read off the audio data and put it in the ring buffer.
       * if this is an input-only stream, we need to process it more,
       * otherwise, we let the output case deal with it.
       */
      OSErr err = 0;
      int chan = stream->inputAudioBufferList.mBuffers[0].mNumberChannels ;
      /* FIXME: looping here may not actually be necessary, but it was something I tried in testing. */
      do {
         err= AudioUnitRender(stream->inputUnit,
                 ioActionFlags,
                 inTimeStamp,
                 INPUT_ELEMENT,
                 inNumberFrames,
                 &stream->inputAudioBufferList );
         if( err == -10874 )
            inNumberFrames /= 2;
      } while( err == -10874 && inNumberFrames > 1 );
      /* FEEDBACK: I'm not sure what to do when this call fails */
      ERR( err );
      assert( !err );
      if( stream->inputSRConverter || stream->outputUnit )
      {
         /* If this is duplex or we use a converter, put the data
            into the ring buffer. */
         long bytesIn, bytesOut;
         bytesIn = sizeof( float ) * inNumberFrames * chan;
         bytesOut = RingBuffer_Write( &stream->inputRingBuffer,
                                stream->inputAudioBufferList.mBuffers[0].mData,
                                bytesIn );
         if( bytesIn != bytesOut )
            stream->xrunFlags |= paInputOverflow ;
      }
      else
      {
         /* for simplex input w/o SR conversion,
            just pop the data into the buffer processor.*/
         PaUtil_BeginBufferProcessing( &(stream->bufferProcessor),
                              &timeInfo,
                              stream->xrunFlags );
         stream->xrunFlags = 0;

         PaUtil_SetInputFrameCount( &(stream->bufferProcessor), inNumberFrames);
         PaUtil_SetInterleavedInputChannels( &(stream->bufferProcessor),
                             0,
                             stream->inputAudioBufferList.mBuffers[0].mData,
                             chan );
         framesProcessed =
              PaUtil_EndBufferProcessing( &(stream->bufferProcessor),
                                          &callbackResult );
      }
      if( !stream->outputUnit && stream->inputSRConverter )
      {
         /* ------------------ Simplex Input w/ SR Conversion
          *
          * if this is a simplex input stream, we need to read off the buffer,
          * do our sample rate conversion and pass the results to the buffer
          * processor.
          * The logic here is complicated somewhat by the fact that we don't
          * know how much data is available, so we loop on reasonably sized
          * chunks, and let the BufferProcessor deal with the rest.
          *
          */
         /*This might be too big or small depending on SR conversion*/
         float data[ chan * inNumberFrames ];
         OSStatus err;
         do
         { /*Run the buffer processor until we are out of data*/
            UInt32 size;
            long f;

            size = sizeof( data );
            err = AudioConverterFillBuffer( 
                          stream->inputSRConverter,
                          ringBufferIOProc,
                          &stream->inputRingBuffer,
                          &size,
                          (void *)data );
            if( err != RING_BUFFER_EMPTY )
               ERR( err );
            assert( err == 0 || err == RING_BUFFER_EMPTY );

            f = size / ( chan * sizeof(float) );
            PaUtil_SetInputFrameCount( &(stream->bufferProcessor), f );
            if( f )
            {
               PaUtil_BeginBufferProcessing( &(stream->bufferProcessor),
                                             &timeInfo,
                                             stream->xrunFlags );
               stream->xrunFlags = 0;

               PaUtil_SetInterleavedInputChannels( &(stream->bufferProcessor),
                                0,
                                data,
                                chan );
               framesProcessed =
                    PaUtil_EndBufferProcessing( &(stream->bufferProcessor),
                                                &callbackResult );
            }
         } while( callbackResult == paContinue && !err );
      }
   }

   switch( callbackResult )
   {
   case paContinue: break;
   case paComplete:
   case paAbort:
      stream->isTimeSet = FALSE;
      stream->state = CALLBACK_STOPPED ;
      if( stream->outputUnit )
         AudioOutputUnitStop(stream->outputUnit);
      if( stream->inputUnit )
         AudioOutputUnitStop(stream->inputUnit);
      break;
   }

   PaUtil_EndCpuLoadMeasurement( &stream->cpuLoadMeasurer, framesProcessed );
   return noErr;
}


/*
    When CloseStream() is called, the multi-api layer ensures that
    the stream has already been stopped or aborted.
*/
static PaError CloseStream( PaStream* s )
{
    /* This may be called from a failed OpenStream.
       Therefore, each piece of info is treated seperately. */
    PaError result = paNoError;
    PaMacCoreStream *stream = (PaMacCoreStream*)s;

    VVDBUG(("CloseStream()\n"));
    VDBUG( ( "Closing stream.\n" ) );

    if( stream ) {
       if( stream->outputUnit && stream->outputUnit != stream->inputUnit ) {
          AudioUnitUninitialize( stream->outputUnit );
          CloseComponent( stream->outputUnit );
       }
       stream->outputUnit = NULL;
       if( stream->inputUnit )
       {
          AudioUnitUninitialize( stream->inputUnit );
          CloseComponent( stream->inputUnit );
          stream->inputUnit = NULL;
       }
       if( stream->inputRingBuffer.buffer )
          free( (void *) stream->inputRingBuffer.buffer );
       stream->inputRingBuffer.buffer = NULL;
       /*TODO: is there more that needs to be done on error
               from AudioConverterDispose?*/
       if( stream->inputSRConverter )
          ERR( AudioConverterDispose( stream->inputSRConverter ) );
       stream->inputSRConverter = NULL;
       if( stream->inputAudioBufferList.mBuffers[0].mData )
          free( stream->inputAudioBufferList.mBuffers[0].mData );
       stream->inputAudioBufferList.mBuffers[0].mData = NULL;

       result = destroyBlioRingBuffers( &stream->blio );
       if( result )
          return result;
       if( stream->bufferProcessorIsInitialized )
          PaUtil_TerminateBufferProcessor( &stream->bufferProcessor );
       PaUtil_TerminateStreamRepresentation( &stream->streamRepresentation );
       PaUtil_FreeMemory( stream );
    }

    return result;
}


static PaError StartStream( PaStream *s )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;
    OSErr result = noErr;
    VVDBUG(("StartStream()\n"));
    VDBUG( ( "Starting stream.\n" ) );

#define ERR_WRAP(mac_err) do { result = mac_err ; if ( result != noErr ) return ERR(result) ; } while(0)

    /*FIXME: maybe want to do this on close/abort for faster start? */
    PaUtil_ResetBufferProcessor( &stream->bufferProcessor );
    if(  stream->inputSRConverter )
       ERR_WRAP( AudioConverterReset( stream->inputSRConverter ) );

    /* -- start -- */
    stream->state = ACTIVE;
    if( stream->inputUnit ) {
       ERR_WRAP( AudioOutputUnitStart(stream->inputUnit) );
    }
    if( stream->outputUnit && stream->outputUnit != stream->inputUnit ) {
       ERR_WRAP( AudioOutputUnitStart(stream->outputUnit) );
    }

    //setStreamStartTime( stream );
    //stream->isTimeSet = TRUE;

    return paNoError;
#undef ERR_WRAP
}


static PaError StopStream( PaStream *s )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;
    OSErr result = noErr;
    PaError paErr;
    VVDBUG(("StopStream()\n"));

    VDBUG( ("Waiting for BLIO.\n") );
    waitUntilBlioWriteBufferIsFlushed( &stream->blio );
    VDBUG( ( "Stopping stream.\n" ) );

    stream->isTimeSet = FALSE;
    stream->state = STOPPING;

#define ERR_WRAP(mac_err) do { result = mac_err ; if ( result != noErr ) return ERR(result) ; } while(0)
    /* -- stop and reset -- */
    if( stream->inputUnit == stream->outputUnit && stream->inputUnit )
    {
       ERR_WRAP( AudioOutputUnitStop(stream->inputUnit) );
       ERR_WRAP( AudioUnitReset(stream->inputUnit, kAudioUnitScope_Global, 1) );
       ERR_WRAP( AudioUnitReset(stream->inputUnit, kAudioUnitScope_Global, 0) );
    }
    else
    {
       if( stream->inputUnit )
       {
          ERR_WRAP(AudioOutputUnitStop(stream->inputUnit) );
          ERR_WRAP(AudioUnitReset(stream->inputUnit,kAudioUnitScope_Global,1));
       }
       if( stream->outputUnit )
       {
          ERR_WRAP(AudioOutputUnitStop(stream->outputUnit));
          ERR_WRAP(AudioUnitReset(stream->outputUnit,kAudioUnitScope_Global,0));
       }
    }
    if( stream->inputRingBuffer.buffer ) {
       RingBuffer_Flush( &stream->inputRingBuffer );
       bzero( (void *)stream->inputRingBuffer.buffer,
              stream->inputRingBuffer.bufferSize );
       /* advance the write point a little, so we are reading from the
          middle of the buffer. We'll need extra at the end because
          testing has shown that this helps. */
       if( stream->outputUnit )
          RingBuffer_AdvanceWriteIndex( &stream->inputRingBuffer,
                                    stream->inputRingBuffer.bufferSize
                                           / RING_BUFFER_ADVANCE_DENOMINATOR );
    }

    stream->xrunFlags = 0;
    stream->state = STOPPED;

    paErr = resetBlioRingBuffers( &stream->blio );
    if( paErr )
       return paErr;

/*
    //stream->isTimeSet = FALSE;
*/

    VDBUG( ( "Stream Stopped.\n" ) );
    return paNoError;
#undef ERR_WRAP
}

static PaError AbortStream( PaStream *s )
{
    VVDBUG(("AbortStream()->StopStream()\n"));
    VDBUG( ( "Aborting stream.\n" ) );
    /* We have nothing faster than StopStream. */
    return StopStream(s);
}


static PaError IsStreamStopped( PaStream *s )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;
    VVDBUG(("IsStreamStopped()\n"));

    return stream->state == STOPPED ? 1 : 0;
}


static PaError IsStreamActive( PaStream *s )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;
    VVDBUG(("IsStreamActive()\n"));
    return ( stream->state == ACTIVE || stream->state == STOPPING );
}


static double GetStreamCpuLoad( PaStream* s )
{
    PaMacCoreStream *stream = (PaMacCoreStream*)s;
    VVDBUG(("GetStreamCpuLoad()\n"));

    return PaUtil_GetCpuLoad( &stream->cpuLoadMeasurer );
}
