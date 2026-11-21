#pragma once

#include <string_view>

namespace ammod::ui {

inline bool IsRowPlayButton(std::wstring_view id, std::wstring_view name,
                            std::wstring_view label) noexcept {
    const auto playId = [](std::wstring_view value) {
        return value == L"Play" || value == L"PlayButton" || value == L"PlayTrack" ||
            value == L"PlayTrackButton" || value == L"SongPlayButton" ||
            value.ends_with(L"_PlayButton") || value.ends_with(L"_Play");
    };
    return playId(id) || playId(name) || label == L"Play" || label == L"播放" ||
        label == L"Play song" || label == L"Play track" || label == L"播放歌曲";
}

} // namespace ammod::ui
