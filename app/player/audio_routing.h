// AudioRoutingMode — multi-track audio channel-selection modes. Copied as a
// standalone enum so the ported audio subsystem doesn't drag in QCView's
// project layer (its only dependency there). Values must match the int API
// of AudioDecoder::setRoutingMode / AudioPlayer::setRoutingMode.
#pragma once
namespace ufbplayer {
enum class AudioRoutingMode {
    Auto       = 0,
    Downmix5_1 = 1,   // BS.775 downmix from channels 1-6
    Stereo7_8  = 2,   // channels 7-8 (zero-indexed 6-7) as L/R
};
} // namespace ufbplayer
