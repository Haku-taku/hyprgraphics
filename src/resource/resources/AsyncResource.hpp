#include <hyprgraphics/resource/resources/AsyncResource.hpp>

#include <condition_variable>

namespace Hyprgraphics {
    struct SAsyncResourceImpl {
        std::mutex              awaitingMtx;
        std::condition_variable awaitingCv;
        bool                    awaitingEvent = false; // guarded by awaitingMtx
    };
}
