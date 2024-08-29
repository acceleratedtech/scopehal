/***********************************************************************************************************************
*                                                                                                                      *
* libscopehal                                                                                                          *
*                                                                                                                      *
* Copyright (c) 2012-2024 Andrew D. Zonenberg and contributors                                                         *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/
/**
	@file
	@author Andrew D. Zonenberg
	@brief Implementation of TestWaveformSource
 */
#include "scopehal.h"
#include "TestWaveformSource.h"
#include <complex>

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

TestWaveformSource::TestWaveformSource(minstd_rand& rng)
	: m_rng(rng)
	, m_rectangularComputePipeline("shaders/RectangularWindow.spv", 2, sizeof(WindowFunctionArgs))
{
#ifndef _APPLE_SILICON
	m_forwardPlan = NULL;
	m_reversePlan = NULL;

	m_cachedNumPoints = 0;
	m_cachedRawSize = 0;
#endif

	TouchstoneParser sxp;
	sxp.Load(FindDataFile("channels/300mm-s2000m.s2p"), m_sparams);

	m_forwardInBuf.SetCpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
	m_forwardInBuf.SetGpuAccessHint(AcceleratorBuffer<float>::HINT_LIKELY);
}

TestWaveformSource::~TestWaveformSource()
{
#ifndef _APPLE_SILICON
	if(m_forwardPlan)
		ffts_free(m_forwardPlan);
	if(m_reversePlan)
		ffts_free(m_reversePlan);

	m_forwardPlan = NULL;
	m_reversePlan = NULL;
#endif
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Signal generation

/**
	@brief Generates a unit step
 */
WaveformBase* TestWaveformSource::GenerateStep(
	float vlo,
	float vhi,
	int64_t sampleperiod,
	size_t depth)
{
	auto ret = new UniformAnalogWaveform("Step");
	ret->m_timescale = sampleperiod;
	ret->Resize(depth);

	size_t mid = depth/2;
	for(size_t i=0; i<depth; i++)
	{
		if(i < mid)
			ret->m_samples[i] = vlo;
		else
			ret->m_samples[i] = vhi;
	}

	return ret;
}

/**
	@brief Generates a sinewave with a bit of extra noise added
 */
WaveformBase* TestWaveformSource::GenerateNoisySinewave(
	float amplitude,
	float startphase,
	float period,
	int64_t sampleperiod,
	size_t depth,
	float noise_amplitude)
{
	auto ret = new UniformAnalogWaveform("NoisySine");
	ret->m_timescale = sampleperiod;
	ret->Resize(depth);

	normal_distribution<> noise(0, noise_amplitude);

	float samples_per_cycle = period * 1.0 / sampleperiod;
	float radians_per_sample = 2 * M_PI / samples_per_cycle;

	//sin is +/- 1, so need to divide amplitude by 2 to get scaling factor
	float scale = amplitude / 2;

	for(size_t i=0; i<depth; i++)
		ret->m_samples[i] = scale * sinf(i*radians_per_sample + startphase) + noise(m_rng);

	return ret;
}

/**
	@brief Generates a mix of two sinewaves plus some noise
 */
WaveformBase* TestWaveformSource::GenerateNoisySinewaveMix(
	float amplitude,
	float startphase1,
	float startphase2,
	float period1,
	float period2,
	int64_t sampleperiod,
	size_t depth,
	float noise_amplitude)
{
	auto ret = new UniformAnalogWaveform("NoisySineMix");
	ret->m_timescale = sampleperiod;
	ret->Resize(depth);

	normal_distribution<> noise(0, noise_amplitude);

	float radians_per_sample1 = 2 * M_PI * sampleperiod / period1;
	float radians_per_sample2 = 2 * M_PI * sampleperiod / period2;

	//sin is +/- 1, so need to divide amplitude by 2 to get scaling factor.
	//Divide by 2 again to avoid clipping the sum of them
	float scale = amplitude / 4;

	for(size_t i=0; i<depth; i++)
	{
		ret->m_samples[i] = scale *
			(sinf(i*radians_per_sample1 + startphase1) + sinf(i*radians_per_sample2 + startphase2))
			+ noise(m_rng);
	}

	return ret;
}

WaveformBase* TestWaveformSource::GeneratePRBS31(
	vk::raii::CommandBuffer& cmdBuf,
	shared_ptr<QueueHandle> queue,
	float amplitude,
	float period,
	int64_t sampleperiod,
	size_t depth,
	bool lpf,
	float noise_amplitude
	)
{
	auto ret = new UniformAnalogWaveform("PRBS31");
	ret->m_timescale = sampleperiod;
	ret->Resize(depth);

	//Generate the PRBS as a square wave. Interpolate zero crossings as needed.
	uint32_t prbs = rand();
	float scale = amplitude / 2;
	float phase_to_next_edge = period;
	bool value = false;
	for(size_t i=0; i<depth; i++)
	{
		//Increment phase accumulator
		float last_phase = phase_to_next_edge;
		phase_to_next_edge -= sampleperiod;

		bool last = value;
		if(phase_to_next_edge < 0)
		{
			uint32_t next = ( (prbs >> 30) ^ (prbs >> 27) ) & 1;
			prbs = (prbs << 1) | next;
			value = next;

			phase_to_next_edge += period;
		}

		//Not an edge, just repeat the value
		if(last == value)
			ret->m_samples[i] = value ? scale : -scale;

		//Edge - interpolate
		else
		{
			float last_voltage = last ? scale : -scale;
			float cur_voltage = value ? scale : -scale;

			float frac = 1 - (last_phase / sampleperiod);
			float delta = cur_voltage - last_voltage;

			ret->m_samples[i] = last_voltage + delta*frac;
		}
	}

	DegradeSerialData(ret, sampleperiod, depth, lpf, noise_amplitude, cmdBuf, queue);

	return ret;
}

WaveformBase* TestWaveformSource::Generate8b10b(
	vk::raii::CommandBuffer& cmdBuf,
	shared_ptr<QueueHandle> queue,
	float amplitude,
	float period,
	int64_t sampleperiod,
	size_t depth,
	bool lpf,
	float noise_amplitude)
{
	auto ret = new UniformAnalogWaveform("8B10B");
	ret->m_timescale = sampleperiod;
	ret->Resize(depth);

	const int patternlen = 20;
	const bool pattern[patternlen] =
	{
		0, 0, 1, 1, 1, 1, 1, 0, 1, 0,		//K28.5
		1, 0, 0, 1, 0, 0, 0, 1, 0, 1		//D16.2
	};

	//Generate the data stream as a square wave. Interpolate zero crossings as needed.
	float scale = amplitude / 2;
	float phase_to_next_edge = period;
	bool value = false;
	int nbit = 0;
	for(size_t i=0; i<depth; i++)
	{
		//Increment phase accumulator
		float last_phase = phase_to_next_edge;
		phase_to_next_edge -= sampleperiod;

		bool last = value;
		if(phase_to_next_edge < 0)
		{
			value = pattern[nbit ++];
			if(nbit >= patternlen)
				nbit = 0;

			phase_to_next_edge += period;
		}

		//Not an edge, just repeat the value
		if(last == value)
			ret->m_samples[i] = value ? scale : -scale;

		//Edge - interpolate
		else
		{
			float last_voltage = last ? scale : -scale;
			float cur_voltage = value ? scale : -scale;

			float frac = 1 - (last_phase / sampleperiod);
			float delta = cur_voltage - last_voltage;

			ret->m_samples[i] = last_voltage + delta*frac;
		}
	}

	DegradeSerialData(ret, sampleperiod, depth, lpf, noise_amplitude, cmdBuf, queue);

	return ret;
}

/**
	@brief Takes an idealized serial data stream and turns it into something less pretty

	by adding noise and a band-limiting filter
 */
void TestWaveformSource::DegradeSerialData(
	UniformAnalogWaveform* cap,
	int64_t sampleperiod,
	size_t depth,
	bool lpf,
	float noise_amplitude,
	vk::raii::CommandBuffer& cmdBuf,
	shared_ptr<QueueHandle> queue)
{
	//assume input came from CPU
	cap->MarkModifiedFromCpu();

	//RNGs
	normal_distribution<> noise(0, noise_amplitude);

	// ffts is not available on apple silicon, so for now we only apply noise there
#ifndef _APPLE_SILICON
	//Prepare for second pass: reallocate FFT buffer if sample depth changed
	const size_t npoints = next_pow2(depth);
	size_t nouts = npoints/2 + 1;
	if(m_cachedNumPoints != npoints)
	{
		if(m_forwardPlan)
			ffts_free(m_forwardPlan);
		m_forwardPlan = ffts_init_1d_real(npoints, FFTS_FORWARD);

		if(m_reversePlan)
			ffts_free(m_reversePlan);
		m_reversePlan = ffts_init_1d_real(npoints, FFTS_BACKWARD);

		m_forwardInBuf.resize(npoints);
		m_forwardOutBuf.resize(2*nouts);
		m_reverseOutBuf.resize(npoints);

		m_cachedNumPoints = npoints;
	}

	if(lpf)
	{
		//Prepare to do all of our compute stuff in one dispatch call to reduce overhead
		cmdBuf.begin({});

		//Copy and zero-pad the input as needed
		WindowFunctionArgs args;
		args.numActualSamples = depth;
		args.npoints = npoints;
		args.scale = 0;
		args.alpha0 = 0;
		args.alpha1 = 0;
		args.offsetIn = 0;
		args.offsetOut = 0;
		m_rectangularComputePipeline.BindBufferNonblocking(0, cap->m_samples, cmdBuf);
		m_rectangularComputePipeline.BindBufferNonblocking(1, m_forwardInBuf, cmdBuf, true);
		m_rectangularComputePipeline.Dispatch(cmdBuf, args, GetComputeBlockCount(npoints, 64));
		m_rectangularComputePipeline.AddComputeMemoryBarrier(cmdBuf);
		m_forwardInBuf.MarkModifiedFromGpu();

		//Done, block until the compute operations finish
		cmdBuf.end();
		queue->SubmitAndBlock(cmdBuf);
		//cap->MarkModifiedFromGpu();

		//Pull the input buffer out to do a software FFT
		m_forwardInBuf.PrepareForCpuAccess();

		//Do the forward FFT
		ffts_execute(m_forwardPlan, m_forwardInBuf.GetCpuPointer(), &m_forwardOutBuf[0]);

		auto& s21 = m_sparams[SPair(2, 1)];

		//Calculate the group delay of the channel at the middle frequency bin
		int64_t groupDelay = s21.GetGroupDelay(s21.size() / 2) * FS_PER_SECOND;
		int64_t groupDelaySamples = groupDelay / cap->m_timescale;

		//Apply the channel
		double sample_ghz = 1e6 / sampleperiod;
		double bin_hz = round((0.5f * sample_ghz * 1e9f) / nouts);
		for(size_t i = 0; i<nouts; i++)
		{
			float freq = bin_hz * i;
			auto pt = s21.InterpolatePoint(freq);
			float mag = pt.m_amplitude;
			float ang = pt.m_phase;

			float sinval = sin(ang) * mag;
			float cosval = cos(ang) * mag;

			auto real_orig = m_forwardOutBuf[i*2];
			auto imag_orig = m_forwardOutBuf[i*2 + 1];

			m_forwardOutBuf[i*2] = real_orig * cosval - imag_orig * sinval;
			m_forwardOutBuf[i*2 + 1] = real_orig * sinval + imag_orig * cosval;
		}

		//Calculate the inverse FFT
		ffts_execute(m_reversePlan, &m_forwardOutBuf[0], &m_reverseOutBuf[0]);

		//Calculate the actual start and end of the samples, accounting for garbage at the beginning of the channel
		size_t istart = groupDelaySamples;
		size_t iend = depth;
		size_t finalLen = iend - istart;

		//Rescale the FFT output and copy to the output, then add noise
		float fftscale = 1.0f / npoints;
		for(size_t i=0; i<finalLen; i++)
			cap->m_samples[i] = m_reverseOutBuf[i + istart] * fftscale + noise(m_rng);

		//Resize the waveform to truncate garbage at the end
		cap->Resize(finalLen);
	}

	else
#endif
	{
		for(size_t i=0; i<depth; i++)
			cap->m_samples[i] += noise(m_rng);
	}
}
