#pragma once

#include <cstdint>

namespace otoworm {
    enum NoteClass
    {
        NK_NORMAL,
        NK_FAKE,
        NK_MINE,
        NK_LIFT,
        NK_ROLL, // subtype of hold
        NK_INVISIBLE,
        NK_TOTAL
    };

    enum TimingType
    {
        TT_TIME,
        TT_BEATS,
        TT_PIXELS
    };

    enum ChartClass {
        CC_NULL,
        CC_BMS,
        CC_OSUMANIA,
        CC_STEPMANIA,
        CC_O2JAM
    };
}


