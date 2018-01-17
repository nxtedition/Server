/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
*
* This file is part of CasparCG (www.casparcg.com).
*
* CasparCG is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CasparCG is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
*
* Author: Robert Nagy, ronag89@gmail.com
*/

#include "../StdAfx.h"

#include "ffmpeg_producer2.h"

#include "../producer/util/util.h"
#include "av_producer.h"

#include "../ffmpeg.h"
#include "../ffmpeg_error.h"

#include <common/param.h>
#include <common/diagnostics/graph.h>
#include <common/future.h>

#include <core/frame/draw_frame.h>
#include <core/help/help_repository.h>
#include <core/help/help_sink.h>
#include <core/producer/media_info/media_info.h>
#include <core/producer/framerate/framerate_producer.h>
#include <core/frame/frame_factory.h>

#include <tbb/concurrent_queue.h>
#include <tbb/atomic.h>

#include <boost/timer.hpp>

#include <atomic>
#include <future>
#include <queue>
#include <thread>

namespace caspar { namespace ffmpeg2 {

typedef std::vector<std::pair<std::string, std::string>> ffmpeg_options;

// HACK
std::wstring get_relative_or_original(
	const std::wstring& filename,
	const boost::filesystem::path& relative_to)
{
	boost::filesystem::path file(filename);
	auto result = file.filename().wstring();

	boost::filesystem::path current_path = file;

	while (true)
	{
		current_path = current_path.parent_path();

		if (boost::filesystem::equivalent(current_path, relative_to))
			break;

		if (current_path.empty())
			return filename;

		result = current_path.filename().wstring() + L"/" + result;
	}

	return result;
}

struct Info
{
    core::draw_frame frame = core::draw_frame::late();
	int64_t number = 0;
	int64_t count = 0;
    bool loop = false;
    int width = 0;
    int height = 0;
};

struct ffmpeg_producer : public core::frame_producer_base
{
    typedef std::packaged_task<std::wstring()>                      task_t;
    typedef tbb::concurrent_bounded_queue<std::shared_ptr<task_t>>  tasks_t;

	const std::wstring 									filename_;
	const std::wstring 									path_relative_to_media_ = get_relative_or_original(filename_, env::media_folder());
	spl::shared_ptr<core::frame_factory> 				frame_factory_;
	core::video_format_desc								format_desc_;
    
    mutable std::mutex									info_mutex_;
	Info												info_;

	AVProducer											producer_;

	core::monitor::subject  							monitor_subject_;
	const spl::shared_ptr<diagnostics::graph>			graph_;
	core::constraints									constraints_;

    boost::optional<int64_t>                            seek_;

    tasks_t                                             tasks_;

	tbb::concurrent_bounded_queue<Info>		            buffer_;
	std::thread										    thread_;
public:
	explicit ffmpeg_producer(
			spl::shared_ptr<core::frame_factory> frame_factory,
			core::video_format_desc format_desc,
			std::wstring filename,
			std::wstring vfilter,
			std::wstring afilter,
			boost::optional<int64_t> start,
			boost::optional<int64_t> duration,
			boost::optional<bool> loop)
		: format_desc_(format_desc)
		, filename_(filename)
		, frame_factory_(frame_factory)
		, producer_(frame_factory_, 
					format_desc_, 
					u8(filename), 
					u8(vfilter), 
					u8(afilter), 
					start, 
					duration, 
					loop)
	{
        buffer_.set_capacity(2);

		if (producer_.width() > 0 && producer_.height() > 0) {
			constraints_.width.set(producer_.width());
			constraints_.height.set(producer_.height());
		}

        info_.number = to_frames(producer_.time());
        info_.count = to_frames(producer_.duration());
		
		diagnostics::register_graph(graph_);
		graph_->set_color("frame-time", diagnostics::color(0.1f, 1.0f, 0.1f));
		graph_->set_color("underflow", diagnostics::color(0.6f, 0.3f, 0.9f));
		graph_->set_color("buffer-count", diagnostics::color(0.7f, 0.4f, 0.4f));
		graph_->set_text(print());

        thread_ = std::thread([this] { run(); });
	}

	~ffmpeg_producer()
	{
		producer_.abort();
        buffer_.abort();
		thread_.join();
	}

	void run()
	{
		try {
			while (true) {
                boost::timer frame_timer;

                const auto frame = producer_.next();

                graph_->set_value("frame-time", frame_timer.elapsed() * boost::rational_cast<double>(format_desc_.framerate) * 0.5);
                graph_->set_value("buffer-count", static_cast<double>(buffer_.size()) / static_cast<double>(buffer_.capacity()));

                std::shared_ptr<task_t> task;
                while (tasks_.try_pop(task)) {
                    (*task)();
                }

                if (seek_) {
                    producer_.seek(*seek_);
                    seek_.reset();
                } else if (frame) {
                    Info info;
                    info.frame = frame;
                    info.number = to_frames(producer_.time());
                    info.count = to_frames(producer_.duration());
                    info.loop = producer_.loop();
                    info.width = producer_.width();
                    info.height = producer_.height();

                    buffer_.push(std::move(info));
                }
            }
		} catch (tbb::user_abort&) {
			return;
		} catch (...) {
			CASPAR_LOG_CURRENT_EXCEPTION();
		}
	}

	int64_t to_frames(int64_t pts)
	{
		return av_rescale_q(pts, AVRational{ 1, AV_TIME_BASE }, AVRational{ format_desc_.duration, format_desc_.time_scale });
	}

	int64_t from_frames(int64_t frames)
	{
		return av_rescale_q(frames, AVRational{ format_desc_.duration, format_desc_.time_scale }, AVRational{ 1, AV_TIME_BASE });
	}

    std::wstring call_internal(const std::vector<std::wstring>& params)
    {
        std::wstring result;

        std::wstring cmd = params.at(0);
        std::wstring value;
        if (params.size() > 1) {
            value = params.at(1);
        }

        if (boost::iequals(cmd, L"loop")) {
            if (!value.empty()) {
                producer_.loop(boost::lexical_cast<bool>(value));
            }

            result = boost::lexical_cast<std::wstring>(producer_.loop());
        } else if (boost::iequals(cmd, L"in") || boost::iequals(cmd, L"start")) {
            if (!value.empty()) {
                producer_.start(from_frames(boost::lexical_cast<std::int64_t>(value)));
            }

            result = boost::lexical_cast<std::wstring>(to_frames(producer_.start()));
        } else if (boost::iequals(cmd, L"out")) {
            if (!value.empty()) {
                producer_.duration(from_frames(boost::lexical_cast<std::int64_t>(value)) - producer_.start());
            }

            result = boost::lexical_cast<std::wstring>(to_frames(producer_.start() + producer_.duration()));
        } else if (boost::iequals(cmd, L"length")) {
            if (!value.empty()) {
                producer_.duration(from_frames(boost::lexical_cast<std::int64_t>(value)));
            }

            result = boost::lexical_cast<std::wstring>(to_frames(producer_.duration()));
        } else if (boost::iequals(cmd, L"seek") && !value.empty()) {
            int64_t seek;
            if (boost::iequals(value, L"rel")) {
                seek = from_frames(info_.number);
            } else if (boost::iequals(value, L"in")) {
                seek = producer_.start();
            } else if (boost::iequals(value, L"out")) {
                seek = producer_.start() + producer_.duration();
            } else if (boost::iequals(value, L"end")) {
                seek = producer_.duration();
            } else {
                seek = from_frames(boost::lexical_cast<std::int64_t>(value));
            }

            if (params.size() > 2) {
                seek += from_frames(boost::lexical_cast<std::int64_t>(params.at(2)));
            }

            seek_ = seek;
            while (buffer_.try_pop(info_))
                ;

            {
                std::lock_guard<std::mutex> info_lock(info_mutex_);

                info_.number = to_frames(seek);
                info_.frame = core::draw_frame::late();
            }

            result = boost::lexical_cast<std::wstring>(info_.number);
        } else {
            CASPAR_THROW_EXCEPTION(invalid_argument());
        }

        return result;
    }
 
	// frame_producer

    core::draw_frame last_frame() override
    {
        std::lock_guard<std::mutex> lock(info_mutex_);

        if (info_.frame == core::draw_frame::late()) {
            buffer_.try_pop(info_);
        }

        return info_.frame;
    }

	core::draw_frame receive_impl() override
	{
        Info info;
        if (buffer_.try_pop(info)) {
            graph_->set_value("buffer-count", static_cast<double>(buffer_.size()) / static_cast<double>(buffer_.capacity()));

            {
                std::lock_guard<std::mutex> lock(info_mutex_);
                info_ = info;
            }

            monitor_subject_
                << core::monitor::message("/file/time") % (info.number / format_desc_.fps) % (info.count / format_desc_.fps)
                << core::monitor::message("/file/frame") % static_cast<int32_t>(info.number) % static_cast<int32_t>(info.count)
                << core::monitor::message("/file/fps") % format_desc_.fps
                << core::monitor::message("/file/path") % path_relative_to_media_
                << core::monitor::message("/loop") % info.loop;
        } else {
            graph_->set_tag(diagnostics::tag_severity::WARNING, "underflow");
        }

		graph_->set_text(print());
		
		return info.frame;
	}
	
	core::constraints& pixel_constraints() override
	{
		return constraints_;
	}

	uint32_t nb_frames() const override
	{
        std::lock_guard<std::mutex> lock(info_mutex_);

		return info_.loop ? std::numeric_limits<std::uint32_t>::max() : static_cast<uint32_t>(info_.number);
	}

	std::future<std::wstring> call(const std::vector<std::wstring>& params) override
	{
        auto task = std::make_shared<task_t>([=]() -> std::wstring { return call_internal(params); });
        tasks_.push(task);
        return task->get_future();
	}

	boost::property_tree::wptree info() const override
	{
        std::lock_guard<std::mutex> lock(info_mutex_);

		boost::property_tree::wptree info;
		info.add(L"type", L"ffmpeg-producer");
		info.add(L"filename", filename_);
		info.add(L"width", info_.width);
		info.add(L"height", info_.height);
		info.add(L"progressive", format_desc_.field_mode == core::field_mode::progressive);
		info.add(L"fps", format_desc_.fps);
		info.add(L"loop", info_.loop);
		info.add(L"file-frame-number", info_.number);
		info.add(L"file-nb-frames", info_.count);
		return info;
	}

	std::wstring print() const override
	{
        std::lock_guard<std::mutex> lock(info_mutex_);

		return L"ffmpeg[" + 
			filename_ + L"|" + 
			boost::lexical_cast<std::wstring>(info_.number) + L"/" + 
			boost::lexical_cast<std::wstring>(info_.count) + 
			L"]";
	}

	std::wstring name() const override
	{
		return L"ffmpeg";
	}

	core::monitor::subject& monitor_output()
	{
		return monitor_subject_;
	}
};

spl::shared_ptr<core::frame_producer> create_producer(
		const core::frame_producer_dependencies& dependencies,
		const std::vector<std::wstring>& params,
		const spl::shared_ptr<core::media_info_repository>& info_repo)
{
	auto file_or_url = params.at(0);

	if (!boost::contains(file_or_url, L"://")) {
		file_or_url = ffmpeg::probe_stem(env::media_folder() + L"/" + file_or_url, false);
	}

	if (file_or_url.empty()) {
		return core::frame_producer::empty();
	}

	auto loop = contains_param(L"LOOP", params);

	auto in = get_param(L"SEEK", params, static_cast<uint32_t>(0)); // compatibility
	in = get_param(L"IN", params, in);

	auto out = get_param(L"LENGTH", params, std::numeric_limits<uint32_t>::max());
	if (out < std::numeric_limits<uint32_t>::max() - in)
		out += in;
	else
		out = std::numeric_limits<uint32_t>::max();
	out = get_param(L"OUT", params, out);

	auto filter_str = get_param(L"FILTER", params, L"");
	auto custom_channel_order = get_param(L"CHANNEL_LAYOUT", params, L"");

	boost::ireplace_all(filter_str, L"DEINTERLACE_BOB", L"YADIF=1:-1");
	boost::ireplace_all(filter_str, L"DEINTERLACE_LQ", L"SEPARATEFIELDS");
	boost::ireplace_all(filter_str, L"DEINTERLACE", L"YADIF=0:-1");

	ffmpeg_options vid_params;
	bool haveFFMPEGStartIndicator = false;
	for (size_t i = 0; i < params.size() - 1; ++i)
	{
		if (!haveFFMPEGStartIndicator && params[i] == L"--") {
			haveFFMPEGStartIndicator = true;
			continue;
		} if (haveFFMPEGStartIndicator)
		{
			auto name = u8(params.at(i++)).substr(1);
			auto value = u8(params.at(i));
			vid_params.push_back(std::make_pair(name, value));
		}
	}

	const auto in_tb = AVRational{ dependencies.format_desc.duration,  dependencies.format_desc.time_scale };
	const auto out_tb = AVRational{ 1, AV_TIME_BASE };

	boost::optional<std::int64_t> start;
	boost::optional<std::int64_t> duration;

	if (in != 0) {
		start = av_rescale_q(static_cast<int64_t>(in), in_tb, out_tb);
	}

	if (out != std::numeric_limits<uint32_t>::max()) {
		duration = av_rescale_q(static_cast<int64_t>(out - in), in_tb, out_tb);
	}

	// TODO
	custom_channel_order;
	vid_params;

	auto vfilter = get_param(L"VF", params, filter_str);
	auto afilter = get_param(L"AF", params, get_param(L"FILTER", params, L""));

	auto producer = spl::make_shared<ffmpeg_producer>(
		dependencies.frame_factory,
		dependencies.format_desc,
		file_or_url,
		vfilter,
		afilter,
		start,
		duration,
		loop);

	return core::create_destroy_proxy(std::move(producer));
}

}}
