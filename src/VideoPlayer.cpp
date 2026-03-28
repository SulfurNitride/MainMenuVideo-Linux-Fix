#include "VideoPlayer.h"

#include "Manager.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

ImGui::Texture::Texture(ID3D11Device* device, std::uint32_t a_width, std::uint32_t a_height)
{
	D3D11_TEXTURE2D_DESC desc{
		.Width = a_width,
		.Height = a_height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
		.SampleDesc = { 1, 0 },
		.Usage = D3D11_USAGE_DYNAMIC,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE,
		.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
		.MiscFlags = 0
	};

	if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture)) ||
		FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &srView))) {
		texture.Reset();
		srView.Reset();
		return;
	}
}

void ImGui::Texture::Update(ID3D11DeviceContext* context, const cv::Mat& mat) const
{
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (SUCCEEDED(context->Map(texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		constexpr std::uint32_t bytesPerPixel = 4;  // BGRA
		const auto              srcRowBytes = mat.cols * bytesPerPixel;

		if (mapped.RowPitch == srcRowBytes) {
			std::memcpy(mapped.pData, mat.data, mat.rows * srcRowBytes);
		} else {
			auto* dst = static_cast<std::uint8_t*>(mapped.pData);
			for (std::int32_t y = 0; y < mat.rows; ++y) {
				std::memcpy(dst + y * mapped.RowPitch, mat.ptr<uchar>(y), srcRowBytes);
			}
		}
		context->Unmap(texture.Get(), 0);
	}
}

// FFmpeg audio decoding + XAudio2 output
// No Windows Media Foundation dependency — works natively on both Windows and Proton
bool VideoPlayer::LoadAudio(const std::string& path)
{
	// Open file with FFmpeg
	if (avformat_open_input(&audioFmtCtx, path.c_str(), nullptr, nullptr) < 0) {
		logger::warn("FFmpeg: failed to open {}", path);
		ResetAudio();
		return false;
	}

	if (avformat_find_stream_info(audioFmtCtx, nullptr) < 0) {
		logger::warn("FFmpeg: failed to find stream info");
		ResetAudio();
		return false;
	}

	// Find best audio stream
	audioStreamIdx = av_find_best_stream(audioFmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	if (audioStreamIdx < 0) {
		logger::warn("FFmpeg: no audio stream found in {}", path);
		ResetAudio();
		return false;
	}

	// Open audio decoder
	const auto* stream = audioFmtCtx->streams[audioStreamIdx];
	const auto* codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!codec) {
		logger::warn("FFmpeg: unsupported audio codec");
		ResetAudio();
		return false;
	}

	audioDecCtx = avcodec_alloc_context3(codec);
	if (!audioDecCtx) {
		ResetAudio();
		return false;
	}

	avcodec_parameters_to_context(audioDecCtx, stream->codecpar);
	if (avcodec_open2(audioDecCtx, codec, nullptr) < 0) {
		logger::warn("FFmpeg: failed to open audio decoder");
		ResetAudio();
		return false;
	}

	// Set up resampler: decode to S16 interleaved PCM for XAudio2
	const int outChannels = audioDecCtx->ch_layout.nb_channels;
	const int outSampleRate = audioDecCtx->sample_rate;

	AVChannelLayout outLayout{};
	av_channel_layout_default(&outLayout, outChannels);

	if (swr_alloc_set_opts2(&audioSwrCtx,
			&outLayout, AV_SAMPLE_FMT_S16, outSampleRate,
			&audioDecCtx->ch_layout, audioDecCtx->sample_fmt, outSampleRate,
			0, nullptr) < 0) {
		logger::warn("FFmpeg: failed to configure resampler");
		av_channel_layout_uninit(&outLayout);
		ResetAudio();
		return false;
	}
	av_channel_layout_uninit(&outLayout);

	if (swr_init(audioSwrCtx) < 0) {
		logger::warn("FFmpeg: failed to init resampler");
		ResetAudio();
		return false;
	}

	// Fill WAVEFORMATEX for XAudio2
	audioFormat = {};
	audioFormat.wFormatTag = WAVE_FORMAT_PCM;
	audioFormat.nChannels = static_cast<WORD>(outChannels);
	audioFormat.nSamplesPerSec = static_cast<DWORD>(outSampleRate);
	audioFormat.wBitsPerSample = 16;
	audioFormat.nBlockAlign = audioFormat.nChannels * audioFormat.wBitsPerSample / 8;
	audioFormat.nAvgBytesPerSec = audioFormat.nSamplesPerSec * audioFormat.nBlockAlign;
	audioFormat.cbSize = 0;

	// Create XAudio2 engine (uses FAudio on Proton)
	HRESULT hr = XAudio2Create(&xaudio2);
	if (FAILED(hr)) {
		logger::warn("Failed to create XAudio2 engine: 0x{:X}", static_cast<std::uint32_t>(hr));
		ResetAudio();
		return false;
	}

	hr = xaudio2->CreateMasteringVoice(&masterVoice);
	if (FAILED(hr)) {
		logger::warn("Failed to create XAudio2 mastering voice: 0x{:X}", static_cast<std::uint32_t>(hr));
		ResetAudio();
		return false;
	}

	hr = xaudio2->CreateSourceVoice(&sourceVoice, &audioFormat);
	if (FAILED(hr)) {
		logger::warn("Failed to create XAudio2 source voice: 0x{:X}", static_cast<std::uint32_t>(hr));
		ResetAudio();
		return false;
	}

	sourceVoice->SetVolume(volume.load(std::memory_order_relaxed));

	logger::info("Audio loaded via FFmpeg+XAudio2: {} {} {}ch {}Hz",
		codec->name, av_get_sample_fmt_name(audioDecCtx->sample_fmt),
		outChannels, outSampleRate);

	return true;
}

void VideoPlayer::CreateVideoThread()
{
	if (videoThread.joinable()) {
		return;
	}

	videoThread = std::jthread([this](std::stop_token st) {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

		if (audioLoaded.load(std::memory_order_relaxed)) {
			startBarrier.arrive_and_wait();  // wait until both video+audio are ready
		}

		playbackState.store(PLAYBACK_STATE::kPlaying, std::memory_order_release);

		time_point frameStartTime = clock::now();
		time_point playbackStart = frameStartTime;
		time_point debugUpdateInfoTime = frameStartTime;

		cv::Mat frame;
		cv::Mat processedFrame;

		auto restart_loop = [&]() {
			readFrameCount.store(0, std::memory_order_relaxed);
			cap.release();
			cap.open(currentVideo, cv::CAP_MSMF, { cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY });
			RestartAudioThread();
			if (audioLoaded.load(std::memory_order_relaxed)) {
				startBarrier.arrive_and_wait();
			}
			// Reset timing for new loop
			frameStartTime = clock::now();
			playbackStart = frameStartTime;
			debugUpdateInfoTime = frameStartTime;
		};

		while (!st.stop_requested()) {
			const auto now = clock::now();
			const auto elapsed = now - frameStartTime;

			if (elapsed < frameDuration) {
				const auto sleepDuration = frameDuration - elapsed;
				if (sleepDuration > std::chrono::milliseconds(0)) {
					std::this_thread::sleep_for(sleepDuration);
				}
				continue;
			}

			if (!cap.read(frame) || frame.empty()) {
				switch (playbackMode) {
				case PLAYBACK_MODE::kPlayOnce:
					Reset();
					return;
				case PLAYBACK_MODE::kPlayNext:
					Reset(true);
					return;
				case PLAYBACK_MODE::kLoop:
					restart_loop();
					continue;
				default:
					std::unreachable();
				}
			}

			const auto channels = frame.channels();
			if (channels == 3) {
				cv::cvtColor(frame, processedFrame, cv::COLOR_BGR2BGRA);
			} else if (channels == 4) {
				processedFrame = frame;
			} else {
				frameStartTime += frameDuration;
				continue;
			}

			{
				WriteLocker lock(videoFrameLock);
				cv::swap(processedFrame, videoFrame);
			}

			readFrameCount.fetch_add(1, std::memory_order_relaxed);
			frameStartTime += frameDuration;

			if (now - debugUpdateInfoTime >= debugUpdateInterval) {
				const auto totalElapsed = duration(now - playbackStart).count();
				const auto frameCount = readFrameCount.load(std::memory_order_relaxed);
				elapsedTime.store(static_cast<float>(totalElapsed), std::memory_order_relaxed);
				actualFPS.store(static_cast<float>(frameCount / totalElapsed), std::memory_order_relaxed);
				debugUpdateInfoTime = now;
			}
		}
	});
}

void VideoPlayer::Update(ID3D11DeviceContext* context)
{
	if (!texture) {
		return;
	}

	cv::Mat localFrame;
	{
		ReadLocker lock(videoFrameLock);
		if (videoFrame.empty()) {
			return;
		}
		localFrame = videoFrame;
	}

	texture->Update(context, localFrame);
}

void VideoPlayer::CreateAudioThread()
{
	if (!audioLoaded.load(std::memory_order_relaxed) || audioThread.joinable()) {
		return;
	}

	audioThread = std::jthread([this](std::stop_token st) {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

		startBarrier.arrive_and_wait();
		sourceVoice->Start();

		constexpr UINT32 MAX_QUEUED_BUFFERS = 4;

		// Ring buffer pool — each slot stays alive until XAudio2 consumes it
		std::vector<std::vector<BYTE>> bufferPool(MAX_QUEUED_BUFFERS);
		UINT32                         currentBuffer = 0;

		AVPacket* packet = av_packet_alloc();
		AVFrame*  frame = av_frame_alloc();

		while (!st.stop_requested()) {
			// Throttle: wait if XAudio2 has enough queued data
			XAUDIO2_VOICE_STATE voiceState;
			sourceVoice->GetState(&voiceState);
			if (voiceState.BuffersQueued >= MAX_QUEUED_BUFFERS) {
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				continue;
			}

			// Read next packet from container
			int ret = av_read_frame(audioFmtCtx, packet);
			if (ret < 0) {
				break;  // EOF or error
			}

			// Skip non-audio packets (video, subtitle, etc.)
			if (packet->stream_index != audioStreamIdx) {
				av_packet_unref(packet);
				continue;
			}

			// Send packet to decoder
			ret = avcodec_send_packet(audioDecCtx, packet);
			av_packet_unref(packet);
			if (ret < 0) {
				continue;
			}

			// Receive decoded frames and resample to S16 PCM
			while (avcodec_receive_frame(audioDecCtx, frame) == 0) {
				if (st.stop_requested()) {
					av_frame_unref(frame);
					break;
				}

				const int outSamples = swr_get_out_samples(audioSwrCtx, frame->nb_samples);
				auto&     buf = bufferPool[currentBuffer % MAX_QUEUED_BUFFERS];
				buf.resize(static_cast<size_t>(outSamples) * audioFormat.nBlockAlign);

				uint8_t*  outPtr = buf.data();
				const int converted = swr_convert(audioSwrCtx,
					&outPtr, outSamples,
					const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);

				av_frame_unref(frame);

				if (converted <= 0) {
					continue;
				}

				// Wait for buffer slot if needed
				do {
					sourceVoice->GetState(&voiceState);
					if (voiceState.BuffersQueued >= MAX_QUEUED_BUFFERS) {
						std::this_thread::sleep_for(std::chrono::milliseconds(5));
					}
				} while (voiceState.BuffersQueued >= MAX_QUEUED_BUFFERS && !st.stop_requested());

				XAUDIO2_BUFFER xbuf{};
				xbuf.AudioBytes = static_cast<UINT32>(converted) * audioFormat.nBlockAlign;
				xbuf.pAudioData = buf.data();

				sourceVoice->SubmitSourceBuffer(&xbuf);
				currentBuffer++;
			}
		}

		av_frame_free(&frame);
		av_packet_free(&packet);

		// Drain remaining buffers before exiting (unless stop requested)
		if (!st.stop_requested()) {
			XAUDIO2_VOICE_STATE voiceState;
			do {
				sourceVoice->GetState(&voiceState);
				if (voiceState.BuffersQueued > 0) {
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
			} while (voiceState.BuffersQueued > 0 && !st.stop_requested());
		}
	});
}

void VideoPlayer::RestartAudioThread()
{
	if (audioLoaded) {
		audioThread = {};
		ResetAudio();
		audioLoaded.store(LoadAudio(currentVideo), std::memory_order_relaxed);
		CreateAudioThread();
	}
}

bool VideoPlayer::LoadVideo(ID3D11Device* device, const std::string& path, bool playAudio)
{
	cap.open(path, cv::CAP_MSMF, { cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY });
	if (!cap.isOpened()) {
		currentVideo.clear();
		logger::warn("Couldn't load {}", path);
		return false;
	}

	currentVideo = path;

	videoWidth = static_cast<std::uint32_t>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
	videoHeight = static_cast<std::uint32_t>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
	frameCount = static_cast<std::uint32_t>(cap.get(cv::CAP_PROP_FRAME_COUNT));
	targetFPS = static_cast<float>(cap.get(cv::CAP_PROP_FPS));
	frameDuration = targetFPS > 0.0f ? duration(1.0f / targetFPS) : duration(0.0333);

	logger::info("Loading {} ({}x{}|{} FPS|{} frames)", path, videoWidth, videoHeight, targetFPS, frameCount);

	const auto screenSize = RE::BSGraphics::Renderer::GetScreenSize();
	if (screenSize.width != videoWidth || screenSize.height != videoHeight) {
		const float scaleX = static_cast<float>(screenSize.width) / videoWidth;
		const float scaleY = static_cast<float>(screenSize.height) / videoHeight;
		const float scale = std::min(scaleX, scaleY);

		auto displayWidth = static_cast<std::uint32_t>(videoWidth * scale);
		auto displayHeight = static_cast<std::uint32_t>(videoHeight * scale);
		logger::info("\tScaling to fit screen ({}x{} -> {}x{} ({:.2f}X))", videoWidth, videoHeight, displayWidth, displayHeight, scale);
		displaySize = { static_cast<float>(displayWidth), static_cast<float>(displayHeight) };
	} else {
		displaySize = { static_cast<float>(videoWidth), static_cast<float>(videoHeight) };
	}

	texture = std::make_unique<ImGui::Texture>(device, videoWidth, videoHeight);
	if (!texture || !texture->texture || !texture->srView) {
		cap.release();
		return false;
	}

	audioLoaded.store(playAudio ? LoadAudio(path) : false, std::memory_order_relaxed);

	CreateAudioThread();
	CreateVideoThread();

	return true;
}

void VideoPlayer::ResetAudio()
{
	if (audioSwrCtx) {
		swr_free(&audioSwrCtx);
	}
	if (audioDecCtx) {
		avcodec_free_context(&audioDecCtx);
	}
	if (audioFmtCtx) {
		avformat_close_input(&audioFmtCtx);
	}
	audioStreamIdx = -1;

	if (sourceVoice) {
		sourceVoice->Stop();
		sourceVoice->FlushSourceBuffers();
		sourceVoice->DestroyVoice();
		sourceVoice = nullptr;
	}
	if (masterVoice) {
		masterVoice->DestroyVoice();
		masterVoice = nullptr;
	}
	xaudio2 = nullptr;
}

void VideoPlayer::ResetImpl(bool playNextVideo)
{
	if (videoThread.joinable()) {
		videoThread.request_stop();
		videoThread.join();
	}
	if (audioThread.joinable()) {
		audioThread.request_stop();
		audioThread.join();
	}

	readFrameCount.store(0, std ::memory_order_relaxed);
	elapsedTime.store(0, std::memory_order_relaxed);

	{
		WriteLocker lock(videoFrameLock);
		videoFrame.release();
	}

	if (audioLoaded.load(std::memory_order_relaxed)) {
		ResetAudio();
		if (!playNextVideo) {
			audioLoaded.store(false, std::memory_order_relaxed);
		}
	}

	if (!playNextVideo) {
		texture.reset();
	}
	cap.release();

	if (playNextVideo) {
		if (!Manager::GetSingleton()->LoadNextVideo()) {
			playbackState.store(PLAYBACK_STATE::kIdle, std::memory_order_release);
		}
	} else {
		playbackState.store(PLAYBACK_STATE::kIdle, std::memory_order_release);
	}
}

void VideoPlayer::Reset(bool playNextVideo)
{
	auto expected = PLAYBACK_STATE::kPlaying;
	auto desired = playNextVideo ? PLAYBACK_STATE::kTransitioning : PLAYBACK_STATE::kStopping;

	if (!playbackState.compare_exchange_strong(expected, desired,
			std::memory_order_acq_rel,
			std::memory_order_acquire)) {
		return;
	}

	resetThread = std::jthread([this, playNextVideo](std::stop_token) {
		ResetImpl(playNextVideo);
	});
	resetThread.detach();
}

ImTextureID VideoPlayer::GetTextureID() const
{
	return texture ? (ImTextureID)texture->srView.Get() : 0;
}

ImVec2 VideoPlayer::GetNativeSize() const
{
	return texture ? displaySize : ImGui::GetIO().DisplaySize;
}

bool VideoPlayer::IsInitialized() const
{
	return texture && texture->srView;
}

bool VideoPlayer::IsPlaying() const
{
	auto state = playbackState.load(std::memory_order_acquire);
	return state == PLAYBACK_STATE::kPlaying || state == PLAYBACK_STATE::kTransitioning;
}

bool VideoPlayer::IsTransitioning() const
{
	return playbackState.load(std::memory_order_acquire) == PLAYBACK_STATE::kTransitioning;
}

bool VideoPlayer::IsPlayingAudio() const
{
	return IsPlaying() && audioLoaded.load(std::memory_order_relaxed);
}

void VideoPlayer::ShowDebugInfo()
{
	auto min = ImGui::GetItemRectMin();
	ImGui::SetCursorScreenPos(min);

	if (IsTransitioning()) {
		ImGui::Text("TRANSITIONING");
		return;
	}

	ImGui::Text("%s", currentVideo.c_str());
	ImGui::Text("\tElapsed Time: %.1f seconds", elapsedTime.load(std::memory_order_relaxed));
	ImGui::Text("\tFrames Processed: %u/%u", readFrameCount.load(std::memory_order_relaxed), frameCount);
	ImGui::Text("\tTarget FPS: %.1f", targetFPS);
	ImGui::Text("\tActual FPS: %.1f", actualFPS.load(std::memory_order_relaxed));
	ImGui::Text("\tVolume: %.0f%%", volume.load(std::memory_order_relaxed) * 100.0f);
}

void VideoPlayer::OnVolumeUpdate()
{
	const auto elapsed = std::chrono::steady_clock::now() - volumeDisplayStart;
	if (elapsed < volumeDisplayDuration) {
		auto min = ImGui::GetItemRectMin();
		ImGui::SetCursorScreenPos(min);

		const float t = float(elapsed / volumeDisplayDuration);

		float alpha;
		if (t <= 0.75f) {
			alpha = 1.0f;
		} else {
			float fadeProgress = (t - 0.75f) / 0.25f;
			alpha = 1.0f - fadeProgress;
		}

		ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, alpha), "Volume: %.0f%%", volume.load(std::memory_order_relaxed) * 100.0f);
	}
}

PLAYBACK_MODE VideoPlayer::GetPlaybackMode() const
{
	return playbackMode;
}

void VideoPlayer::SetPlaybackMode(PLAYBACK_MODE a_mode)
{
	playbackMode = a_mode;
}

void VideoPlayer::IncrementVolume(float a_delta)
{
	if (sourceVoice) {
		auto tempVolume = std::clamp(volume.load(std::memory_order_relaxed) + a_delta, 0.0f, 1.0f);
		sourceVoice->SetVolume(tempVolume);
		volume.store(tempVolume, std::memory_order_relaxed);
		volumeDisplayStart = std::chrono::steady_clock::now();
	}
}
