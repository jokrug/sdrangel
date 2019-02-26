///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2019 Edouard Griffiths, F4EXB                                   //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#include <stdio.h>

#include <QTime>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QBuffer>

#include "codec2/freedv_api.h"

#include "SWGChannelSettings.h"
#include "SWGFreeDVDemodSettings.h"
#include "SWGChannelReport.h"
#include "SWGFreeDVDemodReport.h"

#include "audio/audiooutput.h"
#include "dsp/dspengine.h"
#include "dsp/downchannelizer.h"
#include "dsp/threadedbasebandsamplesink.h"
#include "dsp/dspcommands.h"
#include "device/devicesourceapi.h"
#include "util/db.h"

#include "freedvdemod.h"

MESSAGE_CLASS_DEFINITION(FreeDVDemod::MsgConfigureFreeDVDemod, Message)
MESSAGE_CLASS_DEFINITION(FreeDVDemod::MsgConfigureFreeDVDemodPrivate, Message)
MESSAGE_CLASS_DEFINITION(FreeDVDemod::MsgConfigureChannelizer, Message)

const QString FreeDVDemod::m_channelIdURI = "sdrangel.channel.freedvdemod";
const QString FreeDVDemod::m_channelId = "FreeDVDemod";

FreeDVDemod::FreeDVDemod(DeviceSourceAPI *deviceAPI) :
        ChannelSinkAPI(m_channelIdURI),
        m_deviceAPI(deviceAPI),
        m_hiCutoff(5000),
        m_lowCutoff(300),
        m_volume(2),
        m_spanLog2(3),
        m_sum(0),
        m_inputSampleRate(48000),
        m_modemSampleRate(48000),
        m_speechSampleRate(8000), // fixed 8 kS/s
        m_inputFrequencyOffset(0),
        m_audioMute(false),
        m_agc(12000, agcTarget, 1e-2),
        m_agcActive(false),
        m_agcClamping(false),
        m_agcNbSamples(12000),
        m_agcPowerThreshold(1e-2),
        m_agcThresholdGate(0),
        m_squelchDelayLine(2*48000),
        m_audioActive(false),
        m_sampleSink(0),
        m_audioFifo(24000),
        m_freeDV(0),
        m_nSpeechSamples(0),
        m_nMaxModemSamples(0),
        m_iSpeech(0),
        m_iModem(0),
        m_speechOut(0),
        m_modIn(0),
        m_scaleFactor(SDR_RX_SCALEF),
        m_settingsMutex(QMutex::Recursive)
{
	setObjectName(m_channelId);

    DSPEngine::instance()->getAudioDeviceManager()->addAudioSink(&m_audioFifo, getInputMessageQueue());
    uint32_t audioSampleRate = DSPEngine::instance()->getAudioDeviceManager()->getOutputSampleRate();
    applyAudioSampleRate(audioSampleRate);

	m_audioBuffer.resize(1<<14);
	m_audioBufferFill = 0;
	m_undersampleCount = 0;

	m_magsq = 0.0f;
	m_magsqSum = 0.0f;
	m_magsqPeak = 0.0f;
	m_magsqCount = 0;

	m_agc.setClampMax(SDR_RX_SCALED/100.0);
	m_agc.setClamping(m_agcClamping);

	SSBFilter = new fftfilt(m_lowCutoff / m_audioSampleRate, m_hiCutoff / m_audioSampleRate, ssbFftLen);

    applyChannelSettings(m_inputSampleRate, m_inputFrequencyOffset, true);
	applySettings(m_settings, true);

    m_channelizer = new DownChannelizer(this);
    m_threadedChannelizer = new ThreadedBasebandSampleSink(m_channelizer, this);
    m_deviceAPI->addThreadedSink(m_threadedChannelizer);
    m_deviceAPI->addChannelAPI(this);

    m_networkManager = new QNetworkAccessManager();
    connect(m_networkManager, SIGNAL(finished(QNetworkReply*)), this, SLOT(networkManagerFinished(QNetworkReply*)));
}

FreeDVDemod::~FreeDVDemod()
{
    disconnect(m_networkManager, SIGNAL(finished(QNetworkReply*)), this, SLOT(networkManagerFinished(QNetworkReply*)));
    delete m_networkManager;
	DSPEngine::instance()->getAudioDeviceManager()->removeAudioSink(&m_audioFifo);

	m_deviceAPI->removeChannelAPI(this);
    m_deviceAPI->removeThreadedSink(m_threadedChannelizer);
    delete m_threadedChannelizer;
    delete m_channelizer;
    delete SSBFilter;
}

void FreeDVDemod::configure(MessageQueue* messageQueue,
		Real Bandwidth,
		Real LowCutoff,
		Real volume,
		int spanLog2,
		bool audioBinaural,
		bool audioFlipChannel,
		bool dsb,
		bool audioMute,
		bool agc,
		bool agcClamping,
        int agcTimeLog2,
        int agcPowerThreshold,
        int agcThresholdGate)
{
	Message* cmd = MsgConfigureFreeDVDemodPrivate::create(
	        Bandwidth,
	        LowCutoff,
	        volume,
	        spanLog2,
	        audioBinaural,
	        audioFlipChannel,
	        dsb,
	        audioMute,
	        agc,
	        agcClamping,
	        agcTimeLog2,
	        agcPowerThreshold,
	        agcThresholdGate);
	messageQueue->push(cmd);
}

void FreeDVDemod::feed(const SampleVector::const_iterator& begin, const SampleVector::const_iterator& end, bool positiveOnly)
{
    (void) positiveOnly;
	Complex ci;
	fftfilt::cmplx *sideband;
	int n_out;

	m_settingsMutex.lock();

	int decim = 1<<(m_spanLog2 - 1);
	unsigned char decim_mask = decim - 1; // counter LSB bit mask for decimation by 2^(m_scaleLog2 - 1)

	for(SampleVector::const_iterator it = begin; it < end; ++it)
	{
		Complex c(it->real(), it->imag());
		c *= m_nco.nextIQ();

		if(m_interpolator.decimate(&m_interpolatorDistanceRemain, c, &ci))
		{
            n_out = SSBFilter->runSSB(ci, &sideband, true);
			m_interpolatorDistanceRemain += m_interpolatorDistance;
		}
		else
		{
			n_out = 0;
		}

		for (int i = 0; i < n_out; i++)
		{
			// Downsample by 2^(m_scaleLog2 - 1) for SSB band spectrum display
			// smart decimation with bit gain using float arithmetic (23 bits significand)

			m_sum += sideband[i];

			if (!(m_undersampleCount++ & decim_mask))
			{
				Real avgr = m_sum.real() / decim;
				Real avgi = m_sum.imag() / decim;
				m_magsq = (avgr * avgr + avgi * avgi) / (SDR_RX_SCALED*SDR_RX_SCALED);

                m_magsqSum += m_magsq;

                if (m_magsq > m_magsqPeak)
                {
                    m_magsqPeak = m_magsq;
                }

                m_magsqCount++;
                m_sampleBuffer.push_back(Sample(avgr, avgi));
                m_sum.real(0.0);
                m_sum.imag(0.0);
			}

            float agcVal = m_agcActive ? m_agc.feedAndGetValue(sideband[i]) : 10.0; // 10.0 for 3276.8, 1.0 for 327.68
            fftfilt::cmplx& delayedSample = m_squelchDelayLine.readBack(m_agc.getStepDownDelay());
            m_audioActive = delayedSample.real() != 0.0;
            m_squelchDelayLine.write(sideband[i]*agcVal);
            fftfilt::cmplx z = delayedSample * m_agc.getStepValue();
            Real demod = (z.real() + z.imag()) * 0.7;

            pushSampleToDV((qint16) demod);
		}
	}

	uint res = m_audioFifo.write((const quint8*)&m_audioBuffer[0], m_audioBufferFill);

	if (res != m_audioBufferFill)
	{
        qDebug("FreeDVDemod::feed: %u/%u tail samples written", res, m_audioBufferFill);
	}

	m_audioBufferFill = 0;

	if (m_sampleSink != 0)
	{
		m_sampleSink->feed(m_sampleBuffer.begin(), m_sampleBuffer.end(), true);
	}

	m_sampleBuffer.clear();

	m_settingsMutex.unlock();
}

void FreeDVDemod::start()
{
    applyChannelSettings(m_inputSampleRate, m_inputFrequencyOffset, true);
}

void FreeDVDemod::stop()
{
}

bool FreeDVDemod::handleMessage(const Message& cmd)
{
	if (DownChannelizer::MsgChannelizerNotification::match(cmd))
	{
		DownChannelizer::MsgChannelizerNotification& notif = (DownChannelizer::MsgChannelizerNotification&) cmd;
		qDebug("FreeDVDemod::handleMessage: MsgChannelizerNotification: m_sampleRate");

		applyChannelSettings(notif.getSampleRate(), notif.getFrequencyOffset());

		return true;
	}
    else if (MsgConfigureChannelizer::match(cmd))
    {
        MsgConfigureChannelizer& cfg = (MsgConfigureChannelizer&) cmd;
        qDebug() << "FreeDVDemod::handleMessage: MsgConfigureChannelizer: sampleRate: " << cfg.getSampleRate()
                << " centerFrequency: " << cfg.getCenterFrequency();

        m_channelizer->configure(m_channelizer->getInputMessageQueue(),
            cfg.getSampleRate(),
            cfg.getCenterFrequency());

        return true;
    }
    else if (MsgConfigureFreeDVDemod::match(cmd))
    {
        MsgConfigureFreeDVDemod& cfg = (MsgConfigureFreeDVDemod&) cmd;
        qDebug("FreeDVDemod::handleMessage: MsgConfigureFreeDVDemod");

        applySettings(cfg.getSettings(), cfg.getForce());

        return true;
    }
    else if (BasebandSampleSink::MsgThreadedSink::match(cmd))
    {
        BasebandSampleSink::MsgThreadedSink& cfg = (BasebandSampleSink::MsgThreadedSink&) cmd;
        const QThread *thread = cfg.getThread();
        qDebug("FreeDVDemod::handleMessage: BasebandSampleSink::MsgThreadedSink: %p", thread);
        return true;
    }
    else if (DSPConfigureAudio::match(cmd))
    {
        DSPConfigureAudio& cfg = (DSPConfigureAudio&) cmd;
        uint32_t sampleRate = cfg.getSampleRate();

        qDebug() << "FreeDVDemod::handleMessage: DSPConfigureAudio:"
                << " sampleRate: " << sampleRate;

        if (sampleRate != m_audioSampleRate) {
            applyAudioSampleRate(sampleRate);
        }

        return true;
    }
    else if (DSPSignalNotification::match(cmd))
    {
        return true;
    }
	else
	{
		if(m_sampleSink != 0)
		{
		   return m_sampleSink->handleMessage(cmd);
		}
		else
		{
			return false;
		}
	}
}

void FreeDVDemod::pushSampleToDV(int16_t sample)
{
    qint16 speechSample, audioSample;

    if (m_iModem == m_nin)
    {
        int nout = freedv_rx(m_freeDV, m_speechOut, m_modIn);

        for (int i = 0; i < nout; i++)
        {
            speechSample = (qint16)(m_speechOut[i] * m_volume);

            while (!m_audioResampler.upSample(speechSample, audioSample)) {
                pushSampleToAudio(audioSample);
            }

            pushSampleToAudio(audioSample);
        }

        m_iModem = 0;
        m_iSpeech = 0;
    }

    m_modIn[m_iModem++] = sample;
}

void FreeDVDemod::pushSampleToAudio(int16_t sample)
{
    m_audioBuffer[m_audioBufferFill].l = sample;
    m_audioBuffer[m_audioBufferFill].r = sample;
    ++m_audioBufferFill;

    if (m_audioBufferFill >= m_audioBuffer.size())
    {
        uint res = m_audioFifo.write((const quint8*)&m_audioBuffer[0], m_audioBufferFill);

        if (res != m_audioBufferFill) {
            qDebug("FreeDVDemod::pushSampleToAudio: %u/%u samples written", res, m_audioBufferFill);
        }

        m_audioBufferFill = 0;
    }
}

void FreeDVDemod::applyChannelSettings(int inputSampleRate, int inputFrequencyOffset, bool force)
{
    qDebug() << "FreeDVDemod::applyChannelSettings:"
            << " inputSampleRate: " << inputSampleRate
            << " inputFrequencyOffset: " << inputFrequencyOffset;

    if ((m_inputFrequencyOffset != inputFrequencyOffset) ||
        (m_inputSampleRate != inputSampleRate) || force)
    {
        m_nco.setFreq(-inputFrequencyOffset, inputSampleRate);
    }

    if ((m_inputSampleRate != inputSampleRate) || force)
    {
        m_settingsMutex.lock();
        m_interpolator.create(16, inputSampleRate, m_hiCutoff * 1.5f, 2.0f);
        m_interpolatorDistanceRemain = 0;
        m_interpolatorDistance = (Real) inputSampleRate / (Real) m_audioSampleRate;
        m_settingsMutex.unlock();
    }

    m_inputSampleRate = inputSampleRate;
    m_inputFrequencyOffset = inputFrequencyOffset;
}

void FreeDVDemod::applyAudioSampleRate(int sampleRate)
{
    qDebug("FreeDVDemod::applyAudioSampleRate: %d", sampleRate);

    m_settingsMutex.lock();
    m_audioFifo.setSize(sampleRate);
    m_audioResampler.setDecimation(sampleRate / m_speechSampleRate);
    m_audioResampler.setAudioFilters(sampleRate, m_speechSampleRate, 250, 3300);
    m_settingsMutex.unlock();

    m_audioSampleRate = sampleRate;
}

void FreeDVDemod::applyFreeDVMode(FreeDVDemodSettings::FreeDVMode mode)
{
    m_hiCutoff = FreeDVDemodSettings::getHiCutoff(mode);
    m_lowCutoff = FreeDVDemodSettings::getLowCutoff(mode);
    uint32_t modemSampleRate = FreeDVDemodSettings::getModSampleRate(mode);

    m_settingsMutex.lock();

    // baseband interpolator and filter
    if (modemSampleRate != m_modemSampleRate)
    {
        MsgConfigureChannelizer* channelConfigMsg = MsgConfigureChannelizer::create(
                modemSampleRate, m_settings.m_inputFrequencyOffset);
        m_inputMessageQueue.push(channelConfigMsg);

        m_interpolator.create(16, m_inputSampleRate, m_hiCutoff * 1.5f, 2.0f);
        m_interpolatorDistanceRemain = 0;
        m_interpolatorDistance = (Real) m_inputSampleRate / (Real) modemSampleRate;

        SSBFilter->create_filter(m_lowCutoff / (float) modemSampleRate, m_hiCutoff / (float) modemSampleRate);

        int agcNbSamples = (modemSampleRate / 1000) * (1<<m_settings.m_agcTimeLog2);
        int agcThresholdGate = (modemSampleRate / 1000) * m_settings.m_agcThresholdGate; // ms

        if (m_agcNbSamples != agcNbSamples)
        {
            m_agc.resize(agcNbSamples, agcNbSamples/2, agcTarget);
            m_agc.setStepDownDelay(agcNbSamples);
            m_agcNbSamples = agcNbSamples;
        }

        if (m_agcThresholdGate != agcThresholdGate)
        {
            m_agc.setGate(agcThresholdGate);
            m_agcThresholdGate = agcThresholdGate;
        }

        m_modemSampleRate = modemSampleRate;

        if (getMessageQueueToGUI())
        {
            DSPConfigureAudio *cfg = new DSPConfigureAudio(m_modemSampleRate);
            getMessageQueueToGUI()->push(cfg);
        }
    }

    // FreeDV object

    if (m_freeDV) {
        freedv_close(m_freeDV);
    }

    int fdv_mode = -1;

    switch(mode)
    {
    case FreeDVDemodSettings::FreeDVMode700D:
        fdv_mode = FREEDV_MODE_700D;
        m_scaleFactor = SDR_RX_SCALEF / 3.2f;
        break;
    case FreeDVDemodSettings::FreeDVMode800XA:
        fdv_mode = FREEDV_MODE_800XA;
        m_scaleFactor = SDR_RX_SCALEF / 8.2f;
        break;
    case FreeDVDemodSettings::FreeDVMode1600:
        fdv_mode = FREEDV_MODE_1600;
        m_scaleFactor = SDR_RX_SCALEF / 3.2f;
        break;
    case FreeDVDemodSettings::FreeDVMode2400A:
    default:
        fdv_mode = FREEDV_MODE_2400A;
        m_scaleFactor = SDR_RX_SCALEF / 8.2f;
        break;
    }

    if (fdv_mode == FREEDV_MODE_700D)
    {
        struct freedv_advanced adv;
        adv.interleave_frames = 1;
        m_freeDV = freedv_open_advanced(fdv_mode, &adv);
    }
    else
    {
        m_freeDV = freedv_open(fdv_mode);
    }

    if (m_freeDV)
    {
        freedv_set_test_frames(m_freeDV, 0);
        freedv_set_snr_squelch_thresh(m_freeDV, -100.0);
        freedv_set_squelch_en(m_freeDV, 0);
        freedv_set_clip(m_freeDV, 0);
        freedv_set_tx_bpf(m_freeDV, 1);
        freedv_set_ext_vco(m_freeDV, 0);

        int nSpeechSamples = freedv_get_n_speech_samples(m_freeDV);
        int nMaxModemSamples = freedv_get_n_max_modem_samples(m_freeDV);
        int Fs = freedv_get_modem_sample_rate(m_freeDV);
        int Rs = freedv_get_modem_symbol_rate(m_freeDV);

        if (nSpeechSamples != m_nSpeechSamples)
        {
            if (m_speechOut) {
                delete[] m_speechOut;
            }

            m_speechOut = new int16_t[nSpeechSamples];
            m_nSpeechSamples = nSpeechSamples;
        }

        if (nMaxModemSamples != m_nMaxModemSamples)
        {
            if (m_modIn) {
                delete[] m_modIn;
            }

            m_modIn = new int16_t[nMaxModemSamples];
            m_nMaxModemSamples = nMaxModemSamples;
        }

        m_iSpeech = 0;
        m_iModem = 0;
        m_nin = freedv_nin(m_freeDV);

        qDebug() << "FreeDVMod::applyFreeDVMode:"
                << " fdv_mode: " << fdv_mode
                << " m_modemSampleRate: " << m_modemSampleRate
                << " Fs: " << Fs
                << " Rs: " << Rs
                << " m_nSpeechSamples: " << m_nSpeechSamples
                << " m_nMaxModemSamples: " << m_nMaxModemSamples
                << " m_nin: " << m_nin;
    }

    m_settingsMutex.unlock();
}

void FreeDVDemod::applySettings(const FreeDVDemodSettings& settings, bool force)
{
    qDebug() << "FreeDVDemod::applySettings:"
            << " m_inputFrequencyOffset: " << settings.m_inputFrequencyOffset
            << " m_volume: " << settings.m_volume
            << " m_spanLog2: " << settings.m_spanLog2
            << " m_audioMute: " << settings.m_audioMute
            << " m_agcActive: " << settings.m_agc
            << " m_agcClamping: " << settings.m_agcClamping
            << " m_agcTimeLog2: " << settings.m_agcTimeLog2
            << " agcPowerThreshold: " << settings.m_agcPowerThreshold
            << " agcThresholdGate: " << settings.m_agcThresholdGate
            << " m_audioDeviceName: " << settings.m_audioDeviceName
            << " force: " << force;

    QList<QString> reverseAPIKeys;

    if((m_settings.m_inputFrequencyOffset != settings.m_inputFrequencyOffset) || force) {
        reverseAPIKeys.append("inputFrequencyOffset");
    }

    if ((m_settings.m_volume != settings.m_volume) || force)
    {
        reverseAPIKeys.append("volume");
        m_volume = settings.m_volume;
        m_volume /= 4.0; // for 3276.8
    }

    if ((m_settings.m_agcTimeLog2 != settings.m_agcTimeLog2) || force) {
        reverseAPIKeys.append("agcTimeLog2");
    }
    if ((m_settings.m_agcPowerThreshold != settings.m_agcPowerThreshold) || force) {
        reverseAPIKeys.append("agcPowerThreshold");
    }
    if ((m_settings.m_agcThresholdGate != settings.m_agcThresholdGate) || force) {
        reverseAPIKeys.append("agcThresholdGate");
    }
    if ((m_settings.m_agcClamping != settings.m_agcClamping) || force) {
        reverseAPIKeys.append("agcClamping");
    }

    if ((m_settings.m_agcTimeLog2 != settings.m_agcTimeLog2) ||
        (m_settings.m_agcPowerThreshold != settings.m_agcPowerThreshold) ||
        (m_settings.m_agcThresholdGate != settings.m_agcThresholdGate) ||
        (m_settings.m_agcClamping != settings.m_agcClamping) || force)
    {
        int agcNbSamples = (m_audioSampleRate / 1000) * (1<<settings.m_agcTimeLog2);
        m_agc.setThresholdEnable(settings.m_agcPowerThreshold != -FreeDVDemodSettings::m_minPowerThresholdDB);
        double agcPowerThreshold = CalcDb::powerFromdB(settings.m_agcPowerThreshold) * (SDR_RX_SCALED*SDR_RX_SCALED);
        int agcThresholdGate = (m_audioSampleRate / 1000) * settings.m_agcThresholdGate; // ms
        bool agcClamping = settings.m_agcClamping;

        if (m_agcNbSamples != agcNbSamples)
        {
            m_settingsMutex.lock();
            m_agc.resize(agcNbSamples, agcNbSamples/2, agcTarget);
            m_agc.setStepDownDelay(agcNbSamples);
            m_agcNbSamples = agcNbSamples;
            m_settingsMutex.unlock();
        }

        if (m_agcPowerThreshold != agcPowerThreshold)
        {
            m_agc.setThreshold(agcPowerThreshold);
            m_agcPowerThreshold = agcPowerThreshold;
        }

        if (m_agcThresholdGate != agcThresholdGate)
        {
            m_agc.setGate(agcThresholdGate);
            m_agcThresholdGate = agcThresholdGate;
        }

        if (m_agcClamping != agcClamping)
        {
            m_agc.setClamping(agcClamping);
            m_agcClamping = agcClamping;
        }

        qDebug() << "SBDemod::applySettings: AGC:"
            << " agcNbSamples: " << agcNbSamples
            << " agcPowerThreshold: " << agcPowerThreshold
            << " agcThresholdGate: " << agcThresholdGate
            << " agcClamping: " << agcClamping;
    }

    if ((settings.m_audioDeviceName != m_settings.m_audioDeviceName) || force)
    {
        reverseAPIKeys.append("audioDeviceName");
        AudioDeviceManager *audioDeviceManager = DSPEngine::instance()->getAudioDeviceManager();
        int audioDeviceIndex = audioDeviceManager->getOutputDeviceIndex(settings.m_audioDeviceName);
        audioDeviceManager->addAudioSink(&m_audioFifo, getInputMessageQueue(), audioDeviceIndex);
        uint32_t audioSampleRate = audioDeviceManager->getOutputSampleRate(audioDeviceIndex);

        if (m_audioSampleRate != audioSampleRate) {
            applyAudioSampleRate(audioSampleRate);
        }
    }

    if ((m_settings.m_spanLog2 != settings.m_spanLog2) || force) {
        reverseAPIKeys.append("spanLog2");
    }
    if ((m_settings.m_audioMute != settings.m_audioMute) || force) {
        reverseAPIKeys.append("audioMute");
    }
    if ((m_settings.m_agc != settings.m_agc) || force) {
        reverseAPIKeys.append("agc");
    }

    if ((settings.m_freeDVMode != m_settings.m_freeDVMode) || force) {
        applyFreeDVMode(settings.m_freeDVMode);
    }

    m_spanLog2 = settings.m_spanLog2;
    m_audioMute = settings.m_audioMute;
    m_agcActive = settings.m_agc;

    if (settings.m_useReverseAPI)
    {
        bool fullUpdate = ((m_settings.m_useReverseAPI != settings.m_useReverseAPI) && settings.m_useReverseAPI) ||
                (m_settings.m_reverseAPIAddress != settings.m_reverseAPIAddress) ||
                (m_settings.m_reverseAPIPort != settings.m_reverseAPIPort) ||
                (m_settings.m_reverseAPIDeviceIndex != settings.m_reverseAPIDeviceIndex) ||
                (m_settings.m_reverseAPIChannelIndex != settings.m_reverseAPIChannelIndex);
        webapiReverseSendSettings(reverseAPIKeys, settings, fullUpdate || force);
    }

    m_settings = settings;
}

QByteArray FreeDVDemod::serialize() const
{
    return m_settings.serialize();
}

bool FreeDVDemod::deserialize(const QByteArray& data)
{
    if (m_settings.deserialize(data))
    {
        MsgConfigureFreeDVDemod *msg = MsgConfigureFreeDVDemod::create(m_settings, true);
        m_inputMessageQueue.push(msg);
        return true;
    }
    else
    {
        m_settings.resetToDefaults();
        MsgConfigureFreeDVDemod *msg = MsgConfigureFreeDVDemod::create(m_settings, true);
        m_inputMessageQueue.push(msg);
        return false;
    }
}

int FreeDVDemod::webapiSettingsGet(
        SWGSDRangel::SWGChannelSettings& response,
        QString& errorMessage)
{
    (void) errorMessage;
    response.setFreeDvDemodSettings(new SWGSDRangel::SWGFreeDVDemodSettings());
    response.getFreeDvDemodSettings()->init();
    webapiFormatChannelSettings(response, m_settings);
    return 200;
}

int FreeDVDemod::webapiSettingsPutPatch(
        bool force,
        const QStringList& channelSettingsKeys,
        SWGSDRangel::SWGChannelSettings& response,
        QString& errorMessage)
{
    (void) errorMessage;
    FreeDVDemodSettings settings = m_settings;
    bool frequencyOffsetChanged = false;

    if (channelSettingsKeys.contains("inputFrequencyOffset"))
    {
        settings.m_inputFrequencyOffset = response.getFreeDvDemodSettings()->getInputFrequencyOffset();
        frequencyOffsetChanged = true;
    }
    if (channelSettingsKeys.contains("volume")) {
        settings.m_volume = response.getFreeDvDemodSettings()->getVolume();
    }
    if (channelSettingsKeys.contains("spanLog2")) {
        settings.m_spanLog2 = response.getFreeDvDemodSettings()->getSpanLog2();
    }
    if (channelSettingsKeys.contains("audioMute")) {
        settings.m_audioMute = response.getFreeDvDemodSettings()->getAudioMute() != 0;
    }
    if (channelSettingsKeys.contains("agc")) {
        settings.m_agc = response.getFreeDvDemodSettings()->getAgc() != 0;
    }
    if (channelSettingsKeys.contains("agcClamping")) {
        settings.m_agcClamping = response.getFreeDvDemodSettings()->getAgcClamping() != 0;
    }
    if (channelSettingsKeys.contains("agcTimeLog2")) {
        settings.m_agcTimeLog2 = response.getFreeDvDemodSettings()->getAgcTimeLog2();
    }
    if (channelSettingsKeys.contains("agcPowerThreshold")) {
        settings.m_agcPowerThreshold = response.getFreeDvDemodSettings()->getAgcPowerThreshold();
    }
    if (channelSettingsKeys.contains("agcThresholdGate")) {
        settings.m_agcThresholdGate = response.getFreeDvDemodSettings()->getAgcThresholdGate();
    }
    if (channelSettingsKeys.contains("rgbColor")) {
        settings.m_rgbColor = response.getFreeDvDemodSettings()->getRgbColor();
    }
    if (channelSettingsKeys.contains("title")) {
        settings.m_title = *response.getFreeDvDemodSettings()->getTitle();
    }
    if (channelSettingsKeys.contains("audioDeviceName")) {
        settings.m_audioDeviceName = *response.getFreeDvDemodSettings()->getAudioDeviceName();
    }

    if (frequencyOffsetChanged)
    {
        MsgConfigureChannelizer* channelConfigMsg = MsgConfigureChannelizer::create(
                m_audioSampleRate, settings.m_inputFrequencyOffset);
        m_inputMessageQueue.push(channelConfigMsg);
    }

    MsgConfigureFreeDVDemod *msg = MsgConfigureFreeDVDemod::create(settings, force);
    m_inputMessageQueue.push(msg);

    qDebug("FreeDVDemod::webapiSettingsPutPatch: forward to GUI: %p", m_guiMessageQueue);
    if (m_guiMessageQueue) // forward to GUI if any
    {
        MsgConfigureFreeDVDemod *msgToGUI = MsgConfigureFreeDVDemod::create(settings, force);
        m_guiMessageQueue->push(msgToGUI);
    }

    webapiFormatChannelSettings(response, settings);

    return 200;
}

int FreeDVDemod::webapiReportGet(
        SWGSDRangel::SWGChannelReport& response,
        QString& errorMessage)
{
    (void) errorMessage;
    response.setFreeDvDemodReport(new SWGSDRangel::SWGFreeDVDemodReport());
    response.getFreeDvDemodReport()->init();
    webapiFormatChannelReport(response);
    return 200;
}

void FreeDVDemod::webapiFormatChannelSettings(SWGSDRangel::SWGChannelSettings& response, const FreeDVDemodSettings& settings)
{
    response.getFreeDvDemodSettings()->setAudioMute(settings.m_audioMute ? 1 : 0);
    response.getFreeDvDemodSettings()->setInputFrequencyOffset(settings.m_inputFrequencyOffset);
    response.getFreeDvDemodSettings()->setVolume(settings.m_volume);
    response.getFreeDvDemodSettings()->setSpanLog2(settings.m_spanLog2);
    response.getFreeDvDemodSettings()->setAudioMute(settings.m_audioMute ? 1 : 0);
    response.getFreeDvDemodSettings()->setAgc(settings.m_agc ? 1 : 0);
    response.getFreeDvDemodSettings()->setAgcClamping(settings.m_agcClamping ? 1 : 0);
    response.getFreeDvDemodSettings()->setAgcTimeLog2(settings.m_agcTimeLog2);
    response.getFreeDvDemodSettings()->setAgcPowerThreshold(settings.m_agcPowerThreshold);
    response.getFreeDvDemodSettings()->setAgcThresholdGate(settings.m_agcThresholdGate);
    response.getFreeDvDemodSettings()->setRgbColor(settings.m_rgbColor);

    if (response.getFreeDvDemodSettings()->getTitle()) {
        *response.getFreeDvDemodSettings()->getTitle() = settings.m_title;
    } else {
        response.getFreeDvDemodSettings()->setTitle(new QString(settings.m_title));
    }

    if (response.getFreeDvDemodSettings()->getAudioDeviceName()) {
        *response.getFreeDvDemodSettings()->getAudioDeviceName() = settings.m_audioDeviceName;
    } else {
        response.getFreeDvDemodSettings()->setAudioDeviceName(new QString(settings.m_audioDeviceName));
    }
}

void FreeDVDemod::webapiFormatChannelReport(SWGSDRangel::SWGChannelReport& response)
{
    double magsqAvg, magsqPeak;
    int nbMagsqSamples;
    getMagSqLevels(magsqAvg, magsqPeak, nbMagsqSamples);

    response.getFreeDvDemodReport()->setChannelPowerDb(CalcDb::dbPower(magsqAvg));
    response.getFreeDvDemodReport()->setSquelch(m_audioActive ? 1 : 0);
    response.getFreeDvDemodReport()->setAudioSampleRate(m_audioSampleRate);
    response.getFreeDvDemodReport()->setChannelSampleRate(m_inputSampleRate);
}

void FreeDVDemod::webapiReverseSendSettings(QList<QString>& channelSettingsKeys, const FreeDVDemodSettings& settings, bool force)
{
    SWGSDRangel::SWGChannelSettings *swgChannelSettings = new SWGSDRangel::SWGChannelSettings();
    swgChannelSettings->setTx(0);
    swgChannelSettings->setChannelType(new QString("SSBDemod"));
    swgChannelSettings->setFreeDvDemodSettings(new SWGSDRangel::SWGFreeDVDemodSettings());
    SWGSDRangel::SWGFreeDVDemodSettings *swgFreeDVDemodSettings = swgChannelSettings->getFreeDvDemodSettings();

    // transfer data that has been modified. When force is on transfer all data except reverse API data

    if (channelSettingsKeys.contains("inputFrequencyOffset") || force) {
        swgFreeDVDemodSettings->setInputFrequencyOffset(settings.m_inputFrequencyOffset);
    }
    if (channelSettingsKeys.contains("volume") || force) {
        swgFreeDVDemodSettings->setVolume(settings.m_volume);
    }
    if (channelSettingsKeys.contains("spanLog2") || force) {
        swgFreeDVDemodSettings->setSpanLog2(settings.m_spanLog2);
    }
    if (channelSettingsKeys.contains("audioMute") || force) {
        swgFreeDVDemodSettings->setAudioMute(settings.m_audioMute ? 1 : 0);
    }
    if (channelSettingsKeys.contains("agc") || force) {
        swgFreeDVDemodSettings->setAgc(settings.m_agc ? 1 : 0);
    }
    if (channelSettingsKeys.contains("agcClamping") || force) {
        swgFreeDVDemodSettings->setAgcClamping(settings.m_agcClamping ? 1 : 0);
    }
    if (channelSettingsKeys.contains("agcTimeLog2") || force) {
        swgFreeDVDemodSettings->setAgcTimeLog2(settings.m_agcTimeLog2);
    }
    if (channelSettingsKeys.contains("agcPowerThreshold") || force) {
        swgFreeDVDemodSettings->setAgcPowerThreshold(settings.m_agcPowerThreshold);
    }
    if (channelSettingsKeys.contains("agcThresholdGate") || force) {
        swgFreeDVDemodSettings->setAgcThresholdGate(settings.m_agcThresholdGate);
    }
    if (channelSettingsKeys.contains("rgbColor") || force) {
        swgFreeDVDemodSettings->setRgbColor(settings.m_rgbColor);
    }
    if (channelSettingsKeys.contains("title") || force) {
        swgFreeDVDemodSettings->setTitle(new QString(settings.m_title));
    }
    if (channelSettingsKeys.contains("audioDeviceName") || force) {
        swgFreeDVDemodSettings->setAudioDeviceName(new QString(settings.m_audioDeviceName));
    }

    QString channelSettingsURL = QString("http://%1:%2/sdrangel/deviceset/%3/channel/%4/settings")
            .arg(settings.m_reverseAPIAddress)
            .arg(settings.m_reverseAPIPort)
            .arg(settings.m_reverseAPIDeviceIndex)
            .arg(settings.m_reverseAPIChannelIndex);
    m_networkRequest.setUrl(QUrl(channelSettingsURL));
    m_networkRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QBuffer *buffer=new QBuffer();
    buffer->open((QBuffer::ReadWrite));
    buffer->write(swgChannelSettings->asJson().toUtf8());
    buffer->seek(0);

    // Always use PATCH to avoid passing reverse API settings
    m_networkManager->sendCustomRequest(m_networkRequest, "PATCH", buffer);

    delete swgChannelSettings;
}

void FreeDVDemod::networkManagerFinished(QNetworkReply *reply)
{
    QNetworkReply::NetworkError replyError = reply->error();

    if (replyError)
    {
        qWarning() << "FreeDVDemod::networkManagerFinished:"
                << " error(" << (int) replyError
                << "): " << replyError
                << ": " << reply->errorString();
        return;
    }

    QString answer = reply->readAll();
    answer.chop(1); // remove last \n
    qDebug("FreeDVDemod::networkManagerFinished: reply:\n%s", answer.toStdString().c_str());
}