/*
* XBMC Media Center
* Copyright (c) 2002 d7o3g4q and RUNTiME
* Portions Copyright (c) by the authors of ffmpeg and xvid
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#if (defined HAVE_CONFIG_H) && (!defined WIN32)
  #include "config.h"
#elif defined(_WIN32)
#include "system.h"
#endif

#include "OMXAudio.h"
#include "utils/log.h"

#define CLASSNAME "COMXAudio"

#include "linux/XMemUtils.h"

#ifndef STANDALONE
#include "guilib/AudioContext.h"
#include "settings/AdvancedSettings.h"
#include "settings/GUISettings.h"
#include "settings/Settings.h"
#include "guilib/LocalizeStrings.h"
#endif

#ifndef VOLUME_MINIMUM
#define VOLUME_MINIMUM -6000  // -60dB
#endif

using namespace std;

static enum PCMChannels OMXChannelMap[8] =
{
  PCM_FRONT_LEFT  , PCM_FRONT_RIGHT  ,
  PCM_BACK_LEFT   , PCM_BACK_RIGHT   ,
  PCM_FRONT_CENTER, PCM_LOW_FREQUENCY,
  PCM_SIDE_LEFT   , PCM_SIDE_RIGHT
};

static enum OMX_AUDIO_CHANNELTYPE OMXChannels[8] =
{
  OMX_AUDIO_ChannelLF, OMX_AUDIO_ChannelRF ,
  OMX_AUDIO_ChannelLR, OMX_AUDIO_ChannelRR ,
  OMX_AUDIO_ChannelCF, OMX_AUDIO_ChannelLFE,
  OMX_AUDIO_ChannelLS, OMX_AUDIO_ChannelRS
};

static unsigned int WAVEChannels[8] =
{
  SPEAKER_FRONT_LEFT,       SPEAKER_FRONT_RIGHT,
  SPEAKER_BACK_LEFT,        SPEAKER_BACK_RIGHT,
  SPEAKER_TOP_FRONT_CENTER, SPEAKER_LOW_FREQUENCY,
  SPEAKER_SIDE_LEFT,        SPEAKER_SIDE_RIGHT
};

static const uint16_t DTSFSCod   [] = {0, 8000, 16000, 32000, 0, 0, 11025, 22050, 44100, 0, 0, 12000, 24000, 48000, 0, 0};

//OMX_API OMX_ERRORTYPE OMX_APIENTRY vc_OMX_Init(void);

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
//***********************************************************************************************
COMXAudio::COMXAudio() :
  m_Initialized     (false  ),
  m_Pause           (false  ),
  m_CanPause        (false  ),
  m_CurrentVolume   (0      ),
  m_Passthrough     (false  ),
  m_BytesPerSec     (0      ),
  m_BufferLen       (0      ),
  m_ChunkLen        (0      ),
  m_DataChannels    (0      ),
  m_Channels        (0      ),
  m_BitsPerSample   (0      ),
  m_omx_clock       (NULL   ),
  m_av_clock        (NULL   ),
  m_external_clock  (false  ),
  m_setStartTime    (false  ),
  m_SampleSize      (0      ),
  m_firstFrame      (true   ),
  m_LostSync        (true   ),
  m_SampleRate      (0      ),
  m_eEncoding       (OMX_AUDIO_CodingPCM),
  m_extradata       (NULL   ),
  m_extrasize       (0      )
{
}

COMXAudio::~COMXAudio()
{
  if(m_Initialized)
    Deinitialize();
}


bool COMXAudio::Initialize(IAudioCallback* pCallback, const CStdString& device, enum PCMChannels *channelMap,
                           COMXStreamInfo &hints, OMXClock *clock, bool bResample, bool bIsMusic, bool bPassthrough)
{
  if(bPassthrough)
    SetCodingType(hints.codec);
  else
    SetCodingType(CODEC_ID_PCM_S16LE);

  SetClock(clock);

  if(hints.extrasize > 0 && hints.extradata != NULL)
  {
    m_extrasize = hints.extrasize;
    m_extradata = (uint8_t *)malloc(m_extrasize);
    memcpy(m_extradata, hints.extradata, hints.extrasize);
  }

  return Initialize(pCallback, device, hints.channels, channelMap, hints.samplerate, hints.bitspersample, false, false, bPassthrough);
}

bool COMXAudio::Initialize(IAudioCallback* pCallback, const CStdString& device, int iChannels, enum PCMChannels *channelMap, unsigned int uiSamplesPerSec, unsigned int uiBitsPerSample, bool bResample, bool bIsMusic, bool bPassthrough)
{
  CStdString deviceuse;
  if(device == "hdmi") {
    deviceuse = "hdmi";
  } else {
    deviceuse = "local";
  }

  m_Passthrough = bPassthrough;
  m_drc         = 0;

  memset(&m_wave_header, 0x0, sizeof(m_wave_header));

#ifndef STANDALONE
  bool bAudioOnAllSpeakers(false);
  g_audioContext.SetupSpeakerConfig(iChannels, bAudioOnAllSpeakers, bIsMusic);

  if(bPassthrough)
  {
    g_audioContext.SetActiveDevice(CAudioContext::DIRECTSOUND_DEVICE_DIGITAL);
  } else {
    g_audioContext.SetActiveDevice(CAudioContext::DIRECTSOUND_DEVICE);
  }

  m_CurrentVolume = g_settings.m_nVolumeLevel; 
#else
  m_CurrentVolume = 0;
#endif

  m_DataChannels = iChannels;
  m_remap.Reset();

  OMX_INIT_STRUCTURE(m_pcm);
  m_Channels = 2;
  m_pcm.nChannels = m_Channels;
  m_pcm.eChannelMapping[0] = OMX_AUDIO_ChannelLF;
  m_pcm.eChannelMapping[1] = OMX_AUDIO_ChannelRF;
  m_pcm.eChannelMapping[2] = OMX_AUDIO_ChannelMax;

  m_wave_header.Format.nChannels  = m_Channels;
  m_wave_header.dwChannelMask     = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

  if (!m_Passthrough && channelMap)
  {
    enum PCMChannels *outLayout;

    // set the input format, and get the channel layout so we know what we need to open
    outLayout = m_remap.SetInputFormat (iChannels, channelMap, uiBitsPerSample / 8, uiSamplesPerSec);

    unsigned int outChannels = 0;
    unsigned int ch = 0, map;
    unsigned int chan = 0;
    while(outLayout[ch] != PCM_INVALID)
    {
      for(map = 0; map < 8; ++map)
      {
        if (outLayout[ch] == OMXChannelMap[map])
        {
          m_pcm.eChannelMapping[chan] = OMXChannels[map]; 
          m_pcm.eChannelMapping[chan + 1] = OMX_AUDIO_ChannelMax;
          m_wave_header.dwChannelMask |= WAVEChannels[map];
          chan++;
          if (map > outChannels)
            outChannels = map;
          break;
        }
      }
      ++ch;
    }

    m_remap.SetOutputFormat(++outChannels, OMXChannelMap);
    if (m_remap.CanRemap())
    {
      iChannels = outChannels;
      if (m_DataChannels != (unsigned int)iChannels)
        CLog::Log(LOGDEBUG, "COMXAudio:::Initialize Requested channels changed from %i to %i", m_DataChannels, iChannels);
    }

  }

  m_Channels = iChannels;

  // set the m_pcm parameters
  m_pcm.eNumData            = OMX_NumericalDataSigned;
  m_pcm.eEndian             = OMX_EndianLittle;
  m_pcm.bInterleaved        = OMX_TRUE;
  m_pcm.nBitPerSample       = uiBitsPerSample;
  m_pcm.ePCMMode            = OMX_AUDIO_PCMModeLinear;
  m_pcm.nChannels           = m_Channels;
  m_pcm.nSamplingRate       = uiSamplesPerSec;

  m_SampleRate              = uiSamplesPerSec;

  m_wave_header.Samples.wValidBitsPerSample = uiBitsPerSample;
  m_wave_header.Samples.wSamplesPerBlock    = 0;
  m_wave_header.Format.nChannels            = m_Channels;
  m_wave_header.Format.nBlockAlign          = m_Channels * (uiBitsPerSample >> 3);
  m_wave_header.Format.wFormatTag           = WAVE_FORMAT_PCM;
  m_wave_header.Format.nSamplesPerSec       = uiSamplesPerSec;
  m_wave_header.Format.nAvgBytesPerSec      = m_BytesPerSec;
  m_wave_header.Format.wBitsPerSample       = uiBitsPerSample;
  m_wave_header.Format.cbSize               = 0;
  m_wave_header.SubFormat                   = KSDATAFORMAT_SUBTYPE_PCM;

  m_SampleSize              = (m_pcm.nChannels * m_pcm.nBitPerSample * m_pcm.nSamplingRate)>>3;

  PrintPCM(&m_pcm);

  /* no external clock. we got initialized outside omxplayer */
  if(m_av_clock == NULL)
    m_OMX.Initialize();

  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  CStdString componentName = "";

  componentName = "OMX.broadcom.audio_render";
  if(!m_omx_render.Initialize((const CStdString)componentName, OMX_IndexParamAudioInit))
    return false;

  OMX_CONFIG_BRCMAUDIODESTINATIONTYPE audioDest;
  OMX_INIT_STRUCTURE(audioDest);
  strncpy((char *)audioDest.sName, device.c_str(), strlen(device.c_str()));

  omx_err = m_omx_render.SetConfig(OMX_IndexConfigBrcmAudioDestination, &audioDest);
  if (omx_err != OMX_ErrorNone)
    return false;

  componentName = "OMX.broadcom.audio_decode";
  if(!m_omx_decoder.Initialize((const CStdString)componentName, OMX_IndexParamAudioInit))
    return false;

  if(m_Passthrough /*&& m_eEncoding != OMX_AUDIO_CodingVORBIS && m_eEncoding != OMX_AUDIO_CodingMP3*/)
  {
    OMX_CONFIG_BOOLEANTYPE boolType;
    OMX_INIT_STRUCTURE(boolType);
    boolType.bEnabled = OMX_TRUE;
    omx_err = m_omx_decoder.SetParameter(OMX_IndexParamBrcmDecoderPassThrough, &boolType);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "COMXAudio::Initialize - Error OMX_IndexParamBrcmDecoderPassThrough 0x%08x", omx_err);
      printf("OMX_IndexParamBrcmDecoderPassThrough omx_err(0x%08x)\n", omx_err);
      return false;
    }
  }

  if(m_av_clock == NULL)
  {
    /* no external clock set. generate one */
    m_external_clock = false;

    m_av_clock = new OMXClock();
    
    if(!m_av_clock->Initialize())
    {
      delete m_av_clock;
      m_av_clock = NULL;
      CLog::Log(LOGERROR, "COMXAudio::Initialize error creating av clock\n");
      return false;
    }
  }

  m_omx_clock = m_av_clock->GetOMXClock();

  m_omx_tunnel_clock.Initialize(m_omx_clock, m_omx_clock->GetInputPort(), &m_omx_render, m_omx_render.GetOutputPort());

  omx_err = m_omx_tunnel_clock.Establish(false);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXAudio::Initialize m_omx_tunnel_clock.Establish\n");
    return false;
  }

  if(!m_external_clock)
  {
    omx_err = m_omx_clock->SetStateForComponent(OMX_StateExecuting);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "COMXAudio::Initialize m_omx_clock.SetStateForComponent\n");
      return false;
    }
  }

  m_BitsPerSample = uiBitsPerSample;
  m_BufferLen     = m_BytesPerSec = uiSamplesPerSec * (uiBitsPerSample >> 3) * iChannels;
  m_BufferLen     *= AUDIO_BUFFER_SECONDS;
  m_ChunkLen      = 6144;
  //m_ChunkLen      = 2048;

  // set up the number/size of buffers
  OMX_PARAM_PORTDEFINITIONTYPE port_param;
  OMX_INIT_STRUCTURE(port_param);
  port_param.nPortIndex = m_omx_decoder.GetInputPort();

  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_param);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXAudio::Initialize error OMX_IndexParamPortDefinition omx_err(0x%08x)\n", omx_err);
    return false;
  }

  port_param.format.audio.eEncoding = m_eEncoding;

  /*
  m_ChunkLen    = port_param.nBufferSize;
  m_BufferLen   = port_param.nBufferCountActual * m_ChunkLen;
  */
  port_param.nBufferSize = m_ChunkLen;
  port_param.nBufferCountActual = m_BufferLen / m_ChunkLen;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamPortDefinition, &port_param);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXAudio::Initialize error OMX_IndexParamPortDefinition omx_err(0x%08x)\n", omx_err);
    return false;
  }

  /*
  OMX_AUDIO_PARAM_PORTFORMATTYPE formatType;
  OMX_INIT_STRUCTURE(formatType);
  formatType.nPortIndex = m_omx_render.GetInputPort();

  formatType.eEncoding = m_eEncoding;

  omx_err = m_omx_render.SetParameter(OMX_IndexParamAudioPortFormat, &formatType);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXAudio::Initialize error OMX_IndexParamAudioPortFormat omx_err(0x%08x)\n", omx_err);
    return false;
  }
 
  m_pcm.nPortIndex          = m_omx_render.GetInputPort();
  omx_err = m_omx_render.SetParameter(OMX_IndexParamAudioPcm, &m_pcm);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXAudio::Initialize OMX_IndexParamAudioPcm omx_err(0x%08x)\n", omx_err);
    return false;
  }
  */

  omx_err = m_omx_decoder.AllocInputBuffers();
  if(omx_err != OMX_ErrorNone) 
  {
    CLog::Log(LOGERROR, "COMXAudio::Initialize - Error alloc buffers 0x%08x", omx_err);
    return false;
  }

  m_omx_tunnel_decoder.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_render, m_omx_render.GetInputPort());
  omx_err = m_omx_tunnel_decoder.Establish(false);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXAudio::Initialize - Error m_omx_tunnel_decoder.Establish 0x%08x", omx_err);
    return false;
  }

  omx_err = m_omx_decoder.SetStateForComponent(OMX_StateExecuting);
  if(omx_err != OMX_ErrorNone) {
    CLog::Log(LOGERROR, "COMXAudio::Initialize - Error setting OMX_StateExecuting 0x%08x", omx_err);
    return false;
  }

  omx_err = m_omx_render.SetStateForComponent(OMX_StateExecuting);
  if(omx_err != OMX_ErrorNone) 
  {
    CLog::Log(LOGERROR, "COMXAudio::Initialize - Error setting OMX_StateExecuting 0x%08x", omx_err);
    return false;
  }

  if(m_eEncoding == OMX_AUDIO_CodingPCM)
  {
    OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer();
    if(omx_buffer == NULL)
    {
      CLog::Log(LOGERROR, "COMXAudio::Initialize - buffer error 0x%08x", omx_err);
      return false;
    }

    omx_buffer->nOffset = 0;
    omx_buffer->nFilledLen = sizeof(m_wave_header);
    if(omx_buffer->nFilledLen > omx_buffer->nAllocLen)
    {
      CLog::Log(LOGERROR, "COMXAudio::Initialize - omx_buffer->nFilledLen > omx_buffer->nAllocLen");
      return false;
    }
    memset((unsigned char *)omx_buffer->pBuffer, 0x0, omx_buffer->nAllocLen);
    memcpy((unsigned char *)omx_buffer->pBuffer, &m_wave_header, omx_buffer->nFilledLen);
    omx_buffer->nFlags = OMX_BUFFERFLAG_CODECCONFIG | OMX_BUFFERFLAG_ENDOFFRAME;

    omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }
  } 
#if 0
  else if((m_eEncoding == OMX_AUDIO_CodingVORBIS || m_eEncoding == OMX_AUDIO_CodingMP3) && m_Passthrough)
  {
    /* send decoder config */
    if(m_extrasize > 0 && m_extradata != NULL)
    {
      OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer();
  
      if(omx_buffer == NULL)
      {
        CLog::Log(LOGERROR, "%s::%s - buffer error 0x%08x", CLASSNAME, __func__, omx_err);
        return false;
      }
  
      omx_buffer->nOffset = 0;
      omx_buffer->nFilledLen = m_extrasize;
      if(omx_buffer->nFilledLen > omx_buffer->nAllocLen)
      {
        CLog::Log(LOGERROR, "%s::%s - omx_buffer->nFilledLen > omx_buffer->nAllocLen", CLASSNAME, __func__);
        return false;
      }

      memset((unsigned char *)omx_buffer->pBuffer, 0x0, omx_buffer->nAllocLen);
      memcpy((unsigned char *)omx_buffer->pBuffer, m_extradata, omx_buffer->nFilledLen);
      omx_buffer->nFlags = OMX_BUFFERFLAG_CODECCONFIG | OMX_BUFFERFLAG_ENDOFFRAME;
  
      for(int i = 0; i < omx_buffer->nFilledLen; i++)
        printf("0x%02x ", (uint8_t)omx_buffer->pBuffer[i]);
      printf("\n");

      omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
      if (omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
        return false;
      }
    }
  }
#endif

  m_Initialized   = true;
  m_setStartTime  = true;
  m_firstFrame    = true;

  SetCurrentVolume(m_CurrentVolume);

  CLog::Log(LOGDEBUG, "COMXAudio::Initialize bps %d samplerate %d channels %d device %s buffer size %d bytes per second %d passthrough %d", 
      (int)m_pcm.nBitPerSample, (int)m_pcm.nSamplingRate, (int)m_pcm.nChannels, deviceuse.c_str(), m_BufferLen, m_BytesPerSec, m_Passthrough);

  return true;
}

//***********************************************************************************************
bool COMXAudio::Deinitialize()
{
  if(!m_Initialized)
    return true;

  if(!m_external_clock && m_av_clock != NULL)
  {
    m_av_clock->Pause();
  }

  m_omx_tunnel_clock.Deestablish();

  m_omx_tunnel_decoder.Deestablish();

  m_omx_decoder.Deinitialize();

  m_omx_render.Deinitialize();

  m_Initialized = false;
  m_BytesPerSec = 0;
  m_BufferLen   = 0;

  if(!m_external_clock && m_av_clock != NULL)
  {
    delete m_av_clock;
    m_av_clock  = NULL;
    m_external_clock = false;

    /* not initialized in omxplayer */
    m_OMX.Deinitialize();
  }

  m_omx_clock = NULL;
  m_av_clock  = NULL;

  m_Initialized = false;
  m_firstFrame  = false;
  m_LostSync    = true;

  if(m_extradata)
    free(m_extradata);
  m_extradata = NULL;
  m_extrasize = 0;

  return true;
}

void COMXAudio::Flush()
{
  if(!m_Initialized)
    return;

  m_omx_render.FlushInput();
  m_omx_decoder.FlushInput();

  m_omx_tunnel_clock.Flush();
  m_omx_tunnel_decoder.Flush();

  m_setStartTime = true;
}

//***********************************************************************************************
bool COMXAudio::Pause()
{
  if (!m_Initialized)
     return -1;

  if(m_Pause) return true;
  m_Pause = true;

  m_omx_decoder.SetStateForComponent(OMX_StatePause);

  return true;
}

//***********************************************************************************************
bool COMXAudio::Resume()
{
  if (!m_Initialized)
     return -1;

  if(!m_Pause) return true;
  m_Pause = false;

  m_omx_decoder.SetStateForComponent(OMX_StateExecuting);

  return true;
}

//***********************************************************************************************
bool COMXAudio::Stop()
{
  if (!m_Initialized)
     return -1;

  Flush();

  m_Pause = false;

  return true;
}

//***********************************************************************************************
long COMXAudio::GetCurrentVolume() const
{
  return m_CurrentVolume;
}

//***********************************************************************************************
void COMXAudio::Mute(bool bMute)
{
  if(!m_Initialized)
    return;

  if (bMute)
    SetCurrentVolume(VOLUME_MINIMUM);
  else
    SetCurrentVolume(m_CurrentVolume);
}

//***********************************************************************************************
bool COMXAudio::SetCurrentVolume(long nVolume)
{
  if(!m_Initialized || m_Passthrough)
    return -1;

  m_CurrentVolume = nVolume;

  OMX_AUDIO_CONFIG_VOLUMETYPE volume;
  OMX_INIT_STRUCTURE(volume);
  volume.nPortIndex = m_omx_render.GetInputPort();

  volume.sVolume.nValue = nVolume;

  m_omx_render.SetConfig(OMX_IndexConfigAudioVolume, &volume);

  return true;
}


//***********************************************************************************************
unsigned int COMXAudio::GetSpace()
{
  int free = m_omx_decoder.GetInputBufferSpace();
  return (free / m_Channels) * m_DataChannels;
}

unsigned int COMXAudio::AddPackets(const void* data, unsigned int len)
{
  return AddPackets(data, len, 0, 0);
}

//***********************************************************************************************
unsigned int COMXAudio::AddPackets(const void* data, unsigned int len, int64_t dts, int64_t pts)
{
  if(!m_Initialized) {
    CLog::Log(LOGERROR,"COMXAudio::AddPackets - sanity failed. no valid play handle!");
    return len;
  }

  if(m_eEncoding == OMX_AUDIO_CodingDTS && m_LostSync && m_Passthrough)
  {
    int skip = SyncDTS((uint8_t *)data, len);
    if(skip > 0)
      return len;
  }

  unsigned int length, frames;

  if (m_remap.CanRemap() && !m_Passthrough && m_Channels != m_DataChannels)
    length = (len / m_DataChannels) * m_Channels;
  else
  {
    length = len;
  }

  uint8_t *outData = (uint8_t *)malloc(length);

  if (m_remap.CanRemap() && !m_Passthrough && m_Channels != m_DataChannels)
  {
    frames = length / m_Channels / (m_BitsPerSample >> 3);
    if (frames > 0)
    {
      // remap the audio channels using the frame count
      m_remap.Remap((void*)data, outData, frames, m_drc);
    
      // return the number of input bytes we accepted
      len = (length / m_Channels) * m_DataChannels;
    } 
  }
  else
  {
    memcpy(outData, (uint8_t*) data, length);
  }

  unsigned int demuxer_bytes = (unsigned int)length;
  uint8_t *demuxer_content = outData;

  OMX_ERRORTYPE omx_err;

  unsigned int nSleepTime = 0;

  OMX_BUFFERHEADERTYPE *omx_buffer = NULL;

  while(demuxer_bytes)
  {
    omx_buffer = m_omx_decoder.GetInputBuffer();

    if(omx_buffer == NULL)
    {
      assert(0);
      /*
      OMXSleep(1);
      nSleepTime += 1;
      if(nSleepTime >= 200)
      {
        CLog::Log(LOGERROR, "COMXAudio::Decode timeout\n");
        printf("COMXAudio::Decode timeout\n");
        return len;
      }
      continue;
      */
    }

    nSleepTime = 0;

    omx_buffer->nOffset = 0;
    omx_buffer->nFlags  = 0;

    omx_buffer->nFilledLen = (demuxer_bytes > omx_buffer->nAllocLen) ? omx_buffer->nAllocLen : demuxer_bytes;
    memcpy(omx_buffer->pBuffer, demuxer_content, omx_buffer->nFilledLen);

    if(m_setStartTime) 
    {
      omx_buffer->nFlags = OMX_BUFFERFLAG_STARTTIME;

      m_setStartTime = false;
    }
    else
    {
      if((uint64_t)pts == AV_NOPTS_VALUE)
      {
        omx_buffer->nFlags = OMX_BUFFERFLAG_TIME_UNKNOWN;
      }
    }

    uint64_t val = ((uint64_t)pts == AV_NOPTS_VALUE) ? 0 : pts;
#ifdef OMX_SKIP64BIT
    if((uint64_t)pts == AV_NOPTS_VALUE || (omx_buffer->nFlags & OMX_BUFFERFLAG_TIME_UNKNOWN))
    {
      omx_buffer->nTimeStamp.nLowPart = 0;
      omx_buffer->nTimeStamp.nHighPart = 0;
    }
    else
    {
      omx_buffer->nTimeStamp.nLowPart = val & 0x00000000FFFFFFFF;
      omx_buffer->nTimeStamp.nHighPart = (val & 0xFFFFFFFF00000000) >> 32;
    }
#else
    omx_buffer->nTimeStamp = val; // in microseconds
#endif

    /*
    CLog::Log(LOGDEBUG, "COMXAudio::AddPackets ADec : pts %lld omx_buffer 0x%08x buffer 0x%08x number %d\n", 
        pts, omx_buffer, omx_buffer->pBuffer, (int)omx_buffer->pAppPrivate);
    printf("ADec : pts %lld omx_buffer 0x%08x buffer 0x%08x number %d\n", 
        pts, omx_buffer, omx_buffer->pBuffer, (int)omx_buffer->pAppPrivate);
    */

    if (m_SampleSize > 0 && (uint64_t)pts != AV_NOPTS_VALUE && !(omx_buffer->nFlags & OMX_BUFFERFLAG_TIME_UNKNOWN))
    {
      pts += ((double)omx_buffer->nFilledLen * DVD_TIME_BASE) / m_SampleSize;
    }

    demuxer_bytes -= omx_buffer->nFilledLen;
    demuxer_content += omx_buffer->nFilledLen;

    if(demuxer_bytes == 0)
      omx_buffer->nFlags |= OMX_BUFFERFLAG_ENDOFFRAME;

    omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);

      printf("%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);

      free(outData);
      return 0;
    }

    if(m_firstFrame)
    {
      m_firstFrame = false;
      //m_omx_render.WaitForEvent(OMX_EventPortSettingsChanged);

      m_omx_render.SendCommand(OMX_CommandPortDisable, m_omx_render.GetInputPort(), NULL);
      m_omx_decoder.SendCommand(OMX_CommandPortDisable, m_omx_decoder.GetOutputPort(), NULL);

      m_omx_render.WaitForCommand(OMX_CommandPortDisable, m_omx_render.GetInputPort());
      m_omx_decoder.WaitForCommand(OMX_CommandPortDisable, m_omx_decoder.GetOutputPort());

      if(!m_Passthrough /*|| m_eEncoding == OMX_AUDIO_CodingMP3 || m_eEncoding == OMX_AUDIO_CodingVORBIS*/)
      {
        m_pcm.nPortIndex      = m_omx_decoder.GetOutputPort();
        m_omx_decoder.GetParameter(OMX_IndexParamAudioPcm, &m_pcm);

        m_pcm.nPortIndex      = m_omx_render.GetInputPort();
        m_omx_render.SetParameter(OMX_IndexParamAudioPcm, &m_pcm);
        m_omx_render.GetParameter(OMX_IndexParamAudioPcm, &m_pcm);

        PrintPCM(&m_pcm);
      }
      else
      {
        m_pcm.nPortIndex      = m_omx_decoder.GetOutputPort();
        m_omx_decoder.GetParameter(OMX_IndexParamAudioPcm, &m_pcm);
        PrintPCM(&m_pcm);

        OMX_AUDIO_PARAM_PORTFORMATTYPE formatType;
        OMX_INIT_STRUCTURE(formatType);
        formatType.nPortIndex = m_omx_render.GetInputPort();

        omx_err = m_omx_render.GetParameter(OMX_IndexParamAudioPortFormat, &formatType);
        if(omx_err != OMX_ErrorNone)
        {
          CLog::Log(LOGERROR, "COMXAudio::AddPackets error OMX_IndexParamAudioPortFormat omx_err(0x%08x)\n", omx_err);
          assert(0);
        }

        formatType.eEncoding = m_eEncoding;

        omx_err = m_omx_render.SetParameter(OMX_IndexParamAudioPortFormat, &formatType);
        if(omx_err != OMX_ErrorNone)
        {
          CLog::Log(LOGERROR, "COMXAudio::AddPackets error OMX_IndexParamAudioPortFormat omx_err(0x%08x)\n", omx_err);
          assert(0);
        }

        if(m_eEncoding == OMX_AUDIO_CodingDDP)
        {
          OMX_AUDIO_PARAM_DDPTYPE m_ddParam;
          OMX_INIT_STRUCTURE(m_ddParam);

          m_ddParam.nPortIndex      = m_omx_render.GetInputPort();

          m_ddParam.nChannels       = (m_DataChannels == 6) ? 8 : m_DataChannels;
          m_ddParam.nSampleRate     = m_SampleRate;
          m_ddParam.eBitStreamId    = OMX_AUDIO_DDPBitStreamIdAC3;
          m_ddParam.nBitRate        = 0;

          for(unsigned int i = 0; i < OMX_AUDIO_MAXCHANNELS; i++)
          {
            if(i >= m_ddParam.nChannels)
            {
              break;
            }
            m_ddParam.eChannelMapping[i] = OMXChannels[i];
          }
  
          m_omx_render.SetParameter(OMX_IndexParamAudioDdp, &m_ddParam);
          m_omx_render.GetParameter(OMX_IndexParamAudioDdp, &m_ddParam);
          PrintDDP(&m_ddParam);
        }
        else if(m_eEncoding == OMX_AUDIO_CodingDTS)
        {
          m_dtsParam.nPortIndex      = m_omx_render.GetInputPort();

          m_dtsParam.nChannels       = (m_DataChannels == 6) ? 8 : m_DataChannels;
          m_dtsParam.nBitRate        = 0;

          for(unsigned int i = 0; i < OMX_AUDIO_MAXCHANNELS; i++)
          {
            if(i >= m_dtsParam.nChannels)
            {
              break;
            }
            m_dtsParam.eChannelMapping[i] = OMXChannels[i];
          }
  
          m_omx_render.SetParameter(OMX_IndexParamAudioDts, &m_dtsParam);
          m_omx_render.GetParameter(OMX_IndexParamAudioDts, &m_dtsParam);
          PrintDTS(&m_dtsParam);
        }
      }

      m_omx_render.SendCommand(OMX_CommandPortEnable, m_omx_render.GetInputPort(), NULL);
      m_omx_decoder.SendCommand(OMX_CommandPortEnable, m_omx_decoder.GetOutputPort(), NULL);

      m_omx_render.WaitForCommand(OMX_CommandPortEnable, m_omx_render.GetInputPort());
      m_omx_decoder.WaitForCommand(OMX_CommandPortEnable, m_omx_decoder.GetOutputPort());
    }
 
  }
  free(outData);

  return len;
}

//***********************************************************************************************
float COMXAudio::GetDelay()
{
  unsigned int free = m_omx_decoder.GetInputBufferSize() - m_omx_decoder.GetInputBufferSpace();
  return (float)free / (float)m_BytesPerSec;
}

float COMXAudio::GetCacheTime()
{
  /*
  unsigned int nBufferLenFull = (m_BufferLen / m_Channels) * m_DataChannels; 
  return (float)(nBufferLenFull - GetSpace()) / (float)m_BytesPerSec;
  */
  float fBufferLenFull = (float)m_BufferLen - (float)GetSpace();
  if(fBufferLenFull < 0)
    fBufferLenFull = 0;
  float ret = fBufferLenFull / (float)m_BytesPerSec;
  return ret;
}

float COMXAudio::GetCacheTotal()
{
  return (float)m_BufferLen / (float)m_BytesPerSec;
}

//***********************************************************************************************
unsigned int COMXAudio::GetChunkLen()
{
  return (m_ChunkLen / m_Channels) * m_DataChannels;
}
//***********************************************************************************************
int COMXAudio::SetPlaySpeed(int iSpeed)
{
  return 0;
}

void COMXAudio::RegisterAudioCallback(IAudioCallback *pCallback)
{
  m_pCallback = pCallback;
}

void COMXAudio::UnRegisterAudioCallback()
{
  m_pCallback = NULL;
}

void COMXAudio::WaitCompletion()
{
  if(!m_Initialized || m_Pause)
    return;

  /*
  OMX_PARAM_U32TYPE param;

  memset(&param, 0, sizeof(OMX_PARAM_U32TYPE));
  param.nSize = sizeof(OMX_PARAM_U32TYPE);
  param.nVersion.nVersion = OMX_VERSION;
  param.nPortIndex = m_omx_render.GetInputPort();

  unsigned int start = XbmcThreads::SystemClockMillis();

  // maximum wait 1s.
  while((XbmcThreads::SystemClockMillis() - start) < 1000) {
    if(m_BufferCount == m_omx_input_avaliable.size())
      break;

    OMXSleep(100);
    //printf("WaitCompletion\n");
  }
  */
}

void COMXAudio::SwitchChannels(int iAudioStream, bool bAudioOnAllSpeakers)
{
    return ;
}

void COMXAudio::EnumerateAudioSinks(AudioSinkList& vAudioSinks, bool passthrough)
{
#ifndef STANDALONE
  if (!passthrough)
  {
    vAudioSinks.push_back(AudioSink(g_localizeStrings.Get(409) + " (OMX)", "omx:default"));
    vAudioSinks.push_back(AudioSink("analog (OMX)" , "omx:analog"));
    vAudioSinks.push_back(AudioSink("hdmi (OMX)"   , "omx:hdmi"));
  }
  else
  {
    vAudioSinks.push_back(AudioSink("hdmi (OMX)"   , "omx:hdmi"));
  }
#endif
}

bool COMXAudio::SetClock(OMXClock *clock)
{
  if(m_av_clock != NULL)
    return false;

  m_av_clock = clock;
  m_external_clock = true;
  return true;
}
 
void COMXAudio::SetCodingType(CodecID codec)
{
  switch(codec)
  { 
    /*
    case CODEC_ID_VORBIS:
      printf("OMX_AUDIO_CodingVORBIS\n");
      m_eEncoding = OMX_AUDIO_CodingVORBIS;
      break;
    case CODEC_ID_MP3:
      printf("OMX_AUDIO_CodingMP3\n");
      m_eEncoding = OMX_AUDIO_CodingMP3;
      break;
    */
    case CODEC_ID_DTS:
      //printf("OMX_AUDIO_CodingDTS\n");
      m_eEncoding = OMX_AUDIO_CodingDTS;
      break;
    case CODEC_ID_AC3:
    case CODEC_ID_EAC3:
      //printf("OMX_AUDIO_CodingDDP\n");
      m_eEncoding = OMX_AUDIO_CodingDDP;
      break;
    default:
      //printf("OMX_AUDIO_CodingPCM\n");
      m_eEncoding = OMX_AUDIO_CodingPCM;
      break;
  } 
}

void COMXAudio::PrintChannels(OMX_AUDIO_CHANNELTYPE eChannelMapping[])
{
  for(int i = 0; i < OMX_AUDIO_MAXCHANNELS; i++)
  {
    switch(eChannelMapping[i])
    {
      case OMX_AUDIO_ChannelLF:
        CLog::Log(LOGDEBUG, "OMX_AUDIO_ChannelLF\n");
        break;
      case OMX_AUDIO_ChannelRF:
        CLog::Log(LOGDEBUG, "OMX_AUDIO_ChannelRF\n");
        break;
      case OMX_AUDIO_ChannelCF:
        CLog::Log(LOGDEBUG, "OMX_AUDIO_ChannelCF\n");
        break;
      case OMX_AUDIO_ChannelLS:
        CLog::Log(LOGDEBUG, "OMX_AUDIO_ChannelLS\n");
        break;
      case OMX_AUDIO_ChannelRS:
        CLog::Log(LOGDEBUG, "OMX_AUDIO_ChannelRS\n");
        break;
      case OMX_AUDIO_ChannelLFE:
        CLog::Log(LOGDEBUG, "OMX_AUDIO_ChannelLFE\n");
        break;
      case OMX_AUDIO_ChannelCS:
        CLog::Log(LOGDEBUG, "OMX_AUDIO_ChannelCS\n");
        break;
      case OMX_AUDIO_ChannelLR:
        CLog::Log(LOGDEBUG, "OMX_AUDIO_ChannelLR\n");
        break;
      case OMX_AUDIO_ChannelRR:
        CLog::Log(LOGDEBUG, "OMX_AUDIO_ChannelRR\n");
        break;
      case OMX_AUDIO_ChannelNone:
      case OMX_AUDIO_ChannelKhronosExtensions:
      case OMX_AUDIO_ChannelVendorStartUnused:
      case OMX_AUDIO_ChannelMax:
      default:
        break;
    }
  }
}

void COMXAudio::PrintPCM(OMX_AUDIO_PARAM_PCMMODETYPE *pcm)
{
  CLog::Log(LOGDEBUG, "pcm->nPortIndex     : %d\n", (int)pcm->nPortIndex);
  CLog::Log(LOGDEBUG, "pcm->eNumData       : %d\n", pcm->eNumData);
  CLog::Log(LOGDEBUG, "pcm->eEndian        : %d\n", pcm->eEndian);
  CLog::Log(LOGDEBUG, "pcm->bInterleaved   : %d\n", (int)pcm->bInterleaved);
  CLog::Log(LOGDEBUG, "pcm->nBitPerSample  : %d\n", (int)pcm->nBitPerSample);
  CLog::Log(LOGDEBUG, "pcm->ePCMMode       : %d\n", pcm->ePCMMode);
  CLog::Log(LOGDEBUG, "pcm->nChannels      : %d\n", (int)pcm->nChannels);
  CLog::Log(LOGDEBUG, "pcm->nSamplingRate  : %d\n", (int)pcm->nSamplingRate);

  PrintChannels(pcm->eChannelMapping);
}

void COMXAudio::PrintDDP(OMX_AUDIO_PARAM_DDPTYPE *ddparm)
{
  CLog::Log(LOGDEBUG, "ddparm->nPortIndex         : %d\n", (int)ddparm->nPortIndex);
  CLog::Log(LOGDEBUG, "ddparm->nChannels          : %d\n", (int)ddparm->nChannels);
  CLog::Log(LOGDEBUG, "ddparm->nBitRate           : %d\n", (int)ddparm->nBitRate);
  CLog::Log(LOGDEBUG, "ddparm->nSampleRate        : %d\n", (int)ddparm->nSampleRate);
  CLog::Log(LOGDEBUG, "ddparm->eBitStreamId       : %d\n", (int)ddparm->eBitStreamId);
  CLog::Log(LOGDEBUG, "ddparm->eBitStreamMode     : %d\n", (int)ddparm->eBitStreamMode);
  CLog::Log(LOGDEBUG, "ddparm->eDolbySurroundMode : %d\n", (int)ddparm->eDolbySurroundMode);

  PrintChannels(ddparm->eChannelMapping);
}

void COMXAudio::PrintDTS(OMX_AUDIO_PARAM_DTSTYPE *dtsparam)
{
  CLog::Log(LOGDEBUG, "dtsparam->nPortIndex         : %d\n", (int)dtsparam->nPortIndex);
  CLog::Log(LOGDEBUG, "dtsparam->nChannels          : %d\n", (int)dtsparam->nChannels);
  CLog::Log(LOGDEBUG, "dtsparam->nBitRate           : %d\n", (int)dtsparam->nBitRate);
  CLog::Log(LOGDEBUG, "dtsparam->nSampleRate        : %d\n", (int)dtsparam->nSampleRate);
  CLog::Log(LOGDEBUG, "dtsparam->nFormat            : 0x%08x\n", dtsparam->nFormat);
  CLog::Log(LOGDEBUG, "dtsparam->nDtsType           : %d\n", (int)dtsparam->nDtsType);
  CLog::Log(LOGDEBUG, "dtsparam->nDtsFrameSizeBytes : %d\n", (int)dtsparam->nDtsFrameSizeBytes);

  PrintChannels(dtsparam->eChannelMapping);
}

unsigned int COMXAudio::SyncDTS(BYTE* pData, unsigned int iSize)
{
  OMX_INIT_STRUCTURE(m_dtsParam);

  unsigned int skip;
  unsigned int srCode;
  unsigned int dtsBlocks;
  bool littleEndian;

  for(skip = 0; iSize - skip > 8; ++skip, ++pData)
  {
    if (pData[0] == 0x7F && pData[1] == 0xFE && pData[2] == 0x80 && pData[3] == 0x01) 
    {
      /* 16bit le */
      littleEndian = true; 
      dtsBlocks    = ((pData[4] >> 2) & 0x7f) + 1;
      m_dtsParam.nFormat = 0x1 | 0x0;
    }
    else if (pData[0] == 0x1F && pData[1] == 0xFF && pData[2] == 0xE8 && pData[3] == 0x00 && pData[4] == 0x07 && (pData[5] & 0xF0) == 0xF0) 
    {
      /* 14bit le */
      littleEndian = true;
      dtsBlocks    = (((pData[4] & 0x7) << 4) | (pData[7] & 0x3C) >> 2) + 1;
      m_dtsParam.nFormat = 0x0 | 0x0;
    }
    else if (pData[1] == 0x7F && pData[0] == 0xFE && pData[3] == 0x80 && pData[2] == 0x01) 
    {
      /* 16bit be */ 
      m_dtsParam.nFormat = 0x1 | 0x2;
      dtsBlocks    = ((pData[5] >> 2) & 0x7f) + 1;
      littleEndian = false;
    }
    else if (pData[1] == 0x1F && pData[0] == 0xFF && pData[3] == 0xE8 && pData[2] == 0x00 && pData[5] == 0x07 && (pData[4] & 0xF0) == 0xF0) 
    {
      /* 14bit be */
      littleEndian = false; 
      dtsBlocks    = (((pData[5] & 0x7) << 4) | (pData[6] & 0x3C) >> 2) + 1;
      m_dtsParam.nFormat = 0x0 | 0x2;
    }
    else
    {
      continue;
    }

    if (littleEndian)
    {
      /* if it is not a termination frame, check the next 6 bits are set */
      if ((pData[4] & 0x80) == 0x80 && (pData[4] & 0x7C) != 0x7C)
        continue;

      /* get the frame size */
      m_dtsParam.nDtsFrameSizeBytes = ((((pData[5] & 0x3) << 8 | pData[6]) << 4) | ((pData[7] & 0xF0) >> 4)) + 1;
      srCode = (pData[8] & 0x3C) >> 2;
   }
   else
   {
      /* if it is not a termination frame, check the next 6 bits are set */
      if ((pData[5] & 0x80) == 0x80 && (pData[5] & 0x7C) != 0x7C)
        continue;

      /* get the frame size */
      m_dtsParam.nDtsFrameSizeBytes = ((((pData[4] & 0x3) << 8 | pData[7]) << 4) | ((pData[6] & 0xF0) >> 4)) + 1;
      srCode = (pData[9] & 0x3C) >> 2;
   }

    /* make sure the framesize is sane */
    if (m_dtsParam.nDtsFrameSizeBytes < 96 || m_dtsParam.nDtsFrameSizeBytes > 16384)
      continue;

    m_dtsParam.nSampleRate = DTSFSCod[srCode];

    switch(dtsBlocks << 5)
    {
      case 512 : 
        m_dtsParam.nDtsType = 1;
        break;
      case 1024: 
        m_dtsParam.nDtsType = 2;
        break;
      case 2048: 
        m_dtsParam.nDtsType = 3;
        break;
      default:
        m_dtsParam.nDtsType = 0;
        break;
    }

    m_dtsParam.nFormat = 1;
    m_dtsParam.nDtsType = 1;

    m_LostSync = false;

    return skip;
  }

  m_LostSync = true;
  return iSize;
}

