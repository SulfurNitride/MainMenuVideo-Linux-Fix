#include "VideoPlayer.h"

#include "Manager.h"

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

// XAudio2 audio output (replaces MF Audio Renderer for Wine/Proton compatibility)
// MF Source Reader decodes audio to PCM, XAudio2 handles playback via FAudio on Linux
bool VideoPlayer::LoadAudio(const std::string& path)
{
	HRESULT hr = MFCreateSourceReaderFromURL(stl::utf8_to_utf16(path)->c_str(), nullptr, &audioReader);
	if (FAILED(hr)) {
		logger::warn("Failed to create MF source reader for audio: 0x{:X}", static_cast<std::uint32_t>(hr));
		ResetAudio();
		return false;
	}

	// Select only the audio stream
	hr = audioReader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
	if (FAILED(hr)) {
		ResetAudio();
		return false;
	}
	hr = audioReader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
	if (FAILED(hr)) {
		ResetAudio();
		return false;
	}

	// Request decoded PCM output from the source reader
	ComPtr<IMFMediaType> pcmType;
	hr = MFCreateMediaType(&pcmType);
	if (FAILED(hr)) {
		ResetAudio();
		return false;
	}
	pcmType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	pcmType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
	hr = audioReader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, pcmType.Get());
	if (FAILED(hr)) {
		logger::warn("Failed to set PCM output type on source reader: 0x{:X}", static_cast<std::uint32_t>(hr));
		ResetAudio();
		return false;
	}

	// Get the actual decoded output format
	ComPtr<IMFMediaType> outputType;
	hr = audioReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &outputType);
	if (FAILED(hr)) {
		ResetAudio();
		return false;
	}

	UINT32         formatSize = 0;
	WAVEFORMATEX*  pFormat = nullptr;
	hr = MFCreateWaveFormatExFromMFMediaType(outputType.Get(), &pFormat, &formatSize);
	if (FAILED(hr) || !pFormat) {
		ResetAudio();
		return false;
	}
	audioFormat = *pFormat;
	CoTaskMemFree(pFormat);

	// Create XAudio2 engine (uses FAudio on Proton/Wine)
	hr = XAudio2Create(&xaudio2);
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

	logger::info("Audio loaded via XAudio2: {}ch {}Hz {}bit",
		audioFormat.nChannels, audioFormat.nSamplesPerSec, audioFormat.wBitsPerSample);

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

		constexpr DWORD  audioStreamIndex = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
		constexpr UINT32 MAX_QUEUED_BUFFERS = 4;

		// Ring buffer pool — each slot stays alive until XAudio2 consumes it
		std::vector<std::vector<BYTE>> bufferPool(MAX_QUEUED_BUFFERS);
		UINT32 currentBuffer = 0;

		while (!st.stop_requested()) {
			// Throttle: wait if XAudio2 has enough queued data
			XAUDIO2_VOICE_STATE voiceState;
			sourceVoice->GetState(&voiceState);
			if (voiceState.BuffersQueued >= MAX_QUEUED_BUFFERS) {
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				continue;
			}

			ComPtr<IMFSample> sample;
			DWORD             streamFlags = 0;
			MFTIME            timestamp = 0;

			HRESULT hr = audioReader->ReadSample(audioStreamIndex, 0, nullptr, &streamFlags, &timestamp, &sample);
			if (FAILED(hr) || (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM)) {
				break;
			}

			if (!sample) {
				continue;
			}

			ComPtr<IMFMediaBuffer> mediaBuffer;
			hr = sample->ConvertToContiguousBuffer(&mediaBuffer);
			if (FAILED(hr)) {
				continue;
			}

			BYTE* rawData = nullptr;
			DWORD rawLen = 0;
			hr = mediaBuffer->Lock(&rawData, nullptr, &rawLen);
			if (FAILED(hr)) {
				continue;
			}

			// Copy decoded PCM into our persistent buffer slot
			auto& buf = bufferPool[currentBuffer % MAX_QUEUED_BUFFERS];
			buf.assign(rawData, rawData + rawLen);
			mediaBuffer->Unlock();

			XAUDIO2_BUFFER xbuf{};
			xbuf.AudioBytes = rawLen;
			xbuf.pAudioData = buf.data();

			sourceVoice->SubmitSourceBuffer(&xbuf);
			currentBuffer++;
		}

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
	audioReader = nullptr;
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
