/** @file patest_wire.c
	@brief Pass input directly to output.

	Note that some HW devices, for example many ISA audio cards
	on PCs, do NOT support full duplex! For a PC, you normally need
	a PCI based audio card such as the SBLive.

	@author Phil Burk  http://www.softsynth.com
	@todo needs to be updated to use the V19 API
*/
/*
 * $Id$
 *
 * This program uses the PortAudio Portable Audio Library.
 * For more information see: http://www.portaudio.com
 * Copyright (c) 1999-2000 Ross Bencina and Phil Burk
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
 *
 */
#include <stdio.h>
#include <math.h>
#include "portaudio.h"

#define SAMPLE_RATE            (44100)

typedef struct WireConfig_s
{
    int isInputInterleaved;
    int isOutputInterleaved;
    int numInputChannels;
    int numOutputChannels;
    int framesPerCallback;
} WireConfig_t;

#define USE_FLOAT_INPUT        (1)
#define USE_FLOAT_OUTPUT       (1)

#define INPUT_LATENCY_MSEC     (0)
#define INPUT_LATENCY_FRAMES   ((INPUT_LATENCY_MSEC * SAMPLE_RATE) / 1000)
#define OUTPUT_LATENCY_MSEC    (0)
#define OUTPUT_LATENCY_FRAMES  ((OUTPUT_LATENCY_MSEC * SAMPLE_RATE) / 1000)


#if USE_FLOAT_INPUT
    #define INPUT_FORMAT  paFloat32
    typedef float INPUT_SAMPLE;
#else
    #define INPUT_FORMAT  paInt16
    typedef short INPUT_SAMPLE;
#endif

#if USE_FLOAT_OUTPUT
    #define OUTPUT_FORMAT  paFloat32
    typedef float OUTPUT_SAMPLE;
#else
    #define OUTPUT_FORMAT  paInt16
    typedef short OUTPUT_SAMPLE;
#endif

double gInOutScaler = 1.0;
#define CONVERT_IN_TO_OUT(in)  ((OUTPUT_SAMPLE) ((in) * gInOutScaler))

#define INPUT_DEVICE           (Pa_GetDefaultInputDevice())
#define OUTPUT_DEVICE          (Pa_GetDefaultOutputDevice())

static PaError TestConfiguration( WireConfig_t *config );

static int wireCallback( void *inputBuffer, void *outputBuffer,
                         unsigned long framesPerBuffer,
                         PaTimestamp outTime, void *userData );

/* This routine will be called by the PortAudio engine when audio is needed.
** It may be called at interrupt level on some machines so don't do anything
** that could mess up the system like calling malloc() or free().
*/

static int wireCallback( void *inputBuffer, void *outputBuffer,
                         unsigned long framesPerBuffer,
                         PaTimestamp outTime, void *userData )
{
    INPUT_SAMPLE *in;
    OUTPUT_SAMPLE *out;
    int inStride;
    int outStride;

    int inDone = 0;
    int outDone = 0;
    WireConfig_t *config = (WireConfig_t *) userData;
    unsigned int i;
    int inChannel, outChannel;
    (void) outTime;

    /* This may get called with NULL inputBuffer during initial setup. */
    if( inputBuffer == NULL) return 0;

    inChannel=0, outChannel=0;
    while( !(inDone && outDone) )
    {
        if( config->isInputInterleaved )
        {
            in = ((INPUT_SAMPLE*)inputBuffer) + inChannel;
            inStride = config->numInputChannels;
        }
        else
        {
            in = ((INPUT_SAMPLE**)inputBuffer)[inChannel];
            inStride = 1;
        }

        if( config->isOutputInterleaved )
        {
            out = ((OUTPUT_SAMPLE*)outputBuffer) + outChannel;
            outStride = config->numOutputChannels;
        }
        else
        {
            out = ((OUTPUT_SAMPLE**)outputBuffer)[outChannel];
            outStride = 1;
        }

        for( i=0; i<framesPerBuffer; i++ )
        {
            *out = CONVERT_IN_TO_OUT( *in );
            out += outStride;
            in += inStride;
        }

        if(inChannel < (config->numInputChannels - 1)) inChannel++;
        else inDone = 1;
        if(outChannel < (config->numOutputChannels - 1)) outChannel++;
        else outDone = 1;
    }
    return 0;
}

/*******************************************************************/
int main(void);
int main(void)
{
    PaError err;
    WireConfig_t CONFIG;
    WireConfig_t *config = &CONFIG;
    int configIndex = 0;;

    err = Pa_Initialize();
    if( err != paNoError ) goto error;

    printf("Please connect audio signal to input and listen for it on output!\n");
    printf("input format = %d\n", INPUT_FORMAT );
    printf("output format = %d\n", OUTPUT_FORMAT );
    printf("input device ID  = %d\n", INPUT_DEVICE );
    printf("output device ID = %d\n", OUTPUT_DEVICE );

    if( INPUT_FORMAT == OUTPUT_FORMAT )
    {
        gInOutScaler = 1.0;
    }
    else if( (INPUT_FORMAT == paInt16) && (OUTPUT_FORMAT == paFloat32) )
    {
        gInOutScaler = 1.0/32768.0;
    }
    else if( (INPUT_FORMAT == paFloat32) && (OUTPUT_FORMAT == paInt16) )
    {
        gInOutScaler = 32768.0;
    }

    for( config->isInputInterleaved = 0; config->isInputInterleaved < 2; config->isInputInterleaved++ )
    {
        for( config->isOutputInterleaved = 0; config->isOutputInterleaved < 2; config->isOutputInterleaved++ )
        {
            for( config->numInputChannels = 1; config->numInputChannels < 3; config->numInputChannels++ )
            {
                for( config->numOutputChannels = 1; config->numOutputChannels < 3; config->numOutputChannels++ )
                {
                    for( config->framesPerCallback = 0; config->framesPerCallback < 65; config->framesPerCallback += 64 )
                    {
                        printf("-----------------------------------------------\n" );
                        printf("Configuration #%d\n", configIndex++ );
                        err = TestConfiguration( config );
                    // give user a chance to bail out
                        if( err == 1 )
                        {
                            err = paNoError;
                            goto done;
                        }
                        else if( err != paNoError ) goto error;
                    }
               }
            }
        }
    }

done:
    Pa_Terminate();
    
    printf("Full duplex sound test complete.\n"); fflush(stdout);
    
    printf("Hit ENTER to quit.\n");  fflush(stdout);
    getchar();

    return 0;

error:
    Pa_Terminate();
    fprintf( stderr, "An error occured while using the portaudio stream\n" );
    fprintf( stderr, "Error number: %d\n", err );
    fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
    printf("Hit ENTER to quit.\n");  fflush(stdout);
    getchar();
    return -1;
}

static PaError TestConfiguration( WireConfig_t *config )
{
    int c;
    PaError err;
    PaStream *stream;
    printf("input %sinterleaved!\n", (config->isInputInterleaved ? " " : "NOT ") );
    printf("output %sinterleaved!\n", (config->isOutputInterleaved ? " " : "NOT ") );
    printf("input channels = %d\n", config->numInputChannels );
    printf("output channels = %d\n", config->numOutputChannels );
    printf("framesPerCallback = %d\n", config->framesPerCallback );

    err = Pa_OpenStream(
              &stream,
              INPUT_DEVICE,
              config->numInputChannels,
              INPUT_FORMAT | (config->isInputInterleaved ? 0 : paNonInterleaved),
              INPUT_LATENCY_FRAMES,    /* input latency */
              NULL,
              OUTPUT_DEVICE,
              config->numOutputChannels,
              OUTPUT_FORMAT | (config->isOutputInterleaved ? 0 : paNonInterleaved),
              OUTPUT_LATENCY_FRAMES,   /* output latency */
              NULL,
              SAMPLE_RATE,
              config->framesPerCallback,            /* frames per buffer */
              paClipOff,       /* we won't output out of range samples so don't bother clipping them */
              wireCallback,
              config );          /* user data */
    if( err != paNoError ) goto error;
    
    err = Pa_StartStream( stream );
    if( err != paNoError ) goto error;
    
    printf("Hit ENTER for next configuration, or 'q' to quit.\n");  fflush(stdout);
    c = getchar();
    
    printf("Closing stream.\n");
    err = Pa_CloseStream( stream );
    if( err != paNoError ) goto error;

    if( c == 'q' ) return 1;

error:
    return err;
}
