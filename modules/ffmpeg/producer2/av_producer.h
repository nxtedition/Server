#include <memory>

#include <core/frame/frame.h>
#include <core/frame/frame_factory.h>

#include <boost/optional.hpp>
#include <boost/rational.hpp>

#include <string>
#include <memory>

namespace caspar {
namespace ffmpeg2 {

class AVProducer {
public:
	AVProducer(
        std::shared_ptr<core::frame_factory> frame_factory,
        core::video_format_desc format_desc,
        std::string filename,
        boost::optional<std::string> vfilter,
        boost::optional<std::string> afilter,
        boost::optional<int64_t> start,
        boost::optional<int64_t> duration,
        boost::optional<bool> loop);

    core::draw_frame get();
    bool next();

    AVProducer& seek(int64_t time);
    int64_t time() const;

    AVProducer& loop(bool loop);
    bool loop() const;

    AVProducer& start(int64_t start);
    int64_t start() const;

    AVProducer& duration(int64_t duration);
    int64_t duration() const;

    int width() const;
    int height() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace ffmpeg
}  // namespace caspar
