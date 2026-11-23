#pragma once

#include "twosfdefs.h"

#include <core/coresettings.h>
#include <core/engine/audioinput.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct NDS_state;

namespace Fooyin::TwoSF {
struct TwosfLoaderState {
    std::vector<uint8_t> rom;
    std::vector<uint8_t> state;
    size_t romSize{0};
    size_t stateSize{0};
    int initialFrames{-1};
    int syncType{0};
    int clockdown{0};
    int arm9Clockdown{0};
    int arm7Clockdown{0};
};

struct TwosfTags {
    QString title;
    QString artist;
    QString game;
    QString copyright;
    QString year;
    QString comment;
    QString genre;
    int lengthMs{0};
    int fadeMs{0};
    bool hasLength{false};
    bool hasFade{false};
};

class TwoSFDecoder final : public AudioDecoder {
public:
    TwoSFDecoder();
    ~TwoSFDecoder() override;

    [[nodiscard]] QStringList extensions() const override;
    [[nodiscard]] bool isSeekable() const override;
    [[nodiscard]] RepeatHandling repeatHandling() const override;
    [[nodiscard]] bool trackHasChanged() const override;
    [[nodiscard]] Track changedTrack() const override;

    std::optional<AudioFormat> init(const AudioSource& source, const Track& track, DecoderOptions options) override;
    void start() override;
    void stop() override;
    void seek(uint64_t pos) override;
    AudioBuffer readBuffer(size_t bytes) override;

protected:
    void playbackHintsChanged(PlaybackHints hints) override;

private:
    bool loadEmulator();
    void destroyEmulator();
    void renderFrames(int16_t* destination, int frames);
    [[nodiscard]] uint64_t finiteLengthFrames() const;
    [[nodiscard]] bool isAtEnd() const;

    FySettings m_settings;
    AudioFormat m_format;
    AudioSource m_source;
    QString m_path;
    TwosfLoaderState m_loader;
    TwosfTags m_tags;
    std::unique_ptr<NDS_state> m_emu;
    Track m_changedTrack;
    uint64_t m_positionFrames{0};
    uint64_t m_songFrames{0};
    uint64_t m_fadeFrames{0};
    int m_interpolation{DefaultInterpolation};
    bool m_isDecoding{false};
    bool m_repeatTrack{false};
    bool m_allowInfiniteRepeat{true};
};

class TwoSFReader final : public AudioReader {
public:
    TwoSFReader();

    [[nodiscard]] QStringList extensions() const override;
    [[nodiscard]] bool canReadCover() const override;
    [[nodiscard]] bool canWriteMetaData() const override;
    [[nodiscard]] int subsongCount() const override;

    bool init(const AudioSource& source) override;
    bool readTrack(const AudioSource& source, Track& track) override;

private:
    FySettings m_settings;
    AudioSource m_source;
    QString m_path;
};
} // namespace Fooyin::TwoSF
