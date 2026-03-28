#pragma once

// Forward declarations for FFmpeg (C library)
struct AVFormatContext;
struct AVCodecContext;
struct SwrContext;

namespace ImGui
{
	struct Texture
	{
		Texture() = default;
		Texture(ID3D11Device* device, std::uint32_t a_width, std::uint32_t a_height);
		~Texture() = default;

		void Update(ID3D11DeviceContext* context, const cv::Mat& frame) const;

		// members
		ComPtr<ID3D11Texture2D>          texture{ nullptr };
		ComPtr<ID3D11ShaderResourceView> srView{ nullptr };
	};
}

enum class PLAYBACK_MODE
{
	kPlayOnce,
	kPlayNext,
	kLoop
};

enum class PLAYBACK_STATE : std::uint8_t
{
	kIdle,
	kPlaying,
	kStopping,  // Resetting
	kTransitioning
};

class VideoPlayer
{
public:
	VideoPlayer() = default;
	~VideoPlayer()
	{
		auto expected = PLAYBACK_STATE::kPlaying;
		if (playbackState.compare_exchange_strong(expected, PLAYBACK_STATE::kStopping,
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
			ResetImpl();
		} else if (expected == PLAYBACK_STATE::kStopping || expected == PLAYBACK_STATE::kTransitioning) {
			if (resetThread.joinable()) {
				resetThread.request_stop();
				resetThread.join();
			}
		}
	}

	bool LoadVideo(ID3D11Device* device, const std::string& path, bool playAudio);
	void Update(ID3D11DeviceContext* context);
	void Reset(bool playNextVideo = false);

	ImTextureID GetTextureID() const;
	ImVec2      GetNativeSize() const;

	bool IsInitialized() const;
	bool IsPlaying() const;
	bool IsTransitioning() const;
	bool IsPlayingAudio() const;

	void ShowDebugInfo();
	void OnVolumeUpdate();

	PLAYBACK_MODE GetPlaybackMode() const;
	void          SetPlaybackMode(PLAYBACK_MODE a_mode);

	void IncrementVolume(float a_delta);

private:
	using clock = std::chrono::steady_clock;
	using duration = std::chrono::duration<double>;
	using time_point = std::chrono::time_point<clock, duration>;

	using Lock = std::shared_mutex;
	using ReadLocker = std::shared_lock<Lock>;
	using WriteLocker = std::unique_lock<Lock>;

	void CreateVideoThread();
	void CreateAudioThread();
	void RestartAudioThread();

	bool LoadAudio(const std::string& path);

	void ResetAudio();
	void ResetImpl(bool playNextVideo = false);

	// members
	std::string                     currentVideo;
	cv::VideoCapture                cap;
	std::unique_ptr<ImGui::Texture> texture;
	ImVec2                          displaySize{ 0.0f, 0.0f };
	PLAYBACK_MODE                   playbackMode{ PLAYBACK_MODE::kLoop };
	std::uint32_t                   videoWidth{ 0 };
	std::uint32_t                   videoHeight{ 0 };
	float                           targetFPS{ 30.0f };
	std::atomic<float>              actualFPS{ 0.0f };
	std::uint32_t                   frameCount{ 0 };
	duration                        frameDuration{ 0.0 };
	std::atomic<std::uint32_t>      readFrameCount{ 0 };
	std::atomic<float>              elapsedTime{ 0.0f };
	duration                        debugUpdateInterval{ 0.1 };
	cv::Mat                         videoFrame;
	mutable Lock                    videoFrameLock;
	AVFormatContext*                audioFmtCtx{ nullptr };
	AVCodecContext*                 audioDecCtx{ nullptr };
	SwrContext*                     audioSwrCtx{ nullptr };
	int                             audioStreamIdx{ -1 };
	ComPtr<IXAudio2>                xaudio2{};
	IXAudio2MasteringVoice*         masterVoice{ nullptr };
	IXAudio2SourceVoice*            sourceVoice{ nullptr };
	WAVEFORMATEX                    audioFormat{};
	std::atomic<float>              volume{ 1.0f };
	time_point                      volumeDisplayStart{};
	std::jthread                    audioThread;
	std::jthread                    videoThread;
	std::jthread                    resetThread;
	std::barrier<>                  startBarrier{ 2 };
	std::atomic<bool>               audioLoaded{ false };
	std::atomic<PLAYBACK_STATE>     playbackState{ PLAYBACK_STATE::kIdle };

	static constexpr duration volumeDisplayDuration{ 1.5 };
};
