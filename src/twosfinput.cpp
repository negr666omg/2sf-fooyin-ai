#include "twosfinput.h"
#include "twosfdefs.h"

#include <psflib.h>
#include <state.h>
#include <SPU.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <zlib.h>

Q_LOGGING_CATEGORY(TWOSF, "fy.twosf")

namespace Fooyin::TwoSF {
namespace {
constexpr int PsfVersion2SF = 0x24;
constexpr int RenderChunkFrames = 8192;
constexpr size_t MaxPsfSaveBytes = 256U * 1024U * 1024U;
// Vio2SF contains legacy translation-unit globals (notably resampler setup).
// Serialise all core entry points so Fooyin's gapless predecode cannot race them.
std::mutex Vio2sfMutex;

QString normalisedName(QString name) {
    name.replace(u'\\', u'/');
    return QDir::cleanPath(name);
}

struct PsfIoContext {
    AudioSource source;
    QString sourcePath;
};

struct PsfFileHandle {
    std::unique_ptr<QIODevice> ownedDevice;
    QIODevice* device{nullptr};
};

uint32_t readLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0])
         | (static_cast<uint32_t>(data[1]) << 8)
         | (static_cast<uint32_t>(data[2]) << 16)
         | (static_cast<uint32_t>(data[3]) << 24);
}

bool applyMap(std::vector<uint8_t>& destination, size_t& logicalSize,
              const uint8_t* data, size_t size, bool padToPowerOfTwo) {
    if(!data || size < 8) {
        return false;
    }

    const uint64_t offset = readLe32(data);
    const uint64_t bytes  = readLe32(data + 4);
    if(bytes > size - 8 || offset > MaxPsfSaveBytes || bytes > MaxPsfSaveBytes - offset) {
        return false;
    }

    uint64_t required = offset + bytes;
    if(padToPowerOfTwo && required > 0) {
        --required;
        required |= required >> 1;
        required |= required >> 2;
        required |= required >> 4;
        required |= required >> 8;
        required |= required >> 16;
        required |= required >> 32;
        ++required;
    }
    if(required > MaxPsfSaveBytes) {
        return false;
    }

    logicalSize = std::max(logicalSize, static_cast<size_t>(required));
    const size_t allocationSize = logicalSize + 10U; // Vio2SF's loader keeps ten guard bytes.
    if(destination.size() < allocationSize) {
        destination.resize(allocationSize, 0);
    }
    if(bytes > 0) {
        std::memcpy(destination.data() + offset, data + 8, static_cast<size_t>(bytes));
    }
    return true;
}

bool applyCompressedMap(std::vector<uint8_t>& destination, size_t& logicalSize,
                        const uint8_t* data, size_t size) {
    if(!data || size == 0 || size > std::numeric_limits<uLong>::max()) {
        return false;
    }

    uLongf capacity = 4096;
    std::vector<uint8_t> unpacked;
    for(;;) {
        if(capacity > MaxPsfSaveBytes || capacity < 8) {
            return false;
        }
        unpacked.resize(static_cast<size_t>(capacity));
        uLongf actual = capacity;
        const int result = uncompress(unpacked.data(), &actual, data, static_cast<uLong>(size));
        if(result == Z_OK) {
            unpacked.resize(static_cast<size_t>(actual));
            return applyMap(destination, logicalSize, unpacked.data(), unpacked.size(), false);
        }
        if(result != Z_BUF_ERROR || capacity > MaxPsfSaveBytes / 2U) {
            return false;
        }
        capacity *= 2U;
    }
}

int psfLoadData(void* context, const uint8_t* executable, size_t executableSize,
                const uint8_t* reserved, size_t reservedSize) {
    auto* state = static_cast<TwosfLoaderState*>(context);
    if(!state) {
        return -1;
    }

    if(executableSize >= 8 && !applyMap(state->rom, state->romSize, executable, executableSize, true)) {
        return -1;
    }

    if(reservedSize > 0) {
        if(!reserved || reservedSize < 12) {
            return -1;
        }
        size_t position = 0;
        while(position + 12 <= reservedSize) {
            const uint32_t magic = readLe32(reserved + position);
            const uint32_t payloadSize = readLe32(reserved + position + 4);
            if(static_cast<uint64_t>(position) + 12U + payloadSize > reservedSize) {
                return -1;
            }
            if(magic == 0x45564153U) { // "SAVE"
                if(!applyCompressedMap(state->state, state->stateSize, reserved + position + 12, payloadSize)) {
                    return -1;
                }
            }
            position += 12U + payloadSize;
        }
    }
    return 0;
}

int parseTimeMs(const char* raw) {
    if(!raw) {
        return 0;
    }
    const QString value = QString::fromUtf8(raw).trimmed();
    if(value.isEmpty()) {
        return 0;
    }

    const QStringList parts = value.split(u':');
    bool ok = false;
    double milliseconds = 0.0;
    if(parts.size() == 1) {
        // PSF tags without colons are seconds, optionally fractional.
        const double seconds = parts[0].toDouble(&ok);
        if(!ok || seconds < 0.0) {
            return 0;
        }
        milliseconds = seconds * 1000.0;
    }
    else if(parts.size() == 2 || parts.size() == 3) {
        const double last = parts.back().toDouble(&ok);
        if(!ok || last < 0.0 || last >= 60.0) {
            return 0;
        }
        bool middleOk = false;
        const int middle = parts[parts.size() - 2].toInt(&middleOk);
        if(!middleOk || middle < 0 || middle >= 60) {
            return 0;
        }
        milliseconds = last * 1000.0 + middle * 60000.0;
        if(parts.size() == 3) {
            bool firstOk = false;
            const int hours = parts[0].toInt(&firstOk);
            if(!firstOk || hours < 0) {
                return 0;
            }
            milliseconds += hours * 3600000.0;
        }
    }
    else {
        return 0;
    }

    if(milliseconds > static_cast<double>(std::numeric_limits<int>::max())) {
        return 0;
    }
    return static_cast<int>(std::llround(milliseconds));
}

int psfInfo(void* context, const char* name, const char* value) {
    auto* info = static_cast<TwosfTags*>(context);
    if(!info || !name || !value) {
        return 0;
    }

    const QString key = QString::fromLatin1(name).trimmed().toLower();
    const QString text = QString::fromUtf8(value).trimmed();
    if(key == u"title") {
        info->title = text;
    }
    else if(key == u"artist") {
        info->artist = text;
    }
    else if(key == u"game" || key == u"album") {
        info->game = text;
    }
    else if(key == u"copyright") {
        info->copyright = text;
    }
    else if(key == u"year" || key == u"date") {
        info->year = text;
    }
    else if(key == u"comment") {
        info->comment = text;
    }
    else if(key == u"genre") {
        info->genre = text;
    }
    else if(key == u"length") {
        info->lengthMs = parseTimeMs(value);
        info->hasLength = true;
    }
    else if(key == u"fade") {
        info->fadeMs = parseTimeMs(value);
        info->hasFade = true;
    }
    return 0;
}

int psfConfigInfo(void* context, const char* name, const char* value) {
    auto* state = static_cast<TwosfLoaderState*>(context);
    if(!state || !name || !value) {
        return 0;
    }
    char* end = nullptr;
    const long number = std::strtol(value, &end, 10);
    if(end == value || *end != '\0') {
        return 0;
    }

    const QByteArray key = QByteArray(name).toLower();
    if(key == "_frames") {
        state->initialFrames = static_cast<int>(number);
    }
    else if(key == "_clockdown") {
        state->clockdown = static_cast<int>(number);
    }
    else if(key == "_vio2sf_sync_type") {
        state->syncType = static_cast<int>(number);
    }
    else if(key == "_vio2sf_arm9_clockdown_level") {
        state->arm9Clockdown = static_cast<int>(number);
    }
    else if(key == "_vio2sf_arm7_clockdown_level") {
        state->arm7Clockdown = static_cast<int>(number);
    }
    return 0;
}

struct PsfInfoContext {
    TwosfTags* tags;
    TwosfLoaderState* loader;
};

int psfCombinedInfo(void* context, const char* name, const char* value) {
    auto* info = static_cast<PsfInfoContext*>(context);
    if(info->tags) {
        psfInfo(info->tags, name, value);
    }
    if(info->loader) {
        psfConfigInfo(info->loader, name, value);
    }
    return 0;
}

void psfStatus(void*, const char*) { }

void* psfOpen(void* opaqueContext, const char* filename) {
    auto* context = static_cast<PsfIoContext*>(opaqueContext);
    if(!context || !filename) {
        return nullptr;
    }

    const QString path = normalisedName(QString::fromUtf8(filename));
    auto handle = std::make_unique<PsfFileHandle>();

    if(context->source.device && path == context->sourcePath) {
        handle->device = context->source.device;
    }
    else if(context->source.archiveReader) {
        ArchiveEntryData entry = context->source.archiveReader->entry(path);
        handle->ownedDevice = std::move(entry.device);
        handle->device = handle->ownedDevice.get();
    }
    else {
        auto file = std::make_unique<QFile>(path);
        if(file->open(QIODevice::ReadOnly)) {
            handle->ownedDevice = std::move(file);
            handle->device = handle->ownedDevice.get();
        }
    }

    if(!handle->device || !handle->device->isOpen() || !handle->device->isReadable() || handle->device->isSequential()) {
        return nullptr;
    }
    if(!handle->device->seek(0)) {
        return nullptr;
    }
    return handle.release();
}

size_t psfRead(void* destination, size_t size, size_t count, void* opaqueHandle) {
    auto* handle = static_cast<PsfFileHandle*>(opaqueHandle);
    if(!handle || !handle->device || !destination || size == 0 || count == 0
       || count > std::numeric_limits<size_t>::max() / size) {
        return 0;
    }
    const size_t requested = size * count;
    if(requested > static_cast<size_t>(std::numeric_limits<qint64>::max())) {
        return 0;
    }
    const qint64 bytes = handle->device->read(static_cast<char*>(destination), static_cast<qint64>(requested));
    return bytes > 0 ? static_cast<size_t>(bytes) / size : 0;
}

int psfSeek(void* opaqueHandle, int64_t offset, int origin) {
    auto* handle = static_cast<PsfFileHandle*>(opaqueHandle);
    if(!handle || !handle->device) {
        return -1;
    }
    qint64 base = 0;
    switch(origin) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = handle->device->pos(); break;
        case SEEK_END: base = handle->device->size(); break;
        default: return -1;
    }
    if((offset < 0 && base < -offset) || (offset > 0 && base > std::numeric_limits<qint64>::max() - offset)) {
        return -1;
    }
    const qint64 target = base + offset;
    return target >= 0 && handle->device->seek(target) ? 0 : -1;
}

int psfClose(void* opaqueHandle) {
    delete static_cast<PsfFileHandle*>(opaqueHandle);
    return 0;
}

long psfTell(void* opaqueHandle) {
    auto* handle = static_cast<PsfFileHandle*>(opaqueHandle);
    if(!handle || !handle->device) {
        return -1;
    }
    const qint64 position = handle->device->pos();
    if(position > std::numeric_limits<long>::max()) {
        return -1;
    }
    return static_cast<long>(position);
}

psf_file_callbacks makePsfCallbacks(PsfIoContext& context) {
    psf_file_callbacks callbacks{};
    callbacks.path_separators = "\\/";
    callbacks.context = &context;
    callbacks.fopen = psfOpen;
    callbacks.fread = psfRead;
    callbacks.fseek = psfSeek;
    callbacks.fclose = psfClose;
    callbacks.ftell = psfTell;
    return callbacks;
}

// Switch the quality of existing resamplers as well as the mode used for
// channels that are started while seeking. Quality changes only affect the
// generated audio samples, not DS CPU/timer execution.
void setEmulatorInterpolation(NDS_state* state, int quality) {
    if(!state) {
        return;
    }

    state->dwInterpolation = static_cast<unsigned long>(quality);
    if(!state->SPU_core) {
        return;
    }

    for(channel_struct& channel : state->SPU_core->channels) {
        if(channel.resampler) {
            const int channelQuality = channel.format == 3 ? RESAMPLER_QUALITY_BLEP : quality;
            resampler_set_quality(channel.resampler, channelQuality);
        }
    }
}

bool loadPsf(const AudioSource& source, TwosfLoaderState* loader, TwosfTags* tags, bool includeNestedTags) {
    PsfIoContext context{source, normalisedName(source.filepath)};
    psf_file_callbacks callbacks = makePsfCallbacks(context);
    const QByteArray path = source.filepath.toUtf8();

    // Vio2SF configuration tags (_frames, _clockdown, etc.) may live in a
    // linked .2sflib, so collect them from the complete PSF library chain.
    // User-facing metadata belongs to the selected .mini2sf itself: parse it
    // separately with nested tags disabled, otherwise a library's TITLE or
    // ARTIST can accidentally overwrite the current track's tags.
    if(loader) {
        PsfInfoContext configContext{nullptr, loader};
        const int dataResult = psf_load(path.constData(), &callbacks, PsfVersion2SF,
                                        psfLoadData, loader, psfCombinedInfo,
                                        &configContext, includeNestedTags ? 1 : 0, psfStatus, nullptr);
        if(dataResult <= 0) {
            qCWarning(TWOSF) << "PSF data loader rejected" << source.filepath << "with code" << dataResult;
            return false;
        }
    }

    if(tags) {
        const int tagResult = psf_load(path.constData(), &callbacks, PsfVersion2SF,
                                       nullptr, nullptr, psfInfo, tags, 0, psfStatus, nullptr);
        if(tagResult <= 0) {
            qCWarning(TWOSF) << "PSF metadata loader rejected" << source.filepath << "with code" << tagResult;
            return false;
        }
    }
    return true;
}

uint64_t framesFromMs(int milliseconds) {
    if(milliseconds <= 0) {
        return 0;
    }
    return (static_cast<uint64_t>(milliseconds) * SampleRate + 500U) / 1000U;
}

void applyTagsToTrack(const TwosfTags& tags, Track& track) {
    if(!tags.title.isEmpty()) {
        track.setTitle(tags.title);
    }
    if(!tags.artist.isEmpty()) {
        track.setArtists({tags.artist});
    }
    if(!tags.game.isEmpty()) {
        track.setAlbum(tags.game);
        track.addExtraTag(QStringLiteral("GAME"), tags.game);
    }
    if(!tags.year.isEmpty()) {
        track.setDate(tags.year);
    }
    if(!tags.comment.isEmpty()) {
        track.setComment(tags.comment);
    }
    if(!tags.genre.isEmpty()) {
        track.setGenres({tags.genre});
    }
    if(!tags.copyright.isEmpty()) {
        track.addExtraTag(QStringLiteral("COPYRIGHT"), tags.copyright);
    }
    if(tags.lengthMs > 0) {
        track.addExtraTag(QStringLiteral("PSF_LENGTH"), QString::number(tags.lengthMs));
    }
    if(tags.fadeMs > 0) {
        track.addExtraTag(QStringLiteral("PSF_FADE"), QString::number(tags.fadeMs));
    }
}
} // namespace

TwoSFDecoder::TwoSFDecoder() = default;
TwoSFDecoder::~TwoSFDecoder() {
    stop();
}

QStringList TwoSFDecoder::extensions() const {
    return {QStringLiteral("2sf"), QStringLiteral("mini2sf")};
}

bool TwoSFDecoder::isSeekable() const {
    return true;
}

AudioDecoder::RepeatHandling TwoSFDecoder::repeatHandling() const {
    return RepeatHandling::DecoderLoop;
}

bool TwoSFDecoder::trackHasChanged() const {
    return m_changedTrack.isValid();
}

Track TwoSFDecoder::changedTrack() const {
    return m_changedTrack;
}

std::optional<AudioFormat> TwoSFDecoder::init(const AudioSource& source, const Track& track, DecoderOptions options) {
    stop();
    m_changedTrack = {};
    m_source = source;
    m_path = source.filepath;
    m_positionFrames = 0;
    m_allowInfiniteRepeat = !(options & (NoLooping | NoInfiniteLooping));
    m_repeatTrack = m_allowInfiniteRepeat && isRepeatingTrack();
    m_interpolation = std::clamp(m_settings.value(InterpolationSetting, DefaultInterpolation).toInt(), 0, 4);

    if(source.device && source.device->isSequential()) {
        qCWarning(TWOSF) << "Sequential input is not seekable:" << source.filepath;
        return {};
    }

    if(!loadPsf(source, &m_loader, &m_tags, true)) {
        return {};
    }

    const bool useDefaultTiming = !m_tags.hasLength || m_tags.lengthMs <= 0;
    const int lengthMs = useDefaultTiming ? DefaultLengthMs : m_tags.lengthMs;
    const int fadeMs = useDefaultTiming ? DefaultFadeMs : (m_tags.hasFade ? m_tags.fadeMs : 0);
    m_songFrames = framesFromMs(lengthMs);
    m_fadeFrames = framesFromMs(fadeMs);

    if(!loadEmulator()) {
        return {};
    }

    m_format = AudioFormat{SampleFormat::F32, SampleRate, 2};
    if(!m_format.isValid()) {
        destroyEmulator();
        return {};
    }

    if(options & UpdateTracks) {
        const uint64_t duration = (m_songFrames + m_fadeFrames) * 1000U / SampleRate;
        if(duration > 0 && track.duration() != duration) {
            m_changedTrack = track;
            m_changedTrack.setDuration(duration);
        }
    }
    return m_format;
}

bool TwoSFDecoder::loadEmulator() {
    destroyEmulator();
    if(m_loader.romSize == 0 && m_loader.stateSize == 0) {
        qCWarning(TWOSF) << "2SF file contains no Nintendo DS ROM/save-state data:" << m_path;
        return false;
    }

    auto emulator = std::make_unique<NDS_state>();
    std::memset(emulator.get(), 0, sizeof(NDS_state));
    {
        const std::lock_guard lock{Vio2sfMutex};
        if(state_init(emulator.get()) != 0) {
            state_deinit(emulator.get());
            qCWarning(TWOSF) << "Vio2SF state_init failed for" << m_path;
            return false;
        }

        emulator->dwInterpolation = static_cast<unsigned long>(m_interpolation);
        emulator->dwChannelMute = 0;
        emulator->initial_frames = m_loader.initialFrames;
        emulator->sync_type = m_loader.syncType;
        emulator->arm9_clockdown_level = m_loader.arm9Clockdown ? m_loader.arm9Clockdown : m_loader.clockdown;
        emulator->arm7_clockdown_level = m_loader.arm7Clockdown ? m_loader.arm7Clockdown : m_loader.clockdown;

        if(m_loader.romSize > 0) {
            if(m_loader.romSize > std::numeric_limits<uint32_t>::max()) {
                state_deinit(emulator.get());
                qCWarning(TWOSF) << "2SF ROM image is too large:" << m_path;
                return false;
            }
            state_setrom(emulator.get(), m_loader.rom.data(), static_cast<uint32_t>(m_loader.romSize), 0);
        }
        if(m_loader.stateSize > 0) {
            if(m_loader.stateSize > std::numeric_limits<uint32_t>::max()) {
                state_deinit(emulator.get());
                qCWarning(TWOSF) << "2SF save-state is too large:" << m_path;
                return false;
            }
            state_loadstate(emulator.get(), m_loader.state.data(), static_cast<uint32_t>(m_loader.stateSize));
        }
    }

    m_emu = std::move(emulator);
    return true;
}

void TwoSFDecoder::destroyEmulator() {
    if(m_emu) {
        {
            const std::lock_guard lock{Vio2sfMutex};
            state_deinit(m_emu.get());
        }
        m_emu.reset();
    }
}

void TwoSFDecoder::renderFrames(int16_t* destination, int frames) {
    if(!m_emu || !destination || frames <= 0) {
        return;
    }
    const std::lock_guard lock{Vio2sfMutex};
    state_render(m_emu.get(), destination, static_cast<unsigned int>(frames));
}

uint64_t TwoSFDecoder::finiteLengthFrames() const {
    return m_songFrames + m_fadeFrames;
}

bool TwoSFDecoder::isAtEnd() const {
    return !m_repeatTrack && finiteLengthFrames() > 0 && m_positionFrames >= finiteLengthFrames();
}

void TwoSFDecoder::start() {
    m_isDecoding = true;
}

void TwoSFDecoder::stop() {
    m_isDecoding = false;
    destroyEmulator();
    m_source = {};
    m_loader = {};
    m_tags = {};
    m_changedTrack = {};
    m_positionFrames = 0;
    m_songFrames = 0;
    m_fadeFrames = 0;
}

void TwoSFDecoder::seek(uint64_t positionMs) {
    if(!m_emu) {
        return;
    }
    const uint64_t wholeSeconds = positionMs / 1000U;
    const uint64_t remainderMs = positionMs % 1000U;
    const uint64_t maxFrames = std::numeric_limits<uint64_t>::max();
    const uint64_t tailFrames = (remainderMs * SampleRate) / 1000U;
    uint64_t target = maxFrames;
    if(wholeSeconds <= maxFrames / SampleRate) {
        const uint64_t baseFrames = wholeSeconds * SampleRate;
        if(baseFrames <= maxFrames - tailFrames) {
            target = baseFrames + tailFrames;
        }
    }
    if(!m_repeatTrack) {
        target = std::min(target, finiteLengthFrames());
    }

    if(target < m_positionFrames) {
        if(!loadEmulator()) {
            m_isDecoding = false;
            return;
        }
        m_positionFrames = 0;
    }

    if(m_positionFrames < target) {
        std::vector<int16_t> discard(static_cast<size_t>(RenderChunkFrames) * 2U);

        // Seeking does not need high-quality resampling for samples that are
        // immediately discarded. Use the cheapest mode during the fast-forward,
        // then restore the user's selected quality before audible playback.
        {
            const std::lock_guard lock{Vio2sfMutex};
            setEmulatorInterpolation(m_emu.get(), RESAMPLER_QUALITY_ZOH);
        }

        while(m_positionFrames < target) {
            const uint64_t remaining = target - m_positionFrames;
            const int count = static_cast<int>(std::min<uint64_t>(remaining, RenderChunkFrames));
            renderFrames(discard.data(), count);
            m_positionFrames += static_cast<uint64_t>(count);
        }

        {
            const std::lock_guard lock{Vio2sfMutex};
            setEmulatorInterpolation(m_emu.get(), m_interpolation);
        }
    }
}

AudioBuffer TwoSFDecoder::readBuffer(size_t bytes) {
    if(!m_isDecoding || !m_emu || bytes == 0 || isAtEnd()) {
        return {};
    }

    const int requestedFrames = m_format.framesForBytes(bytes);
    if(requestedFrames <= 0) {
        return {};
    }

    int frames = requestedFrames;
    if(!m_repeatTrack) {
        const uint64_t remaining = finiteLengthFrames() - m_positionFrames;
        frames = static_cast<int>(std::min<uint64_t>(static_cast<uint64_t>(frames), remaining));
    }
    if(frames <= 0) {
        return {};
    }

    const uint64_t startFrame = m_positionFrames;
    const uint64_t startTime = (startFrame / SampleRate) * 1000U
                             + ((startFrame % SampleRate) * 1000U) / SampleRate;
    AudioBuffer buffer{m_format, startTime};
    const size_t outputBytes = static_cast<size_t>(frames) * static_cast<size_t>(m_format.bytesPerFrame());
    buffer.resize(outputBytes);

    std::vector<int16_t> decoded(static_cast<size_t>(frames) * 2U);
    renderFrames(decoded.data(), frames);
    auto* output = reinterpret_cast<float*>(buffer.data());
    for(int frame = 0; frame < frames; ++frame) {
        float gain = 1.0F;
        const uint64_t absoluteFrame = startFrame + static_cast<uint64_t>(frame);
        if(!m_repeatTrack && m_fadeFrames > 0 && absoluteFrame >= m_songFrames) {
            const uint64_t intoFade = absoluteFrame - m_songFrames;
            gain = intoFade >= m_fadeFrames
                ? 0.0F
                : static_cast<float>(m_fadeFrames - intoFade) / static_cast<float>(m_fadeFrames);
        }
        output[frame * 2] = (static_cast<float>(decoded[static_cast<size_t>(frame) * 2U]) / 32768.0F) * gain;
        output[frame * 2 + 1] = (static_cast<float>(decoded[static_cast<size_t>(frame) * 2U + 1U]) / 32768.0F) * gain;
    }

    m_positionFrames += static_cast<uint64_t>(frames);
    return buffer;
}

void TwoSFDecoder::playbackHintsChanged(PlaybackHints) {
    if(!m_allowInfiniteRepeat || !m_emu) {
        return;
    }
    // Repeat One keeps this exact emulator state alive. The DS game's own
    // sequencer performs its loop; there is no stop/re-open boundary to gap.
    m_repeatTrack = isRepeatingTrack();
}

TwoSFReader::TwoSFReader() = default;

QStringList TwoSFReader::extensions() const {
    return {QStringLiteral("2sf"), QStringLiteral("mini2sf")};
}

bool TwoSFReader::canReadCover() const {
    return false;
}

bool TwoSFReader::canWriteMetaData() const {
    return false;
}

int TwoSFReader::subsongCount() const {
    return 1;
}

bool TwoSFReader::init(const AudioSource& source) {
    if(source.device && source.device->isSequential()) {
        return false;
    }
    m_source = source;
    m_path = source.filepath;
    return true;
}

bool TwoSFReader::readTrack(const AudioSource& source, Track& track) {
    TwosfTags tags;
    // Metadata scans only need tags from the selected MINI2SF/2SF file.
    // With no load callback PSFLib skips dependency loading and decompression,
    // matching Kodi's own lightweight ReadTag/Init path.
    if(!loadPsf(source, nullptr, &tags, false)) {
        return false;
    }

    const bool useDefaultTiming = !tags.hasLength || tags.lengthMs <= 0;
    const int lengthMs = useDefaultTiming ? DefaultLengthMs : tags.lengthMs;
    const int fadeMs = useDefaultTiming ? DefaultFadeMs : (tags.hasFade ? tags.fadeMs : 0);
    const uint64_t duration = static_cast<uint64_t>(lengthMs) + static_cast<uint64_t>(fadeMs);

    const qint64 sourceSize = source.size > 0 ? static_cast<qint64>(source.size)
                            : source.device ? source.device->size()
                                            : QFileInfo{source.filepath}.size();
    track.setFileSize(static_cast<uint64_t>(std::max<qint64>(sourceSize, 0)));
    track.setDuration(duration);
    track.setSampleRate(SampleRate);
    track.setChannels(2);
    track.setBitDepth(16);
    track.setCodec(QStringLiteral("Nintendo DS 2SF"));
    track.setCodecProfile(QStringLiteral("Vio2SF"));
    applyTagsToTrack(tags, track);
    return true;
}
} // namespace Fooyin::TwoSF
